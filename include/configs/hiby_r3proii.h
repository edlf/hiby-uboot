/*
 * HiBy R3 Pro II (Ingenic X1600) configuration
 *
 * What is the R3 Pro II's own; the rest is shared with the other HiBy
 * X1600 players in hiby_x1600_common.h, and the board code with the R1
 * (board/ingenic/hiby_x1600). See README.md (U-Boot top directory).
 *
 * Taken from the stock firmware 1.4 (r3proii(v1.4).upt): the same OTA
 * scheme as the R1 (kernel2/rootfs2 recovery, "ota:kernel2" record), the
 * same console UART and USB IDs; its own key ladder and LEDs.
 *
 * Compared with the R3 Pro II's stock SPL (a dump of its "uboot"
 * partition, U-Boot SPL 2013.07-g2a44fbe of 30 Aug 2025):
 *  - DDR: LPDDR2, one chip, M54D5121632A (64 MiB, the X1600E's own), built
 *    the way the SDK's CONFIG_X1600E_DDR does it. Its register values are
 *    those of the stock SPL, field for field; with one entry it is used
 *    whatever the eFuse SoC ID or the burned DDR ID say, as stock.
 *  - SPI NAND: every chip the stock SPL knows is in the shared table.
 *  - Kernel command lines, the OTA record: the same as the R1's.
 *  - Partition table: the same as the R1's, recovery root fs "rootfs2"
 *    (partitions/r3proii.txt).
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation; either version 2 of
 * the License, or (at your option) any later version.
 */

#ifndef __HIBY_R3PROII_H__
#define __HIBY_R3PROII_H__

/* Names: "getvar product" / version string, "getvar version-hardware", USB */
#define CONFIG_FASTBOOT_BOARD_NAME		"hiby_r3proii"
#define CONFIG_HIBY_HARDWARE_VER		"R3PROII"
#define CONFIG_G_FASTBOOT_PRODUCT_NAME		"R3 Pro II"
#define CONFIG_G_FASTBOOT_INTERFACE_NAME	"HiBy R3 Pro II"

/*
 * Keys at power-on, on the stock ADC ladder (/module_driver/keyboard_adc.sh:
 * channel 0, 3300 mV idle, keys matched within 100 mV):
 *   Play/Pause    0 mV   fastboot
 *   Volume down 1115 mV  normal kernel, forced
 *   Volume up   1700 mV  kernel2 (recovery)
 *   Next        2200 mV  (not used at power-on)
 */
#define CONFIG_HIBY_ADC_KEY_MIN_MV		1600	/* Volume up */
#define CONFIG_HIBY_ADC_KEY_MAX_MV		1800
#define CONFIG_HIBY_FASTBOOT_KEY_MIN_MV		0	/* Play/Pause */
#define CONFIG_HIBY_FASTBOOT_KEY_MAX_MV		100
#define CONFIG_HIBY_NORMAL_KEY_MIN_MV		1015	/* Volume down */
#define CONFIG_HIBY_NORMAL_KEY_MAX_MV		1215

/*
 * No fastboot LED: the R3 Pro II's LEDs hang off an SGM31324 LED driver
 * on I2C bus 0 (/module_driver/leds_sgm31324_add.sh), not plain GPIOs,
 * and U-Boot has no driver for it.
 */

/*
 * DDR as the stock SPL: LPDDR2 M54D5121632A only (x1600_base_common.h), not
 * the R1's table of eight chips chosen by SoC ID. Before the include, which
 * reads it.
 */
#define CONFIG_X1600E_DDR

/*
 * Screen in fastboot mode (board/ingenic/hiby_x1600/display.c,
 * panel_r3proii.c; settings in display.h), from
 * /module_driver/lcd_st7701_sbtc033001.sh, axp2101.sh, soc_i2c.sh and
 * pwm_backlight.sh.
 */
#define CONFIG_HIBY_DISPLAY
#define CONFIG_HIBY_PANEL_ST7701_SBTC033001
#define CONFIG_HIBY_LCD_SPI_CS		(1 * 32 + 4)	/* PB04 */
#define CONFIG_HIBY_LCD_RST		(1 * 32 + 5)	/* PB05 */
#define CONFIG_HIBY_LCD_MV		2800		/* BLDO1, BLDO2 */
#define CONFIG_HIBY_PMU_I2C_SCL		(1 * 32 + 30)	/* PB30 */
#define CONFIG_HIBY_PMU_I2C_SDA		(1 * 32 + 31)	/* PB31 */
#define CONFIG_HIBY_BL_PWM_FREQ		10000
#define CONFIG_HIBY_DISPLAY_BRIGHTNESS	40	/* backlight, percent */
#define CONFIG_INGENIC_SOFT_I2C

#include "hiby_x1600_common.h"

#endif /* __HIBY_R3PROII_H__ */
