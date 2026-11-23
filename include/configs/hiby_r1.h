/*
 * HiBy R1 (Ingenic X1600) configuration
 *
 * Falcon boot from SPI NAND: the SPL loads the kernel directly. HiBy's
 * scheme is normal + recovery, not A/B: kernel/rootfs is the normal
 * system, kernel2/rootfs2 the recovery, booted when the "ota" partition
 * contains "ota:kernel2" (or on Volume up / "reboot recovery"). The full
 * U-Boot only runs for fastboot. Partition layout comes from the burner
 * partition table at CONFIG_SPIFLASH_PART_OFFSET (0x5800). See
 * README.md (U-Boot top directory).
 *
 * Everything else (DDR, clocks, SFC) is shared with x1600_base via
 * x1600_base_common.h.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation; either version 2 of
 * the License, or (at your option) any later version.
 */

#ifndef __HIBY_R1_H__
#define __HIBY_R1_H__

/* Console: UART2 at 3 Mbaud (matches stock firmware) */
#define CONFIG_SYS_UART_INDEX		2
#define CONFIG_BAUDRATE			3000000
#define CONFIG_SPL_SERIAL_SUPPORT

/* Kernel command line */
#define CONFIG_ARG_QUIET
#define CONFIG_ROOTFS_SQUASHFS		/* rootfs  -> /dev/mtdblock_bbt_ro2 */
#define CONFIG_ROOTFS2_SQUASHFS		/* rootfs2 -> /dev/mtdblock_bbt_ro4 */

/* Falcon boot with kernel/kernel2 OTA selection (implies CONFIG_SPL_OS_BOOT) */
#define CONFIG_SPL_OS_OTA_BOOT

/*
 * ADC key (board/ingenic/hiby_r1/adc_key.c): sampled once by the SPL.
 * A reading inside [MIN_MV, MAX_MV] boots kernel2/rootfs2 regardless of
 * the OTA flag. Values match the stock SPL: Volume up (1650 mV).
 */
#define CONFIG_HIBY_R1_ADC_KEY
#define CONFIG_HIBY_R1_ADC_KEY_CHANNEL		0	/* SADC AUX0 */
#define CONFIG_HIBY_R1_ADC_VREF_MV		3300
#define CONFIG_HIBY_R1_ADC_KEY_SAMPLE_MS	20
#define CONFIG_HIBY_R1_ADC_KEY_MIN_MV		1600
#define CONFIG_HIBY_R1_ADC_KEY_MAX_MV		1800

/*
 * DDR selection: use the eFuse SoC ID only for known X1600-family parts
 * (prints "SOC: <name>"), otherwise fall back to the burned DDR ID.
 */
#define CONFIG_X1600_SOCID_TABLE

/* DDR2 tuning from the stock SPL (see X1600HN_DDR2_COMPATIBLE_PARAM.h) */
#define CONFIG_X1600HN_DDR2_CONSERVATIVE

/*
 * Fastboot entry. The SPL starts the full U-Boot (stored right after the
 * SPL in the "uboot" partition) instead of Linux when either:
 *  - page 1 (offset 2048) of the "ota" partition starts with
 *    "ota:fastboot". Page 0 holds HiBy's A/B record ("ota:kernel" or
 *    "ota:kernel2" padded to 256 bytes) and is left as it is; U-Boot
 *    clears the request and keeps page 0.
 *  - Play/Pause is held at power-on.
 *
 * Keys at power-on, on the stock ADC ladder
 * (/module_driver/keyboard_adc_multifunc.sh: channel 0, 3300 mV idle,
 * keys matched within 100 mV):
 *   Play/Pause    0 mV   fastboot
 *   Volume down 1000 mV  normal kernel, forced (over the OTA record, the
 *                        fastboot request and the reboot signatures)
 *   Volume up   1650 mV  kernel2 (recovery), the window above
 */
/*
 * An "ota" block the SPL does not recognise (garbage, an unknown string,
 * an uncorrectable ECC error) is erased and the device boots normally.
 * A block marked bad is left alone.
 */
#define CONFIG_SPL_OTA_ERASE_INVALID

#define CONFIG_SPL_OTA_UBOOT_FLAG		"ota:fastboot"
#define CONFIG_SPL_OTA_UBOOT_FLAG_OFFSET	2048
#define CONFIG_SPL_UBOOT_PART_NAME		"uboot"
#define CONFIG_HIBY_R1_FASTBOOT_KEY_MIN_MV	0	/* Play/Pause */
#define CONFIG_HIBY_R1_FASTBOOT_KEY_MAX_MV	100
#define CONFIG_HIBY_R1_NORMAL_KEY_MIN_MV	900	/* Volume down */
#define CONFIG_HIBY_R1_NORMAL_KEY_MAX_MV	1100

/*
 * "fastboot reboot recovery" boots kernel2/rootfs2 once (like Volume up):
 * U-Boot leaves RECOVERY_SIGNATURE in the CPM scratchpad, the SPL clears
 * it. "fastboot reboot bootloader"/"fastboot" come back to fastboot.
 */
#define CONFIG_FASTBOOT_REBOOT_RECOVERY
#define CONFIG_SPL_SCRPAD_RECOVERY

#include "x1600_base_common.h"

/* Stock SPL leaves DDR2 DLL reset off (except X1600HN, set per chip) */
#undef CONFIG_DDR_DLL_RESET_EN

/*
 * Full U-Boot (fastboot only)
 */

/* No saved environment: the SDK's SFC NAND env sits inside "kernel". */
#undef CONFIG_ENV_IS_IN_SFC_NAND
#undef CONFIG_SYS_REDUNDAND_ENVIRONMENT
#undef CONFIG_ENV_OFFSET
#undef CONFIG_ENV_OFFSET_REDUND
#undef CONFIG_ENV_SECT_SIZE
#undef CONFIG_ENV_SIZE
#undef CONFIG_CMD_SAVEENV
#define CONFIG_ENV_IS_NOWHERE
#define CONFIG_ENV_SIZE			(32 << 10)

/* U-Boot only runs when fastboot was requested */
#undef CONFIG_BOOTDELAY
#define CONFIG_BOOTDELAY		0	/* no countdown: fastboot at once */
#define CONFIG_ZERO_BOOTDELAY_CHECK	/* a key already pressed: prompt */
#undef CONFIG_BOOTCOMMAND
#define CONFIG_BOOTCOMMAND		"fastboot"

/* USB device (DWC2 OTG) + fastboot */
/*
 * Disconnect at U-Boot start and connect once fastboot is up, so a reboot
 * out of a running gadget (Linux) re-enumerates cleanly; bus events
 * (reset, enumerated, suspend, resume) go to the RAM log when enabled.
 */
#define CONFIG_JZ_UDC_CLEAN_START
/*
 * Device-only PHY with VBUS and B-session always valid, as Linux sets it
 * up, so a host-suspended bus does not end the USB session
 * (arch/mips/cpu/xburst/x1600/clk.c: otg_phy_init()).
 */
#define CONFIG_JZ_UDC_VBUS_ALWAYS_VALID
/* USB PHY tuned as Linux does it (squelch, termination, references) */
#define CONFIG_X1600_USB_PHY_TUNING
#define CONFIG_USB_GADGET
#define CONFIG_USB_GADGET_DUALSPEED
#define CONFIG_USB_JZ_DWC2_UDC_V1_1
#define CONFIG_USB_GADGET_VBUS_DRAW	500
#define CONFIG_CMD_FASTBOOT
#define CONFIG_FASTBOOT_GADGET
#define CONFIG_FASTBOOT_FUNCTION
/* the HiBy R1's own USB IDs; host fastboot finds it by its interface class */
#define CONFIG_G_FASTBOOT_VENDOR_NUM	0x32bb
#define CONFIG_G_FASTBOOT_PRODUCT_NUM	0x0101
/* USB names, as the stock firmware reports them (usb_dev_mass_storage.sh) */
#define CONFIG_G_FASTBOOT_MANUFACTURER	"HiBy"
#define CONFIG_G_FASTBOOT_PRODUCT_NAME	"R1"
/* the fastboot interface's name, which host fastboot shows */
#define CONFIG_G_FASTBOOT_INTERFACE_NAME	"HiBy R1"

/*
 * LED while in fastboot mode: purple (red + blue). The R1 has only these
 * two (/module_driver/leds_pwm_add.sh: red on PC01, blue on PC02, both
 * PWM in Linux); U-Boot drives them as plain GPIOs. LED_ON is the pin
 * level that lights them: set it to 0 if they stay dark.
 */
#define CONFIG_HIBY_R1_LED_RED_GPIO	(2 * 32 + 1)	/* PC01 */
#define CONFIG_HIBY_R1_LED_BLUE_GPIO	(2 * 32 + 2)	/* PC02 */
#define CONFIG_HIBY_R1_LED_ON		1

/*
 * "fastboot fetch bootrom rom.bin" dumps the X1600 mask ROM (reset vector
 * 0xbfc00000, uncached). It is 32 KiB: a 64 KiB dump is the same 32 KiB
 * twice (the address range is mirrored).
 */
#define CONFIG_FASTBOOT_BOOTROM_BASE	0xbfc00000
#define CONFIG_FASTBOOT_BOOTROM_SIZE	0x8000		/* 32 KiB */

/* SPI NAND backend for flash/erase (drivers/usb/gadget/f_fastboot.c) */
#define CONFIG_FASTBOOT_SFC_NAND

/*
 * USB serial number from the SPI NAND unique ID (g_fastboot.c); without
 * it, or when the chip has none, the serial is "0123456789".
 */
#define CONFIG_FASTBOOT_SERIAL_NAND_UID
/* Download buffer: low RAM, clear of the SPL/U-Boot load area */
#define CONFIG_FASTBOOT_BUF_ADDR	0x80800000

/*
 * RAM log (common/ramlog.c), off by default: for debugging without a
 * serial console. Enable CONFIG_RAMLOG for "fastboot fetch log",
 * "fastboot oem log", and the same for "lastlog", the run before the last
 * warm reset. 1 MiB below the download buffer, above the U-Boot load area
 * and initial stack (0x80400000); uncached address. Two 64 KiB rings plus
 * a 256-byte header.
 */
/* #define CONFIG_RAMLOG */
#ifdef CONFIG_RAMLOG
#define CONFIG_RAMLOG_BASE		0xa0700000
#define CONFIG_RAMLOG_SIZE		0x10000
/* USB trace lines (setup packets, ep0/ep1 events, heartbeat) to the log */
#define CONFIG_JZ_UDC_RAMLOG_TRACE
#endif

/*
 * Answer the standard GET_STATUS and feature requests (composite.c and
 * the UDC driver). The host sends GET_STATUS after every resume from
 * autosuspend; unanswered, it waited 5 s and reset the device.
 */
#define CONFIG_USB_COMPOSITE_STD_STATUS

#endif /* __HIBY_R1_H__ */
