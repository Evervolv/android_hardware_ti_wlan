/*
 * mac80211 - channel management
 */

#include <linux/nl80211.h>
#include <linux/export.h>
#include <linux/rtnetlink.h>
#include <net/cfg80211.h>
#include "ieee80211_i.h"
#include "driver-ops.h"

static int ieee80211_chanctx_num_assigned(struct ieee80211_local *local,
					  struct ieee80211_chanctx *ctx)
{
	struct ieee80211_sub_if_data *sdata;
	int num = 0;

	lockdep_assert_held(&local->chanctx_mtx);

	list_for_each_entry(sdata, &ctx->assigned_vifs, assigned_chanctx_list)
		num++;

	return num;
}

static int ieee80211_chanctx_num_reserved(struct ieee80211_local *local,
					  struct ieee80211_chanctx *ctx)
{
	struct ieee80211_sub_if_data *sdata;
	int num = 0;

	lockdep_assert_held(&local->chanctx_mtx);

	list_for_each_entry(sdata, &ctx->reserved_vifs, reserved_chanctx_list)
		num++;

	return num;
}

int ieee80211_chanctx_refcount(struct ieee80211_local *local,
			       struct ieee80211_chanctx *ctx)
{
	return ieee80211_chanctx_num_assigned(local, ctx) +
	       ieee80211_chanctx_num_reserved(local, ctx);
}

static int ieee80211_num_chanctx(struct ieee80211_local *local)
{
	struct ieee80211_chanctx *ctx;
	int num = 0;

	lockdep_assert_held(&local->chanctx_mtx);

	list_for_each_entry(ctx, &local->chanctx_list, list)
		num++;

	return num;
}

static bool ieee80211_can_create_new_chanctx(struct ieee80211_local *local)
{
	lockdep_assert_held(&local->chanctx_mtx);
	return ieee80211_num_chanctx(local) < ieee80211_max_num_channels(local);
}

static const struct cfg80211_chan_def *
ieee80211_chanctx_reserved_chandef(struct ieee80211_local *local,
				   struct ieee80211_chanctx *ctx,
				   const struct cfg80211_chan_def *compat)
{
	struct ieee80211_sub_if_data *sdata;

	lockdep_assert_held(&local->chanctx_mtx);

	list_for_each_entry(sdata, &ctx->reserved_vifs,
			    reserved_chanctx_list) {
		if (!compat)
			compat = &sdata->reserved_chandef;

		compat = cfg80211_chandef_compatible(&sdata->reserved_chandef,
						     compat);
		if (!compat)
			break;
	}

	return compat;
}

static const struct cfg80211_chan_def *
ieee80211_chanctx_non_reserved_chandef(struct ieee80211_local *local,
				       struct ieee80211_chanctx *ctx,
				       const struct cfg80211_chan_def *compat)
{
	struct ieee80211_sub_if_data *sdata;

	lockdep_assert_held(&local->chanctx_mtx);

	list_for_each_entry(sdata, &ctx->assigned_vifs,
			    assigned_chanctx_list) {
		if (sdata->reserved_chanctx != NULL)
			continue;

		if (!compat)
			compat = &sdata->vif.bss_conf.chandef;

		compat = cfg80211_chandef_compatible(
				&sdata->vif.bss_conf.chandef, compat);
		if (!compat)
			break;
	}

	return compat;
}

static const struct cfg80211_chan_def *
ieee80211_chanctx_combined_chandef(struct ieee80211_local *local,
				   struct ieee80211_chanctx *ctx,
				   const struct cfg80211_chan_def *compat)
{
	lockdep_assert_held(&local->chanctx_mtx);

	compat = ieee80211_chanctx_reserved_chandef(local, ctx, compat);
	if (!compat)
		return NULL;

	compat = ieee80211_chanctx_non_reserved_chandef(local, ctx, compat);
	if (!compat)
		return NULL;

	return compat;
}

static bool
ieee80211_chanctx_can_reserve_chandef(struct ieee80211_local *local,
				      struct ieee80211_chanctx *ctx,
				      const struct cfg80211_chan_def *def)
{
	lockdep_assert_held(&local->chanctx_mtx);

	if (ieee80211_chanctx_combined_chandef(local, ctx, def))
		return true;

	if (!list_empty(&ctx->reserved_vifs) &&
	    ieee80211_chanctx_reserved_chandef(local, ctx, def))
		return true;

	return false;
}

static struct ieee80211_chanctx *
ieee80211_find_reservation_chanctx(struct ieee80211_local *local,
				   const struct cfg80211_chan_def *chandef,
				   enum ieee80211_chanctx_mode mode)
{
	struct ieee80211_chanctx *ctx;

	lockdep_assert_held(&local->chanctx_mtx);

	if (mode == IEEE80211_CHANCTX_EXCLUSIVE)
		return NULL;

	list_for_each_entry(ctx, &local->chanctx_list, list) {
		if (ctx->mode == IEEE80211_CHANCTX_EXCLUSIVE)
			continue;

		if (!ieee80211_chanctx_can_reserve_chandef(local, ctx,
							   chandef))
			continue;

		return ctx;
	}

	return NULL;
}

static bool
ieee80211_chanctx_all_reserved_vifs_ready(struct ieee80211_local *local,
					  struct ieee80211_chanctx *ctx)
{
	struct ieee80211_sub_if_data *sdata;

	lockdep_assert_held(&local->chanctx_mtx);

	list_for_each_entry(sdata, &ctx->reserved_vifs, reserved_chanctx_list) {
		if (!sdata->reserved_chanctx)
			continue;
		if (!sdata->reserved_ready)
			return false;
	}

	return true;
}

static enum nl80211_chan_width ieee80211_get_sta_bw(struct ieee80211_sta *sta)
{
	switch (sta->bandwidth) {
	case IEEE80211_STA_RX_BW_20:
		if (sta->ht_cap.ht_supported)
			return NL80211_CHAN_WIDTH_20;
		else
			return NL80211_CHAN_WIDTH_20_NOHT;
	case IEEE80211_STA_RX_BW_40:
		return NL80211_CHAN_WIDTH_40;
	case IEEE80211_STA_RX_BW_80:
		return NL80211_CHAN_WIDTH_80;
	case IEEE80211_STA_RX_BW_160:
		/*
		 * This applied for both 160 and 80+80. since we use
		 * the returned value to consider degradation of
		 * ctx->conf.min_def, we have to make sure to take
		 * the bigger one (NL80211_CHAN_WIDTH_160).
		 * Otherwise we might try degrading even when not
		 * needed, as the max required sta_bw returned (80+80)
		 * might be smaller than the configured bw (160).
		 */
		return NL80211_CHAN_WIDTH_160;
	default:
		WARN_ON(1);
		return NL80211_CHAN_WIDTH_20;
	}
}

static enum nl80211_chan_width
ieee80211_get_max_required_bw(struct ieee80211_sub_if_data *sdata)
{
	enum nl80211_chan_width max_bw = NL80211_CHAN_WIDTH_20_NOHT;
	struct sta_info *sta;

	rcu_read_lock();
	list_for_each_entry_rcu(sta, &sdata->local->sta_list, list) {
		if (sdata != sta->sdata &&
		    !(sta->sdata->bss && sta->sdata->bss == sdata->bss))
			continue;

		if (!sta->uploaded)
			continue;

		max_bw = max(max_bw, ieee80211_get_sta_bw(&sta->sta));
	}
	rcu_read_unlock();

	return max_bw;
}

static enum nl80211_chan_width
ieee80211_get_chanctx_max_required_bw(struct ieee80211_local *local,
				      struct ieee80211_chanctx_conf *conf)
{
	struct ieee80211_sub_if_data *sdata;
	enum nl80211_chan_width max_bw = NL80211_CHAN_WIDTH_20_NOHT;

	rcu_read_lock();
	list_for_each_entry_rcu(sdata, &local->interfaces, list) {
		struct ieee80211_vif *vif = &sdata->vif;
		enum nl80211_chan_width width = NL80211_CHAN_WIDTH_20_NOHT;

		if (!ieee80211_sdata_running(sdata))
			continue;

		if (rcu_access_pointer(sdata->vif.chanctx_conf) != conf)
			continue;

		switch (vif->type) {
		case NL80211_IFTYPE_AP:
		case NL80211_IFTYPE_AP_VLAN:
			width = ieee80211_get_max_required_bw(sdata);
			break;
		case NL80211_IFTYPE_P2P_DEVICE:
			continue;
		case NL80211_IFTYPE_STATION:
		case NL80211_IFTYPE_ADHOC:
		case NL80211_IFTYPE_WDS:
		case NL80211_IFTYPE_MESH_POINT:
			width = vif->bss_conf.chandef.width;
			break;
		case NL80211_IFTYPE_UNSPECIFIED:
		case NUM_NL80211_IFTYPES:
		case NL80211_IFTYPE_MONITOR:
		case NL80211_IFTYPE_P2P_CLIENT:
		case NL80211_IFTYPE_P2P_GO:
			WARN_ON_ONCE(1);
		}
		max_bw = max(max_bw, width);
	}
	rcu_read_unlock();

	return max_bw;
}

/*
 * recalc the min required chan width of the channel context, which is
 * the max of min required widths of all the interfaces bound to this
 * channel context.
 */
void ieee80211_recalc_chanctx_min_def(struct ieee80211_local *local,
				      struct ieee80211_chanctx *ctx)
{
	enum nl80211_chan_width max_bw;
	struct cfg80211_chan_def min_def;

	lockdep_assert_held(&local->chanctx_mtx);

	/* don't optimize 5MHz, 10MHz, and radar_enabled confs */
	if (ctx->conf.def.width == NL80211_CHAN_WIDTH_5 ||
	    ctx->conf.def.width == NL80211_CHAN_WIDTH_10 ||
	    ctx->conf.radar_enabled) {
		ctx->conf.min_def = ctx->conf.def;
		return;
	}

	max_bw = ieee80211_get_chanctx_max_required_bw(local, &ctx->conf);

	/* downgrade chandef up to max_bw */
	min_def = ctx->conf.def;
	while (min_def.width > max_bw)
		ieee80211_chandef_downgrade(&min_def);

	if (cfg80211_chandef_identical(&ctx->conf.min_def, &min_def))
		return;

	ctx->conf.min_def = min_def;
	if (!ctx->driver_present)
		return;

	drv_change_chanctx(local, ctx, IEEE80211_CHANCTX_CHANGE_MIN_WIDTH);
}

static void ieee80211_change_chanctx(struct ieee80211_local *local,
				     struct ieee80211_chanctx *ctx,
				     const struct cfg80211_chan_def *chandef)
{
	if (cfg80211_chandef_identical(&ctx->conf.def, chandef))
		return;

	WARN_ON(!cfg80211_chandef_compatible(&ctx->conf.def, chandef));

	ctx->conf.def = *chandef;
	drv_change_chanctx(local, ctx, IEEE80211_CHANCTX_CHANGE_WIDTH);
	ieee80211_recalc_chanctx_min_def(local, ctx);

	if (!local->use_chanctx) {
		local->_oper_chandef = *chandef;
		ieee80211_hw_config(local, 0);
	}
}

static struct ieee80211_chanctx *
ieee80211_find_chanctx(struct ieee80211_local *local,
		       const struct cfg80211_chan_def *chandef,
		       enum ieee80211_chanctx_mode mode)
{
	struct ieee80211_chanctx *ctx;

	lockdep_assert_held(&local->chanctx_mtx);

	if (mode == IEEE80211_CHANCTX_EXCLUSIVE)
		return NULL;

	list_for_each_entry(ctx, &local->chanctx_list, list) {
		const struct cfg80211_chan_def *compat;

		if (ctx->mode == IEEE80211_CHANCTX_EXCLUSIVE)
			continue;

		compat = cfg80211_chandef_compatible(&ctx->conf.def, chandef);
		if (!compat)
			continue;

		compat = ieee80211_chanctx_reserved_chandef(local, ctx,
							    compat);
		if (!compat)
			continue;

		ieee80211_change_chanctx(local, ctx, compat);

		return ctx;
	}

	return NULL;
}

static bool ieee80211_is_radar_required(struct ieee80211_local *local,
					struct ieee80211_chanctx *ctx)
{
	struct ieee80211_chanctx_conf *conf = &ctx->conf;
	struct ieee80211_sub_if_data *sdata;
	bool required = false;

	lockdep_assert_held(&local->chanctx_mtx);
	lockdep_assert_held(&local->mtx);

	rcu_read_lock();
	list_for_each_entry_rcu(sdata, &local->interfaces, list) {
		if (!ieee80211_sdata_running(sdata))
			continue;
		if (rcu_access_pointer(sdata->vif.chanctx_conf) != conf)
			continue;
		if (!sdata->radar_required)
			continue;

		required = true;
	}
	rcu_read_unlock();

	return required;
}

static struct ieee80211_chanctx *
ieee80211_alloc_chanctx(struct ieee80211_local *local,
			const struct cfg80211_chan_def *chandef,
			enum ieee80211_chanctx_mode mode)
{
	struct ieee80211_chanctx *ctx;

	lockdep_assert_held(&local->chanctx_mtx);

	ctx = kzalloc(sizeof(*ctx) + local->hw.chanctx_data_size, GFP_KERNEL);
	if (!ctx)
		return NULL;

	INIT_LIST_HEAD(&ctx->assigned_vifs);
	INIT_LIST_HEAD(&ctx->reserved_vifs);
	ctx->conf.def = *chandef;
	ctx->conf.rx_chains_static = 1;
	ctx->conf.rx_chains_dynamic = 1;
	ctx->mode = mode;
	ctx->conf.radar_enabled = ieee80211_is_radar_required(local, ctx);
	ieee80211_recalc_chanctx_min_def(local, ctx);

	return ctx;
}

static int ieee80211_add_chanctx(struct ieee80211_local *local,
				 struct ieee80211_chanctx *ctx)
{
	u32 changed;
	int err;

	lockdep_assert_held(&local->mtx);
	lockdep_assert_held(&local->chanctx_mtx);

	if (!local->use_chanctx)
		local->hw.conf.radar_enabled = ctx->conf.radar_enabled;

	/* turn idle off *before* setting channel -- some drivers need that */
	changed = ieee80211_idle_off(local);
	if (changed)
		ieee80211_hw_config(local, changed);

	if (!local->use_chanctx) {
		local->_oper_chandef = ctx->conf.def;
		ieee80211_hw_config(local, 0);
	} else {
		err = drv_add_chanctx(local, ctx);
		if (err) {
			ieee80211_recalc_idle(local);
			return err;
		}
	}

	return 0;
}

static struct ieee80211_chanctx *
ieee80211_new_chanctx(struct ieee80211_local *local,
		      const struct cfg80211_chan_def *chandef,
		      enum ieee80211_chanctx_mode mode)
{
	struct ieee80211_chanctx *ctx;
	int err;

	lockdep_assert_held(&local->mtx);
	lockdep_assert_held(&local->chanctx_mtx);

	ctx = ieee80211_alloc_chanctx(local, chandef, mode);
	if (!ctx)
		return ERR_PTR(-ENOMEM);

	err = ieee80211_add_chanctx(local, ctx);
	if (err) {
		kfree(ctx);
		return ERR_PTR(err);
	}

	list_add_rcu(&ctx->list, &local->chanctx_list);
	return ctx;
}

static void ieee80211_del_chanctx(struct ieee80211_local *local,
				  struct ieee80211_chanctx *ctx)
{
	lockdep_assert_held(&local->chanctx_mtx);

	if (!local->use_chanctx) {
		struct cfg80211_chan_def *chandef = &local->_oper_chandef;
		chandef->width = NL80211_CHAN_WIDTH_20_NOHT;
		chandef->center_freq1 = chandef->chan->center_freq;
		chandef->center_freq2 = 0;

		/* NOTE: Disabling radar is only valid here for
		 * single channel context. To be sure, check it ...
		 */
		WARN_ON(local->hw.conf.radar_enabled &&
			!list_empty(&local->chanctx_list));

		local->hw.conf.radar_enabled = false;

		ieee80211_hw_config(local, 0);
	} else {
		drv_remove_chanctx(local, ctx);
	}

	ieee80211_recalc_idle(local);
}

static void ieee80211_free_chanctx(struct ieee80211_local *local,
				   struct ieee80211_chanctx *ctx)
{
	lockdep_assert_held(&local->chanctx_mtx);

	WARN_ON_ONCE(ieee80211_chanctx_refcount(local, ctx) != 0);

	list_del_rcu(&ctx->list);
	ieee80211_del_chanctx(local, ctx);
	kfree_rcu(ctx, rcu_head);
}

static void ieee80211_recalc_chanctx_chantype(struct ieee80211_local *local,
					      struct ieee80211_chanctx *ctx)
{
	struct ieee80211_chanctx_conf *conf = &ctx->conf;
	struct ieee80211_sub_if_data *sdata;
	const struct cfg80211_chan_def *compat = NULL;

	lockdep_assert_held(&local->chanctx_mtx);

	rcu_read_lock();
	list_for_each_entry_rcu(sdata, &local->interfaces, list) {

		if (!ieee80211_sdata_running(sdata))
			continue;
		if (rcu_access_pointer(sdata->vif.chanctx_conf) != conf)
			continue;

		if (!compat)
			compat = &sdata->vif.bss_conf.chandef;

		compat = cfg80211_chandef_compatible(
				&sdata->vif.bss_conf.chandef, compat);
		if (!compat)
			break;
	}
	rcu_read_unlock();

	if (WARN_ON_ONCE(!compat))
		return;

	ieee80211_change_chanctx(local, ctx, compat);
}

static void ieee80211_recalc_radar_chanctx(struct ieee80211_local *local,
					   struct ieee80211_chanctx *chanctx)
{
	bool radar_enabled;

	lockdep_assert_held(&local->chanctx_mtx);
	/* for setting local->radar_detect_enabled */
	lockdep_assert_held(&local->mtx);

	radar_enabled = ieee80211_is_radar_required(local, chanctx);

	if (radar_enabled == chanctx->conf.radar_enabled)
		return;

	chanctx->conf.radar_enabled = radar_enabled;
	local->radar_detect_enabled = chanctx->conf.radar_enabled;

	if (!local->use_chanctx) {
		local->hw.conf.radar_enabled = chanctx->conf.radar_enabled;
		ieee80211_hw_config(local, IEEE80211_CONF_CHANGE_CHANNEL);
	}

	drv_change_chanctx(local, chanctx, IEEE80211_CHANCTX_CHANGE_RADAR);
}

static int ieee80211_assign_vif_chanctx(struct ieee80211_sub_if_data *sdata,
					struct ieee80211_chanctx *new_ctx)
{
	struct ieee80211_local *local = sdata->local;
	struct ieee80211_chanctx_conf *conf;
	struct ieee80211_chanctx *curr_ctx = NULL;
	int ret = 0;

	conf = rcu_dereference_protected(sdata->vif.chanctx_conf,
					 lockdep_is_held(&local->chanctx_mtx));

	if (conf) {
		curr_ctx = container_of(conf, struct ieee80211_chanctx, conf);

		drv_unassign_vif_chanctx(local, sdata, curr_ctx);
		conf = NULL;
		list_del(&sdata->assigned_chanctx_list);
	}

	if (new_ctx) {
		ret = drv_assign_vif_chanctx(local, sdata, new_ctx);
		if (ret)
			goto out;

		conf = &new_ctx->conf;
		list_add(&sdata->assigned_chanctx_list,
			 &new_ctx->assigned_vifs);
	}

out:
	rcu_assign_pointer(sdata->vif.chanctx_conf, conf);

	sdata->vif.bss_conf.idle = !conf;

	if (curr_ctx && ieee80211_chanctx_num_assigned(local, curr_ctx) > 0) {
		ieee80211_recalc_chanctx_chantype(local, curr_ctx);
		ieee80211_recalc_smps_chanctx(local, curr_ctx);
		ieee80211_recalc_radar_chanctx(local, curr_ctx);
		ieee80211_recalc_chanctx_min_def(local, curr_ctx);
	}

	if (new_ctx && ieee80211_chanctx_num_assigned(local, new_ctx) > 0) {
		ieee80211_recalc_txpower(sdata);
		ieee80211_recalc_chanctx_min_def(local, new_ctx);
	}

	if (sdata->vif.type != NL80211_IFTYPE_P2P_DEVICE &&
	    sdata->vif.type != NL80211_IFTYPE_MONITOR)
		ieee80211_bss_info_change_notify(sdata,
						 BSS_CHANGED_IDLE);

	return ret;
}

static void __ieee80211_vif_release_channel(struct ieee80211_sub_if_data *sdata)
{
	struct ieee80211_local *local = sdata->local;
	struct ieee80211_chanctx_conf *conf;
	struct ieee80211_chanctx *ctx;

	lockdep_assert_held(&local->chanctx_mtx);

	conf = rcu_dereference_protected(sdata->vif.chanctx_conf,
					 lockdep_is_held(&local->chanctx_mtx));
	if (!conf)
		return;

	ctx = container_of(conf, struct ieee80211_chanctx, conf);

	if (sdata->reserved_chanctx)
		ieee80211_vif_unreserve_chanctx(sdata);

	ieee80211_assign_vif_chanctx(sdata, NULL);
	if (ieee80211_chanctx_refcount(local, ctx) == 0)
		ieee80211_free_chanctx(local, ctx);
}

void ieee80211_recalc_smps_chanctx(struct ieee80211_local *local,
				   struct ieee80211_chanctx *chanctx)
{
	struct ieee80211_sub_if_data *sdata;
	u8 rx_chains_static, rx_chains_dynamic;

	lockdep_assert_held(&local->chanctx_mtx);

	rx_chains_static = 1;
	rx_chains_dynamic = 1;

	rcu_read_lock();
	list_for_each_entry_rcu(sdata, &local->interfaces, list) {
		u8 needed_static, needed_dynamic;

		if (!ieee80211_sdata_running(sdata))
			continue;

		if (rcu_access_pointer(sdata->vif.chanctx_conf) !=
						&chanctx->conf)
			continue;

		switch (sdata->vif.type) {
		case NL80211_IFTYPE_P2P_DEVICE:
			continue;
		case NL80211_IFTYPE_STATION:
			if (!sdata->u.mgd.associated)
				continue;
			break;
		case NL80211_IFTYPE_AP_VLAN:
			continue;
		case NL80211_IFTYPE_AP:
		case NL80211_IFTYPE_ADHOC:
		case NL80211_IFTYPE_WDS:
		case NL80211_IFTYPE_MESH_POINT:
			break;
		default:
			WARN_ON_ONCE(1);
		}

		switch (sdata->smps_mode) {
		default:
			WARN_ONCE(1, "Invalid SMPS mode %d\n",
				  sdata->smps_mode);
			/* fall through */
		case IEEE80211_SMPS_OFF:
			needed_static = sdata->needed_rx_chains;
			needed_dynamic = sdata->needed_rx_chains;
			break;
		case IEEE80211_SMPS_DYNAMIC:
			needed_static = 1;
			needed_dynamic = sdata->needed_rx_chains;
			break;
		case IEEE80211_SMPS_STATIC:
			needed_static = 1;
			needed_dynamic = 1;
			break;
		}

		rx_chains_static = max(rx_chains_static, needed_static);
		rx_chains_dynamic = max(rx_chains_dynamic, needed_dynamic);
	}
	rcu_read_unlock();

	if (!local->use_chanctx) {
		if (rx_chains_static > 1)
			local->smps_mode = IEEE80211_SMPS_OFF;
		else if (rx_chains_dynamic > 1)
			local->smps_mode = IEEE80211_SMPS_DYNAMIC;
		else
			local->smps_mode = IEEE80211_SMPS_STATIC;
		ieee80211_hw_config(local, 0);
	}

	if (rx_chains_static == chanctx->conf.rx_chains_static &&
	    rx_chains_dynamic == chanctx->conf.rx_chains_dynamic)
		return;

	chanctx->conf.rx_chains_static = rx_chains_static;
	chanctx->conf.rx_chains_dynamic = rx_chains_dynamic;
	drv_change_chanctx(local, chanctx, IEEE80211_CHANCTX_CHANGE_RX_CHAINS);
}

int ieee80211_vif_use_channel(struct ieee80211_sub_if_data *sdata,
			      const struct cfg80211_chan_def *chandef,
			      enum ieee80211_chanctx_mode mode)
{
	struct ieee80211_local *local = sdata->local;
	struct ieee80211_chanctx *ctx;
	u8 radar_detect_width = 0;
	int ret;

	lockdep_assert_held(&local->mtx);

	WARN_ON(sdata->dev && netif_carrier_ok(sdata->dev));

	mutex_lock(&local->chanctx_mtx);

	ret = cfg80211_chandef_dfs_required(local->hw.wiphy,
					    chandef,
					    sdata->wdev.iftype);
	if (ret < 0)
		goto out;
	if (ret > 0)
		radar_detect_width = BIT(chandef->width);

	sdata->radar_required = ret;

	ret = ieee80211_check_combinations(sdata, chandef, mode,
					   radar_detect_width);
	if (ret < 0)
		goto out;

	__ieee80211_vif_release_channel(sdata);

	ctx = ieee80211_find_chanctx(local, chandef, mode);
	if (!ctx)
		ctx = ieee80211_new_chanctx(local, chandef, mode);
	if (IS_ERR(ctx)) {
		ret = PTR_ERR(ctx);
		goto out;
	}

	sdata->vif.bss_conf.chandef = *chandef;

	ret = ieee80211_assign_vif_chanctx(sdata, ctx);
	if (ret) {
		/* if assign fails refcount stays the same */
		if (ieee80211_chanctx_refcount(local, ctx) == 0)
			ieee80211_free_chanctx(local, ctx);
		goto out;
	}

	ieee80211_recalc_smps_chanctx(local, ctx);
	ieee80211_recalc_radar_chanctx(local, ctx);
 out:
	mutex_unlock(&local->chanctx_mtx);
	return ret;
}

static void
__ieee80211_vif_copy_chanctx_to_vlans(struct ieee80211_sub_if_data *sdata,
				      bool clear)
{
	struct ieee80211_local *local = sdata->local;
	struct ieee80211_sub_if_data *vlan;
	struct ieee80211_chanctx_conf *conf;

	if (WARN_ON(sdata->vif.type != NL80211_IFTYPE_AP))
		return;

	lockdep_assert_held(&local->mtx);

	/* Check that conf exists, even when clearing this function
	 * must be called with the AP's channel context still there
	 * as it would otherwise cause VLANs to have an invalid
	 * channel context pointer for a while, possibly pointing
	 * to a channel context that has already been freed.
	 */
	conf = rcu_dereference_protected(sdata->vif.chanctx_conf,
				lockdep_is_held(&local->chanctx_mtx));
	WARN_ON(!conf);

	if (clear)
		conf = NULL;

	list_for_each_entry(vlan, &sdata->u.ap.vlans, u.vlan.list)
		rcu_assign_pointer(vlan->vif.chanctx_conf, conf);
}

void ieee80211_vif_copy_chanctx_to_vlans(struct ieee80211_sub_if_data *sdata,
					 bool clear)
{
	struct ieee80211_local *local = sdata->local;

	mutex_lock(&local->chanctx_mtx);

	__ieee80211_vif_copy_chanctx_to_vlans(sdata, clear);

	mutex_unlock(&local->chanctx_mtx);
}

int ieee80211_vif_unreserve_chanctx(struct ieee80211_sub_if_data *sdata)
{
	struct ieee80211_chanctx *ctx = sdata->reserved_chanctx;

	lockdep_assert_held(&sdata->local->chanctx_mtx);

	if (WARN_ON(!ctx))
		return -EINVAL;

	list_del(&sdata->reserved_chanctx_list);
	sdata->reserved_chanctx = NULL;

	if (ieee80211_chanctx_refcount(sdata->local, ctx) == 0)
		ieee80211_free_chanctx(sdata->local, ctx);

	return 0;
}

int ieee80211_vif_reserve_chanctx(struct ieee80211_sub_if_data *sdata,
				  const struct cfg80211_chan_def *chandef,
				  enum ieee80211_chanctx_mode mode,
				  bool radar_required)
{
	struct ieee80211_local *local = sdata->local;
	struct ieee80211_chanctx_conf *conf;
	struct ieee80211_chanctx *new_ctx, *curr_ctx;

	lockdep_assert_held(&local->chanctx_mtx);

	conf = rcu_dereference_protected(sdata->vif.chanctx_conf,
					 lockdep_is_held(&local->chanctx_mtx));
	if (!conf)
		return -EINVAL;

	curr_ctx = container_of(conf, struct ieee80211_chanctx, conf);

	new_ctx = ieee80211_find_reservation_chanctx(local, chandef, mode);
	if (!new_ctx) {
		if (ieee80211_can_create_new_chanctx(local)) {
			new_ctx = ieee80211_new_chanctx(local, chandef, mode);
			if (IS_ERR(new_ctx))
				return PTR_ERR(new_ctx);
		} else {
			new_ctx = curr_ctx;
		}
	}

	list_add(&sdata->reserved_chanctx_list, &new_ctx->reserved_vifs);
	sdata->reserved_chanctx = new_ctx;
	sdata->reserved_chandef = *chandef;
	sdata->reserved_radar_required = radar_required;
	sdata->reserved_ready = false;

	return 0;
}

static void
ieee80211_vif_chanctx_reservation_complete(struct ieee80211_sub_if_data *sdata)
{
	switch (sdata->vif.type) {
	case NL80211_IFTYPE_AP:
	case NL80211_IFTYPE_ADHOC:
	case NL80211_IFTYPE_MESH_POINT:
		ieee80211_queue_work(&sdata->local->hw,
				     &sdata->csa_finalize_work);
		break;
	case NL80211_IFTYPE_STATION:
		ieee80211_queue_work(&sdata->local->hw,
				     &sdata->u.mgd.chswitch_work);
		break;
	default:
		break;
	}
}

static int
ieee80211_vif_use_reserved_incompat(struct ieee80211_local *local,
				    struct ieee80211_chanctx *ctx,
				    const struct cfg80211_chan_def *chandef)
{
	struct ieee80211_sub_if_data *sdata, *tmp;
	struct ieee80211_chanctx *new_ctx;
	u32 changed = 0;
	int err;

	lockdep_assert_held(&local->mtx);
	lockdep_assert_held(&local->chanctx_mtx);

	if (!ieee80211_chanctx_all_reserved_vifs_ready(local, ctx))
		return 0;

	if (ieee80211_chanctx_num_assigned(local, ctx) !=
	    ieee80211_chanctx_num_reserved(local, ctx)) {
		wiphy_info(local->hw.wiphy,
			   "channel context reservation cannot be finalized because some interfaces aren't switching\n");
		err = -EBUSY;
		goto err;
	}

	new_ctx = ieee80211_alloc_chanctx(local, chandef, ctx->mode);
	if (!new_ctx) {
		err = -ENOMEM;
		goto err;
	}

	list_for_each_entry(sdata, &ctx->reserved_vifs, reserved_chanctx_list) {
		drv_unassign_vif_chanctx(local, sdata, ctx);
		rcu_assign_pointer(sdata->vif.chanctx_conf, &new_ctx->conf);
	}

	list_del_rcu(&ctx->list);
	ieee80211_del_chanctx(local, ctx);

	/* don't simply overwrite radar_required in case of failure */
	list_for_each_entry(sdata, &ctx->reserved_vifs, reserved_chanctx_list) {
		bool tmp = sdata->radar_required;
		sdata->radar_required = sdata->reserved_radar_required;
		sdata->reserved_radar_required = tmp;
	}

	err = ieee80211_add_chanctx(local, new_ctx);
	if (err)
		goto err_revert;

	ieee80211_recalc_radar_chanctx(local, new_ctx);

	list_for_each_entry(sdata, &ctx->reserved_vifs, reserved_chanctx_list) {
		err = drv_assign_vif_chanctx(local, sdata, new_ctx);
		if (err)
			goto err_unassign;
	}

	if (sdata->vif.type == NL80211_IFTYPE_AP)
		__ieee80211_vif_copy_chanctx_to_vlans(sdata, false);

	list_add_rcu(&new_ctx->list, &local->chanctx_list);

	list_for_each_entry(sdata, &ctx->reserved_vifs,
			    reserved_chanctx_list) {
		changed = 0;
		if (sdata->vif.bss_conf.chandef.width !=
		    sdata->reserved_chandef.width)
			changed = BSS_CHANGED_BANDWIDTH;

		sdata->vif.bss_conf.chandef = sdata->reserved_chandef;
		if (changed)
			ieee80211_bss_info_change_notify(sdata, changed);

		ieee80211_recalc_txpower(sdata);
	}

	ieee80211_recalc_chanctx_chantype(local, new_ctx);
	ieee80211_recalc_smps_chanctx(local, new_ctx);
	ieee80211_recalc_radar_chanctx(local, new_ctx);
	ieee80211_recalc_chanctx_min_def(local, new_ctx);

	list_for_each_entry_safe(sdata, tmp, &ctx->reserved_vifs,
				 reserved_chanctx_list) {
		list_del(&sdata->reserved_chanctx_list);
		list_move(&sdata->assigned_chanctx_list,
			  &new_ctx->assigned_vifs);
		sdata->reserved_chanctx = NULL;

		ieee80211_vif_chanctx_reservation_complete(sdata);
	}

	kfree_rcu(ctx, rcu_head);

	return 0;

err_unassign:
	list_for_each_entry_continue_reverse(sdata, &ctx->reserved_vifs,
					     reserved_chanctx_list)
		drv_unassign_vif_chanctx(local, sdata, ctx);
	ieee80211_del_chanctx(local, new_ctx);
err_revert:
	kfree_rcu(new_ctx, rcu_head);
	WARN_ON(ieee80211_add_chanctx(local, ctx));
	list_add_rcu(&ctx->list, &local->chanctx_list);
	list_for_each_entry(sdata, &ctx->reserved_vifs, reserved_chanctx_list) {
		sdata->radar_required = sdata->reserved_radar_required;
		rcu_assign_pointer(sdata->vif.chanctx_conf, &ctx->conf);
		WARN_ON(drv_assign_vif_chanctx(local, sdata, ctx));
	}
err:
	list_for_each_entry_safe(sdata, tmp, &ctx->reserved_vifs,
				 reserved_chanctx_list) {
		list_del(&sdata->reserved_chanctx_list);
		sdata->reserved_chanctx = NULL;
		ieee80211_vif_chanctx_reservation_complete(sdata);
	}
	return err;
}

static int
ieee80211_vif_use_reserved_compat(struct ieee80211_sub_if_data *sdata,
				  struct ieee80211_chanctx *old_ctx,
				  struct ieee80211_chanctx *new_ctx)
{
	struct ieee80211_local *local = sdata->local;
	u32 changed = 0;
	int err;
	bool cur_radar = sdata->radar_required;

	lockdep_assert_held(&local->mtx);
	lockdep_assert_held(&local->chanctx_mtx);

	list_del(&sdata->reserved_chanctx_list);
	sdata->reserved_chanctx = NULL;
	sdata->radar_required = sdata->reserved_radar_required;

	err = ieee80211_assign_vif_chanctx(sdata, new_ctx);
	if (ieee80211_chanctx_refcount(local, old_ctx) == 0)
		ieee80211_free_chanctx(local, old_ctx);
	if (err) {
		/* if assign fails refcount stays the same */
		sdata->radar_required = cur_radar;
		if (ieee80211_chanctx_refcount(local, new_ctx) == 0)
			ieee80211_free_chanctx(local, new_ctx);
		goto out;
	}

	if (sdata->vif.type == NL80211_IFTYPE_AP)
		__ieee80211_vif_copy_chanctx_to_vlans(sdata, false);

	if (sdata->vif.bss_conf.chandef.width != sdata->reserved_chandef.width)
		changed = BSS_CHANGED_BANDWIDTH;

	sdata->vif.bss_conf.chandef = sdata->reserved_chandef;

	if (changed)
		ieee80211_bss_info_change_notify(sdata, changed);

out:
	ieee80211_vif_chanctx_reservation_complete(sdata);
	return err;
}

int ieee80211_vif_use_reserved_context(struct ieee80211_sub_if_data *sdata)
{
	struct ieee80211_local *local = sdata->local;
	struct ieee80211_chanctx *ctx;
	struct ieee80211_chanctx *old_ctx;
	struct ieee80211_chanctx_conf *conf;
	const struct cfg80211_chan_def *chandef;

	lockdep_assert_held(&local->mtx);
	lockdep_assert_held(&local->chanctx_mtx);

	ctx = sdata->reserved_chanctx;
	if (WARN_ON(!ctx))
		return -EINVAL;

	conf = rcu_dereference_protected(sdata->vif.chanctx_conf,
					 lockdep_is_held(&local->chanctx_mtx));
	if (!conf)
		return -EINVAL;

	old_ctx = container_of(conf, struct ieee80211_chanctx, conf);

	if (WARN_ON(sdata->reserved_ready))
		return -EINVAL;

	chandef = ieee80211_chanctx_reserved_chandef(local, ctx, NULL);
	if (WARN_ON(!chandef))
		return -EINVAL;

	sdata->reserved_ready = true;

	if (cfg80211_chandef_compatible(&ctx->conf.def, chandef))
		return ieee80211_vif_use_reserved_compat(sdata, old_ctx, ctx);
	else
		return ieee80211_vif_use_reserved_incompat(local, ctx, chandef);
}

int ieee80211_vif_change_bandwidth(struct ieee80211_sub_if_data *sdata,
				   const struct cfg80211_chan_def *chandef,
				   u32 *changed)
{
	struct ieee80211_local *local = sdata->local;
	struct ieee80211_chanctx_conf *conf;
	struct ieee80211_chanctx *ctx;
	int ret;

	if (!cfg80211_chandef_usable(sdata->local->hw.wiphy, chandef,
				     IEEE80211_CHAN_DISABLED))
		return -EINVAL;

	mutex_lock(&local->chanctx_mtx);
	if (cfg80211_chandef_identical(chandef, &sdata->vif.bss_conf.chandef)) {
		ret = 0;
		goto out;
	}

	if (chandef->width == NL80211_CHAN_WIDTH_20_NOHT ||
	    sdata->vif.bss_conf.chandef.width == NL80211_CHAN_WIDTH_20_NOHT) {
		ret = -EINVAL;
		goto out;
	}

	conf = rcu_dereference_protected(sdata->vif.chanctx_conf,
					 lockdep_is_held(&local->chanctx_mtx));
	if (!conf) {
		ret = -EINVAL;
		goto out;
	}

	ctx = container_of(conf, struct ieee80211_chanctx, conf);
	if (!cfg80211_chandef_compatible(&conf->def, chandef)) {
		ret = -EINVAL;
		goto out;
	}

	sdata->vif.bss_conf.chandef = *chandef;

	ieee80211_recalc_chanctx_chantype(local, ctx);

	*changed |= BSS_CHANGED_BANDWIDTH;
	ret = 0;
 out:
	mutex_unlock(&local->chanctx_mtx);
	return ret;
}

void ieee80211_vif_release_channel(struct ieee80211_sub_if_data *sdata)
{
	WARN_ON(sdata->dev && netif_carrier_ok(sdata->dev));

	lockdep_assert_held(&sdata->local->mtx);

	mutex_lock(&sdata->local->chanctx_mtx);
	__ieee80211_vif_release_channel(sdata);
	mutex_unlock(&sdata->local->chanctx_mtx);
}

void ieee80211_vif_vlan_copy_chanctx(struct ieee80211_sub_if_data *sdata)
{
	struct ieee80211_local *local = sdata->local;
	struct ieee80211_sub_if_data *ap;
	struct ieee80211_chanctx_conf *conf;

	if (WARN_ON(sdata->vif.type != NL80211_IFTYPE_AP_VLAN || !sdata->bss))
		return;

	ap = container_of(sdata->bss, struct ieee80211_sub_if_data, u.ap);

	mutex_lock(&local->chanctx_mtx);

	conf = rcu_dereference_protected(ap->vif.chanctx_conf,
					 lockdep_is_held(&local->chanctx_mtx));
	rcu_assign_pointer(sdata->vif.chanctx_conf, conf);
	mutex_unlock(&local->chanctx_mtx);
}

void ieee80211_iter_chan_contexts_atomic(
	struct ieee80211_hw *hw,
	void (*iter)(struct ieee80211_hw *hw,
		     struct ieee80211_chanctx_conf *chanctx_conf,
		     void *data),
	void *iter_data)
{
	struct ieee80211_local *local = hw_to_local(hw);
	struct ieee80211_chanctx *ctx;

	rcu_read_lock();
	list_for_each_entry_rcu(ctx, &local->chanctx_list, list)
		if (ctx->driver_present)
			iter(hw, &ctx->conf, iter_data);
	rcu_read_unlock();
}
EXPORT_SYMBOL_GPL(ieee80211_iter_chan_contexts_atomic);
