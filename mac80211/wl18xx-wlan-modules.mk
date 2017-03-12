WL18XX_FOLDER ?= hardware/ti/wlan/mac80211/compat_wl18xx

WL18XX_MODULES:
	make mrproper -C $(WL18XX_FOLDER) KLIB=$(KERNEL_OUT) KLIB_BUILD=$(KERNEL_OUT)
	make defconfig-wl18xx -C $(WL18XX_FOLDER) KLIB=$(KERNEL_OUT) KLIB_BUILD=$(KERNEL_OUT)
	make -j8 -C $(WL18XX_FOLDER) KLIB=$(KERNEL_OUT) KLIB_BUILD=$(KERNEL_OUT) \
		ARCH=arm $(if $(ARM_CROSS_COMPILE),$(ARM_CROSS_COMPILE),$(KERNEL_CROSS_COMPILE))
	mv $(WL18XX_FOLDER)/{compat/compat,net/{mac80211/mac,wireless/cfg}80211}.ko $(KERNEL_MODULES_OUT)
	mv $(WL18XX_FOLDER)/drivers/net/wireless/ti/{wlcore/wlcore{,_sdio},wl12xx/wl12xx,wl18xx/wl18xx}.ko \
		$(KERNEL_MODULES_OUT); rm -f $(KERNEL_MODULES_OUT)/wl12xx_sdio.ko
	$(if $(ARM_EABI_TOOLCHAIN),$(ARM_EABI_TOOLCHAIN)/arm-eabi-strip,$(KERNEL_TOOLCHAIN_PATH)strip) \
		--strip-unneeded $(KERNEL_MODULES_OUT)/{compat,{cfg,mac}80211,wlcore{,_sdio},wl1{2,8}xx}.ko

TARGET_KERNEL_MODULES += WL18XX_MODULES
