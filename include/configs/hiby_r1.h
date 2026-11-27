/*
 * HiBy R1 (Ingenic X1600) configuration
 *
 * What is the R1's own; the rest is shared with the other HiBy X1600
 * players in hiby_x1600_common.h. See README.md (U-Boot top directory).
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation; either version 2 of
 * the License, or (at your option) any later version.
 */

#ifndef __HIBY_R1_H__
#define __HIBY_R1_H__

/* Names: "getvar product" / version string, "getvar version-hardware", USB */
#define CONFIG_FASTBOOT_BOARD_NAME		"hiby_r1"
#define CONFIG_HIBY_HARDWARE_VER		"R1"
#define CONFIG_G_FASTBOOT_PRODUCT_NAME		"R1"
#define CONFIG_G_FASTBOOT_INTERFACE_NAME	"HiBy R1"

/*
 * Keys at power-on, on the stock ADC ladder
 * (/module_driver/keyboard_adc_multifunc.sh):
 *   Play/Pause    0 mV   fastboot
 *   Volume down 1000 mV  normal kernel, forced
 *   Volume up   1650 mV  kernel2 (recovery), as the stock SPL
 */
#define CONFIG_HIBY_ADC_KEY_MIN_MV		1600	/* Volume up */
#define CONFIG_HIBY_ADC_KEY_MAX_MV		1800
#define CONFIG_HIBY_FASTBOOT_KEY_MIN_MV		0	/* Play/Pause */
#define CONFIG_HIBY_FASTBOOT_KEY_MAX_MV		100
#define CONFIG_HIBY_NORMAL_KEY_MIN_MV		900	/* Volume down */
#define CONFIG_HIBY_NORMAL_KEY_MAX_MV		1100

/*
 * LED while in fastboot mode: purple (red + blue). The R1 has only these
 * two (/module_driver/leds_pwm_add.sh: red on PC01, blue on PC02, both
 * PWM in Linux); U-Boot drives them as plain GPIOs. LED_ON is the pin
 * level that lights them: set it to 0 if they stay dark.
 */
#define CONFIG_HIBY_LED_RED_GPIO	(2 * 32 + 1)	/* PC01 */
#define CONFIG_HIBY_LED_BLUE_GPIO	(2 * 32 + 2)	/* PC02 */
#define CONFIG_HIBY_LED_ON		1

/*
 * Screen in fastboot mode (board/ingenic/hiby_x1600/display.c, panel_r1.c;
 * settings in display.h), from /module_driver/lcd_lg35583.sh,
 * axp2101.sh, soc_i2c.sh and pwm_backlight.sh.
 */
#define CONFIG_HIBY_DISPLAY
#define CONFIG_HIBY_PANEL_LG35583		/* LG35583 or ST7701S */
#define CONFIG_HIBY_LCD_SPI_CS		(0 * 32 + 30)	/* PA30 */
#define CONFIG_HIBY_LCD_RST		(0 * 32 + 31)	/* PA31 */
#define CONFIG_HIBY_LCD_MV		3000		/* BLDO1, BLDO2 */
#define CONFIG_HIBY_PMU_I2C_SCL		(0 * 32 + 28)	/* PA28 */
#define CONFIG_HIBY_PMU_I2C_SDA		(0 * 32 + 29)	/* PA29 */
#define CONFIG_HIBY_BL_PWM_FREQ		30000
#define CONFIG_HIBY_DISPLAY_BRIGHTNESS	40	/* backlight, percent */
#define CONFIG_INGENIC_SOFT_I2C

#include "hiby_x1600_common.h"

#endif /* __HIBY_R1_H__ */
