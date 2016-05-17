wlcore-objs		= main.o cmd.o io.o event.o tx.o rx.o ps.o acx.o \
			  boot.o init.o debugfs.o scan.o sysfs.o vendor_cmd.o

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

WLCORE_VERSION_DEPS := $(src)/release_version.h

ifeq ($(wildcard $(GIT_CHECK_PATH).git),)
# use only release_version.h if no .git

define filechk_version.h
	(cat $(WLCORE_VERSION_DEPS))
endef

else
# augment version with git describe

define filechk_version.h
	(cat $(WLCORE_VERSION_DEPS) | sed 's/"\(.*\)"/"\1+$(shell \
			$(GIT_CHDIR_CMD) git describe --dirty)"/')
endef

WLCORE_VERSION_DEPS += $(GIT_PATH).git
endif

.PHONY: $(obj)/version.h
$(obj)/version.h: $(WLCORE_VERSION_DEPS)
	@$(call filechk,version.h)

# seems like in some cases, for unknown reason,
# this dependency is not enforced implictly,
# so add it explicitly
$(obj)/main.c: $(src)/version.h

wlcore_spi-objs 	= spi.o
wlcore_sdio-objs	= sdio.o

wlcore-$(CPTCFG_NL80211_TESTMODE)	+= testmode.o
obj-$(CPTCFG_WLCORE)			+= wlcore.o
obj-$(CPTCFG_WLCORE_SPI)		+= wlcore_spi.o
obj-$(CPTCFG_WLCORE_SDIO)		+= wlcore_sdio.o

ccflags-y += -D__CHECK_ENDIAN__
