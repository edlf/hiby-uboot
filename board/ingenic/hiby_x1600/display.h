/*
 * HiBy X1600 fastboot screen: what display.c and the panel files share
 *
 * display.c drives the PMIC supplies, the LCD controller, the backlight
 * and the drawing; a panel file (panel_<board>.c, picked by the board
 * header) describes the panel and powers it on and off.
 *
 * Board header settings (with CONFIG_HIBY_DISPLAY):
 *   CONFIG_HIBY_PANEL_LG35583 or CONFIG_HIBY_PANEL_ST7701_SBTC033001
 *   CONFIG_HIBY_LCD_SPI_CS, CONFIG_HIBY_LCD_RST   panel SPI CS, reset
 *                                (SCK PA00 and MOSI PA01 on both boards)
 *   CONFIG_HIBY_LCD_MV           panel supplies (AXP2101 BLDO1, BLDO2)
 *   CONFIG_HIBY_PMU_I2C_SCL, _SDA  I2C0 pins of the AXP2101
 *   CONFIG_HIBY_BL_PWM_FREQ      backlight PWM frequency
 *   CONFIG_HIBY_DISPLAY_BRIGHTNESS  backlight, percent
 *   CONFIG_HIBY_CHARGER_MP2731   the battery is charged by an MP2731 (I2C0,
 *                                0x4b), not the AXP2101: whether it is
 *                                charging is read from it
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation; either version 2 of
 * the License, or (at your option) any later version.
 */

#ifndef __HIBY_DISPLAY_H__
#define __HIBY_DISPLAY_H__

#include <common.h>
#include <asm/gpio.h>
#include <ingenic_soft_i2c.h>

/* The PMIC's I2C0, bit-banged (display.c); battery.c reads the gauge and the
 * charger on it too */
#define AXP2101_ADDR		0x34
extern struct i2c hiby_pmu_i2c;

#define HIBY_LCD_SPI_SCK	GPIO_PA(0)
#define HIBY_LCD_SPI_MOSI	GPIO_PA(1)
#define HIBY_LCD_SPI_CS		CONFIG_HIBY_LCD_SPI_CS
#define HIBY_LCD_RST		CONFIG_HIBY_LCD_RST

/* The panel supplies, as Linux names them (vcc_/vccio_regulator_name) */
#define HIBY_LDO_VCC		(1 << 4)	/* AXP2101 BLDO1 */
#define HIBY_LDO_VCCIO		(1 << 5)	/* AXP2101 BLDO2 */

/* A panel as its Linux module registers it with soc_fb (lcdc_data) */
struct hiby_panel {
	const char *name;
	unsigned int xres, yres, refresh;
	unsigned int hsync_len, left_margin, right_margin;
	unsigned int vsync_len, upper_margin, lower_margin;
	unsigned int color_even, color_odd;
	/*
	 * Off: release the controller pins (pixel clock low) before
	 * power_off, as the R1's firmware 2.0 soc_fb does; otherwise after.
	 */
	int pins_off_first;
	int (*power_on)(void);		/* supplies, reset, set-up commands */
	void (*power_off)(void);
};

/* The board's panel (panel_<board>.c); NULL: none found */
const struct hiby_panel *hiby_panel_probe(void);

/* display.c, for the panel files */
int hiby_lcd_supply(unsigned int ldos, int on);
void hiby_lcd_spi_write16(u16 val);	/* MSB first */
void hiby_lcd_spi_write9(u16 val);	/* MSB first; bit 8: data */

#define ST_CMD(c)	(c)
#define ST_DAT(d)	(0x100 | (d))
#define ST_DELAY(ms)	(0x8000 | (ms))	/* in a u16 sequence */

/* Sends a sequence of ST_CMD/ST_DAT/ST_DELAY words over 9-bit SPI */
void hiby_lcd_spi_send9(const u16 *seq, int count);

#endif /* __HIBY_DISPLAY_H__ */
