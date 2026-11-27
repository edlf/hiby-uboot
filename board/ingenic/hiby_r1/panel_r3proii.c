/*
 * HiBy R3 Pro II panel for the fastboot screen (display.c)
 *
 * The R3 Pro II's lcd_st7701_sbtc033001 module (kernel
 * module_drivers/hiby/r3proii/lcd_st7701_sbtc033001): a 480x720 ST7701S
 * set up over 9-bit SPI. VCCIO, 5 ms, VCC, 5 ms, then reset; off is
 * reset low, VCCIO, VCC, with no commands. The stock soc_fb powers the
 * panel off before it releases the pins.
 * /module_driver/lcd_st7701_sbtc033001.sh: CS PB04, reset PB05, 2.8 V
 * supplies.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation; either version 2 of
 * the License, or (at your option) any later version.
 */

#include <common.h>
#include "display.h"

#define CMD(c)	ST_CMD(c)
#define DAT(d)	ST_DAT(d)

/* The panel set-up, up to Sleep Out (0x11); Display On follows 120 ms later */
static const u16 st7701s_init_seq[] = {
	CMD(0xff), DAT(0x77), DAT(0x01), DAT(0x00), DAT(0x00), DAT(0x13),
	CMD(0xef), DAT(0x08),
	CMD(0xff), DAT(0x77), DAT(0x01), DAT(0x00), DAT(0x00), DAT(0x10),
	CMD(0xc0), DAT(0x59), DAT(0x00),
	CMD(0xc1), DAT(0x10), DAT(0x0c),
	CMD(0xc2), DAT(0x17), DAT(0x0a),
	CMD(0xc3), DAT(0x02),
	CMD(0xcc), DAT(0x10),
	CMD(0xb0), DAT(0x0f), DAT(0x15), DAT(0x1d), DAT(0x10), DAT(0x13),
		 DAT(0x07), DAT(0x0b), DAT(0x08), DAT(0x08), DAT(0x26),
		 DAT(0x05), DAT(0x11), DAT(0x0f), DAT(0x2b), DAT(0x32),
		 DAT(0x1f),
	CMD(0xb1), DAT(0x0f), DAT(0x1c), DAT(0x21), DAT(0x0b), DAT(0x0f),
		 DAT(0x05), DAT(0x0c), DAT(0x09), DAT(0x08), DAT(0x24),
		 DAT(0x04), DAT(0x13), DAT(0x11), DAT(0x28), DAT(0x30),
		 DAT(0x1f),
	CMD(0xff), DAT(0x77), DAT(0x01), DAT(0x00), DAT(0x00), DAT(0x11),
	CMD(0xb0), DAT(0x4d),
	CMD(0xb1), DAT(0x4b),
	CMD(0xb2), DAT(0x81),
	CMD(0xb3), DAT(0x80),
	CMD(0xb5), DAT(0x4e),
	CMD(0xb7), DAT(0x85),
	CMD(0xb8), DAT(0x20),
	CMD(0xc1), DAT(0x78),
	CMD(0xc2), DAT(0x78),
	CMD(0xd0), DAT(0x88),
	CMD(0xc0), DAT(0x07),
	CMD(0xe0), DAT(0x00), DAT(0x00), DAT(0x02),
	CMD(0xe1), DAT(0x06), DAT(0x30), DAT(0x08), DAT(0x30), DAT(0x05),
		 DAT(0x30), DAT(0x07), DAT(0x30), DAT(0x00), DAT(0x33),
		 DAT(0x33),
	CMD(0xe2), DAT(0x22), DAT(0x22), DAT(0x33), DAT(0x33), DAT(0xe4),
		 DAT(0x00), DAT(0x00), DAT(0x00), DAT(0xe4), DAT(0x00),
		 DAT(0x00), DAT(0x00),
	CMD(0xe3), DAT(0x00), DAT(0x00), DAT(0x22), DAT(0x22),
	CMD(0xe4), DAT(0x44), DAT(0x44),
	CMD(0xe5), DAT(0x0d), DAT(0xe3), DAT(0x30), DAT(0xe6), DAT(0x0f),
		 DAT(0xe5), DAT(0x30), DAT(0xe6), DAT(0x09), DAT(0xdf),
		 DAT(0x30), DAT(0xe6), DAT(0x0b), DAT(0xe1), DAT(0x30),
		 DAT(0xe6),
	CMD(0xe6), DAT(0x00), DAT(0x00), DAT(0x22), DAT(0x22),
	CMD(0xe7), DAT(0x44), DAT(0x44),
	CMD(0xe8), DAT(0x0c), DAT(0xe2), DAT(0x30), DAT(0xe6), DAT(0x0e),
		 DAT(0xe4), DAT(0x30), DAT(0xe6), DAT(0x08), DAT(0xde),
		 DAT(0x30), DAT(0xe6), DAT(0x0a), DAT(0xe0), DAT(0x30),
		 DAT(0xe6),
	CMD(0xe9), DAT(0x36), DAT(0x01),
	CMD(0xeb), DAT(0x00), DAT(0x01), DAT(0xe4), DAT(0xe4), DAT(0x44),
		 DAT(0x88), DAT(0x40),
	CMD(0xed), DAT(0xff), DAT(0x56), DAT(0x7f), DAT(0xab), DAT(0x01),
		 DAT(0x42), DAT(0xcf), DAT(0xff), DAT(0xff), DAT(0xfc),
		 DAT(0x24), DAT(0x10), DAT(0xba), DAT(0xf7), DAT(0x65),
		 DAT(0xff),
	CMD(0xef), DAT(0x10), DAT(0x0d), DAT(0x04), DAT(0x08), DAT(0x3f),
		 DAT(0x1f),
	CMD(0x35), DAT(0x00),
	CMD(0x3a), DAT(0x66),
	CMD(0x11),
};

static int r3proii_power_on(void)
{
	gpio_set_value(HIBY_LCD_SPI_CS, 1);
	if (hiby_lcd_supply(HIBY_LDO_VCCIO, 1))
		return -1;
	mdelay(5);
	if (hiby_lcd_supply(HIBY_LDO_VCC, 1))
		return -1;
	mdelay(5);

	gpio_set_value(HIBY_LCD_RST, 1);
	mdelay(5);
	gpio_set_value(HIBY_LCD_RST, 0);
	mdelay(50);
	gpio_set_value(HIBY_LCD_RST, 1);
	mdelay(30);

	hiby_lcd_spi_send9(st7701s_init_seq, ARRAY_SIZE(st7701s_init_seq));
	mdelay(120);
	hiby_lcd_spi_write9(CMD(0x29));
	return 0;
}

static void r3proii_power_off(void)
{
	gpio_set_value(HIBY_LCD_RST, 0);
	hiby_lcd_supply(HIBY_LDO_VCCIO, 0);
	hiby_lcd_supply(HIBY_LDO_VCC, 0);

	gpio_set_value(HIBY_LCD_SPI_MOSI, 0);
	gpio_set_value(HIBY_LCD_SPI_SCK, 0);
	gpio_set_value(HIBY_LCD_SPI_CS, 0);
}

/* lcd_st7701_sbtc033001.c: lcdc_data */
static const struct hiby_panel r3proii_panel = {
	.name = "ST7701S (SBTC033001)",
	.refresh = 62,
	.xres = 480,
	.yres = 720,
	.left_margin = 40,
	.right_margin = 80,
	.upper_margin = 40,
	.lower_margin = 40,
	.hsync_len = 10,
	.vsync_len = 10,
	.color_even = 0,
	.color_odd = 0,
	.pins_off_first = 0,
	.power_on = r3proii_power_on,
	.power_off = r3proii_power_off,
};

const struct hiby_panel *hiby_panel_probe(void)
{
	printf("display: panel %s\n", r3proii_panel.name);
	return &r3proii_panel;
}
