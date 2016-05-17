wl12xx-objs		= main.o cmd.o io.o event.o tx.o rx.o ps.o acx.o \
			  boot.o init.o debugfs.o scan.o

# use special params for the compat case
ifneq ($(KLIB_BUILD),)
ifneq ($(GIT_TREE),)
GIT_PATH := $(GIT_TREE)/
GIT_CHDIR_CMD := cd $(GIT_TREE) &&
endif
endif

ifneq ($(GIT_PATH),)
GIT_CHECK_PATH := $(GIT_PATH)
else
GIT_CHECK_PATH := $(PWD)/
GIT_CHDIR_CMD := cd $(GIT_CHECK_PATH) &&
endif

WLCORE_VERSION_DEPS := $(src)/../../../../compat_base_tree_version

ifeq ($(wildcard $(GIT_CHECK_PATH).git),)
# use only release_version.h if no .git

define filechk_version.h
	(echo 'static const char *wl12xx_git_head = \
			"$(shell cat $(WLCORE_VERSION_DEPS)";')
endef

else
# augment version with git describe

define filechk_version.h
	(echo 'static const char *wl12xx_git_head = \
			"$(shell cat $(WLCORE_VERSION_DEPS))+$(shell \
			 $(GIT_CHDIR_CMD) git describe --dirty)";')
endef

WLCORE_VERSION_DEPS += $(GIT_PATH).git
endif

.PHONY: $(obj)/version.h
$(obj)/version.h: $(WLCORE_VERSION_DEPS)
	@$(call filechk,version.h)

$(obj)/main.c: $(src)/version.h

# convert all wl12xx-objs to $(src)/file form
define WL12XX_OBJS_SRC
$(shell echo $(wl12xx-objs) |
	awk '{for (i=1;i<=NF;i++) {printf "$(src)/%s ", $$i} }')
endef

wl12xx_spi-objs 	= spi.o
wl12xx_sdio-objs	= sdio.o

wl12xx-$(CONFIG_NL80211_TESTMODE)	+= testmode.o
obj-$(CONFIG_WL12XX)			+= wl12xx.o
obj-$(CONFIG_WL12XX_SPI)		+= wl12xx_spi.o
obj-$(CONFIG_COMPAT_WL12XX_SDIO)		+= wl12xx_sdio.o

# small builtin driver bit
obj-$(CONFIG_WL12XX_PLATFORM_DATA)	+= wl12xx_platform_data.o
