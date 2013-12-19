#ifndef LINUX_3_3_COMPAT_H
#define LINUX_3_3_COMPAT_H

#include <linux/version.h>

#if (LINUX_VERSION_CODE < KERNEL_VERSION(3,3,0))

/* include to override NL80211_FEATURE_SK_TX_STATUS */
#include <linux/nl80211.h>
#include <linux/skbuff.h>
#include <net/sch_generic.h>

#if !((LINUX_VERSION_CODE >= KERNEL_VERSION(3,2,9) && LINUX_VERSION_CODE < KERNEL_VERSION(3,3,0)) || (LINUX_VERSION_CODE >= KERNEL_VERSION(3,0,23) && LINUX_VERSION_CODE < KERNEL_VERSION(3,1,0)))
#if (LINUX_VERSION_CODE > KERNEL_VERSION(2,6,37))
static inline void qdisc_cb_private_validate(const struct sk_buff *skb, int sz)
{
	BUILD_BUG_ON(sizeof(skb->cb) < sizeof(struct qdisc_skb_cb) + sz);
}
#else
static inline void qdisc_cb_private_validate(const struct sk_buff *skb, int sz)
{
	/* XXX ? */
}
#endif
#endif

extern struct sk_buff *__pskb_copy(struct sk_buff *skb,
				   int headroom, gfp_t gfp_mask);

static inline void skb_complete_wifi_ack(struct sk_buff *skb, bool acked)
{
	WARN_ON(1);
}
#define NL80211_FEATURE_SK_TX_STATUS 0

typedef u32 netdev_features_t;

/* source include/linux/device.h */
/**
 * module_driver() - Helper macro for drivers that don't do anything
 * special in module init/exit. This eliminates a lot of boilerplate.
 * Each module may only use this macro once, and calling it replaces
 * module_init() and module_exit().
 *
 * Use this macro to construct bus specific macros for registering
 * drivers, and do not use it on its own.
 */
#define module_driver(__driver, __register, __unregister) \
static int __init __driver##_init(void) \
{ \
	return __register(&(__driver)); \
} \
module_init(__driver##_init); \
static void __exit __driver##_exit(void) \
{ \
	__unregister(&(__driver)); \
} \
module_exit(__driver##_exit);

/* source include/linux/usb.h */
/**
 * module_usb_driver() - Helper macro for registering a USB driver
 * @__usb_driver: usb_driver struct
 *
 * Helper macro for USB drivers which do not do anything special in module
 * init/exit. This eliminates a lot of boilerplate. Each module may only
 * use this macro once, and calling it replaces module_init() and module_exit()
 */
#define module_usb_driver(__usb_driver) \
	module_driver(__usb_driver, usb_register, \
		       usb_deregister)

#endif /* (LINUX_VERSION_CODE < KERNEL_VERSION(3,3,0)) */

#endif /* LINUX_3_3_COMPAT_H */
