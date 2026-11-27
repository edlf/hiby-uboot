/*
 * HiBy X1600 fastboot screen (full U-Boot only)
 *
 * Lights the RGB panel while fastboot runs and shows what mode the player
 * is in. A port of the parts of the players' Linux display stack that
 * U-Boot needs, kept to their order and timings:
 *
 *  - panel supplies: AXP2101 BLDO1 (VCC) and BLDO2 (VCCIO), over a
 *    bit-banged I2C0 (the PMIC is at 0x34) (/module_driver/axp2101.sh)
 *  - the panel: panel_<board>.c, from the board's panel module
 *    (kernel module_drivers/hiby/<board>/lcd_*), set up over a
 *    bit-banged SPI
 *  - the X1600 LCD controller: parallel RGB, RGB565 frame, one
 *    descriptor pointing at itself (module_drivers/hiby/common/soc_fb)
 *  - the backlight: PWM channel 0 on PC00, active high, at
 *    CONFIG_HIBY_DISPLAY_BRIGHTNESS percent (pwm_backlight.sh, soc_pwm)
 *
 *  - a menu: Boot system, Boot recovery, Power off; Vol+ and
 *    Vol- move (the ADC key ladder), a short press of Power picks (PC31);
 *    polled from the fastboot main loop without waiting, ignored while a
 *    command runs. A long press of Power picks nothing, so holding it
 *    still forces the PMIC off. Power off is the AXP2101's, as Linux's
 *    pm_power_off.
 *  - the battery line: percentage and charging, from battery.c, every
 *    few seconds; battery.c also keeps the R3 Pro II's charge limit,
 *    released before a reset, a boot or a power off (below).
 *
 * Everything is turned off again before a reset or a "boot" from RAM,
 * so Linux finds the panel as it does after power-on. See display.h for
 * the board settings.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation; either version 2 of
 * the License, or (at your option) any later version.
 */

#include <common.h>
#include <malloc.h>
#include <asm/io.h>
#include <asm/addrspace.h>
#include <asm/gpio.h>
#include <asm/arch/cpm.h>
#include <asm/arch/clk.h>
#include <ingenic_soft_i2c.h>
#include <video_font.h>
#include <video_font_data.h>
#include <fastboot.h>
#include "display.h"
#include "battery.h"

void hiby_r1_display_off(void);

/* --- pins and parts -------------------------------------------------- */

#define PIN_PCLK	GPIO_PA(24)
#define PIN_BACKLIGHT	GPIO_PC(0)

/* RGB666 on a 24-bit bus: D2-D7, D10-D15, D18-D23, PCLK, VSYNC, HSYNC, DE */
#define TFT_PINS	0x0ffcfcfc

#define AXP_CHIP_ID	0x03
#define AXP_LDO_EN0	0x90
#define AXP_BLDO1_V	0x96
#define AXP_BLDO2_V	0x97
#define AXP_LDO_MV(mv)	(((mv) - 500) / 100)	/* 0.5 V + 100 mV steps */

/* --- backlight (soc_pwm.c, pwm_backlight.c) -------------------------- */

#ifndef CONFIG_HIBY_DISPLAY_BRIGHTNESS
#define CONFIG_HIBY_DISPLAY_BRIGHTNESS	40	/* percent */
#endif

#define PWM_REG(off)		(CKSEG1ADDR(0x134c0000) + (off))
#define PWM_ENS			0x00	/* write 1: channel on */
#define PWM_ENC			0x04	/* write 1: channel off */
#define PWM_UPT			0x10	/* write 1: load the waveform word */
#define PWM_BUSY		0x14
#define PWM_MODE		0x20	/* 0: waveform word, 1: DMA */
#define PWM_INL			0x24	/* level at the start of a period */
#define PWM_IDL			0x28	/* level while idle */
#define PWM_CCFG(n)		(0x40 + 4 * (n))	/* divider, 2^value */
#define PWM_WCFG(n)		(0x80 + 4 * (n))	/* low 15:0, high 31:16 */
#define PWM_MAX_COUNT		0xffff

#define BL_PWM_CH		0	/* PC00, function 0 */
#define BL_PWM_FREQ		CONFIG_HIBY_BL_PWM_FREQ
#define BL_PWM_LEVELS		101	/* pwm_backlight.sh: max_brightness */
#define BL_PWM_CLK		100000000	/* MPLL 1400 MHz / 14 */

/* --- the LCD controller (soc_fb.c) ----------------------------------- */

#define LCDC_REG(off)		(CKSEG1ADDR(0x13050000) + (off))
#define LCDC_FRM_DESC		0x1000
#define LCDC_FRM_START		0x1004
#define LCDC_CTRL		0x2000
#define  LCDC_CTRL_GEN_STOP	(1 << 4)
#define LCDC_CLR_ST		0x2008
#define LCDC_INT_EN		0x200c
#define LCDC_ST			0x2010
#define  LCDC_ST_STOPPED	(1 << 7)
#define  LCDC_ST_UNDERRUN	(1 << 8)
#define  LCDC_ST_FRM_DONE	(1 << 17)
#define LCDC_CFG2		0x2014
#define LCDC_COM_CFG		0x8000
#define LCDC_TFT_HSYNC		0x9000
#define LCDC_TFT_VSYNC		0x9004
#define LCDC_TFT_HDE		0x9008
#define LCDC_TFT_VDE		0x900c
#define LCDC_TFT_CFG		0x9010

struct lcdc_desc {
	u32 next;	/* physical: this descriptor again */
	u32 addr;	/* physical: the frame */
	u32 stride;	/* pixels */
	u32 cfg;	/* format 22:19 */
	u32 ctrl;
};
#define DESC_FMT_RGB565		(2 << 19)
#define DESC_CTRL_BIT17		(1 << 17)

static inline void lcdc_write(u32 off, u32 v)
{
	writel(v, LCDC_REG(off));
}

static inline u32 lcdc_read(u32 off)
{
	return readl(LCDC_REG(off));
}

static u32 timing(unsigned int start, unsigned int end)
{
	return ((start & 0xfff) << 16) | (end & 0xfff);
}

/* --- state ----------------------------------------------------------- */

static const struct hiby_panel *panel;
static u16 *fb;			/* uncached view of the frame */
static struct lcdc_desc *desc;	/* uncached */
static void *fb_mem, *desc_mem;	/* as allocated */
static int xres, yres;
static int lcd_on;
static int shown_busy = -1;
/* The PMIC's I2C0, shared with battery.c (display.h) */
struct i2c hiby_pmu_i2c = {
	.scl = CONFIG_HIBY_PMU_I2C_SCL,
	.sda = CONFIG_HIBY_PMU_I2C_SDA,
};

/* --- PMIC ------------------------------------------------------------ */

static int axp_read(u8 reg, u8 *val)
{
	return i2c_read(&hiby_pmu_i2c, AXP2101_ADDR, reg, 1, val, 1);
}

static int axp_write(u8 reg, u8 val)
{
	return i2c_write(&hiby_pmu_i2c, AXP2101_ADDR, reg, 1, &val, 1);
}

static int axp_update(u8 reg, u8 mask, u8 val)
{
	u8 v;

	if (axp_read(reg, &v))
		return -1;
	return axp_write(reg, (v & ~mask) | (val & mask));
}

static int axp_setup(void)
{
	u8 id;

	i2c_init(&hiby_pmu_i2c);
	if (axp_read(AXP_CHIP_ID, &id)) {
		printf("display: no AXP2101 at 0x%x\n", AXP2101_ADDR);
		return -1;
	}
	if (axp_write(AXP_BLDO1_V, AXP_LDO_MV(CONFIG_HIBY_LCD_MV)) ||
	    axp_write(AXP_BLDO2_V, AXP_LDO_MV(CONFIG_HIBY_LCD_MV)))
		return -1;
	return 0;
}

/* @ldos (HIBY_LDO_VCC, HIBY_LDO_VCCIO) on or off */
int hiby_lcd_supply(unsigned int ldos, int on)
{
	return axp_update(AXP_LDO_EN0, ldos, on ? ldos : 0);
}

/* --- panel SPI ------------------------------------------------------- */

/* One 16-bit word, MSB first (lcd_lg35583.c) */
void hiby_lcd_spi_write16(u16 val)
{
	int i;

	gpio_set_value(HIBY_LCD_SPI_SCK, 0);
	gpio_set_value(HIBY_LCD_SPI_CS, 0);
	udelay(1);
	for (i = 0; i < 16; i++) {
		gpio_set_value(HIBY_LCD_SPI_MOSI, (val & 0x8000) ? 1 : 0);
		val <<= 1;
		gpio_set_value(HIBY_LCD_SPI_SCK, 1);
		gpio_set_value(HIBY_LCD_SPI_SCK, 0);
	}
	udelay(1);
	gpio_set_value(HIBY_LCD_SPI_CS, 1);
	gpio_set_value(HIBY_LCD_SPI_MOSI, 0);
	udelay(1);
}

/* One 9-bit word, MSB first, bit 8 set for data (both ST7701S modules) */
void hiby_lcd_spi_write9(u16 val)
{
	int i;

	gpio_set_value(HIBY_LCD_SPI_SCK, 0);
	gpio_set_value(HIBY_LCD_SPI_CS, 0);
	udelay(1);
	for (i = 0; i < 9; i++) {
		gpio_set_value(HIBY_LCD_SPI_MOSI, (val & 0x100) ? 1 : 0);
		val <<= 1;
		gpio_set_value(HIBY_LCD_SPI_SCK, 0);
		udelay(1);
		gpio_set_value(HIBY_LCD_SPI_SCK, 1);
		udelay(1);
	}
	udelay(1);
	gpio_set_value(HIBY_LCD_SPI_CS, 1);
	gpio_set_value(HIBY_LCD_SPI_MOSI, 0);
	udelay(1);
}

void hiby_lcd_spi_send9(const u16 *seq, int count)
{
	int i;

	for (i = 0; i < count; i++) {
		if (seq[i] & 0x8000)
			mdelay(seq[i] & 0x7fff);
		else
			hiby_lcd_spi_write9(seq[i]);
	}
}

/* --- controller (soc_fb.c: enable_fb() for a TFT panel) -------------- */

static void lcdc_init(void)
{
	const struct hiby_panel *p = panel;
	unsigned int hde = p->hsync_len + p->left_margin;
	unsigned int vde = p->vsync_len + p->upper_margin;
	unsigned int htotal = hde + p->xres + p->right_margin;
	unsigned int vtotal = vde + p->yres + p->lower_margin;
	u32 v;

	cpm_outl(cpm_inl(CPM_CLKGR0) & ~CPM_CLKGR_LCD, CPM_CLKGR0);
	/* soc_fb: a pixclock of 0 is htotal * vtotal * refresh */
	clk_set_rate(LCD, htotal * vtotal * p->refresh);
	/* LPCDR bit 26: both panels leave lpcdr_bit26 at 0 */
	cpm_outl(cpm_inl(CPM_LPCDR) & ~(1 << 26), CPM_LPCDR);

	gpio_set_func(GPIO_PORT_A, GPIO_FUNC_0, TFT_PINS);

	lcdc_write(LCDC_INT_EN, LCDC_ST_FRM_DONE | LCDC_ST_UNDERRUN |
		   LCDC_ST_STOPPED);
	lcdc_write(LCDC_CLR_ST, lcdc_read(LCDC_ST));
	lcdc_write(LCDC_CFG2, lcdc_read(LCDC_CFG2) | 0x30);

	/* RGB565 frame: no dithering; bits 6 and 5; interface 1 (TFT) */
	v = lcdc_read(LCDC_COM_CFG);
	v &= ~((1 << 4) | (0x3f << 16) | 0x3);
	v |= (1 << 6) | (1 << 5) | 1;
	lcdc_write(LCDC_COM_CFG, v);

	lcdc_write(LCDC_TFT_HSYNC, timing(p->hsync_len, htotal));
	lcdc_write(LCDC_TFT_VSYNC, timing(p->vsync_len, vtotal));
	lcdc_write(LCDC_TFT_HDE, timing(hde, hde + p->xres));
	lcdc_write(LCDC_TFT_VDE, timing(vde, vde + p->yres));
	/* all levels active high, data on the rising edge, parallel 24-bit */
	lcdc_write(LCDC_TFT_CFG, ((p->color_even & 0x7) << 19) |
		   ((p->color_odd & 0x7) << 16));
}

static int lcdc_start(void)
{
	int i;

	desc->next = CPHYSADDR((ulong)desc);
	desc->addr = CPHYSADDR((ulong)fb);
	desc->stride = xres;
	desc->cfg = DESC_FMT_RGB565;
	desc->ctrl = DESC_CTRL_BIT17;

	lcdc_write(LCDC_CLR_ST, LCDC_ST_FRM_DONE);
	lcdc_write(LCDC_FRM_DESC, CPHYSADDR((ulong)desc));
	lcdc_write(LCDC_FRM_START, 1);

	for (i = 0; i < 300; i++) {		/* soc_fb waits 300 ms */
		if (lcdc_read(LCDC_ST) & LCDC_ST_FRM_DONE) {
			lcdc_write(LCDC_CLR_ST, LCDC_ST_FRM_DONE);
			return 0;
		}
		mdelay(1);
	}
	printf("display: no frame from the LCD controller (ST 0x%08x)\n",
	       lcdc_read(LCDC_ST));
	return -1;
}

static void lcdc_stop(void)
{
	int i;

	lcdc_write(LCDC_CTRL, LCDC_CTRL_GEN_STOP);
	for (i = 0; i < 100; i++) {		/* soc_fb: STOP_WAIT_MS */
		if (lcdc_read(LCDC_ST) & LCDC_ST_STOPPED)
			break;
		mdelay(1);
	}
	lcdc_write(LCDC_CLR_ST, lcdc_read(LCDC_ST));
	lcdc_write(LCDC_INT_EN, 0);
}

/* --- backlight ------------------------------------------------------ */

static inline void pwm_write(u32 off, u32 v)
{
	writel(v, PWM_REG(off));
}

static inline u32 pwm_read(u32 off)
{
	return readl(PWM_REG(off));
}

static int bl_pwm_on;	/* the PWM channel runs (its clock is open) */

static void pwm_set_bit(u32 off, int on)
{
	u32 v = pwm_read(off) & ~(1 << BL_PWM_CH);

	pwm_write(off, v | (on ? 1 << BL_PWM_CH : 0));
}

/*
 * soc_pwm's pwm2_config() and pwm2_set_level() for the backlight: the
 * period halves the clock until it fits 16 bits and is a whole number of
 * levels; high for the duty, then low; idle low.
 */
static void backlight_on(void)
{
	unsigned long rate;
	u32 period, duty, div = 0;
	int i;

	cpm_outl(cpm_inl(CPM_CLKGR1) & ~CPM_CLKGR_PWM, CPM_CLKGR1);
	clk_set_rate(PWM, BL_PWM_CLK);
	rate = clk_get_rate(PWM);

	period = rate / BL_PWM_FREQ;
	while (period > PWM_MAX_COUNT && div < 7) {
		div++;
		period = (rate >> div) / BL_PWM_FREQ;
	}
	if (period > PWM_MAX_COUNT)
		period = PWM_MAX_COUNT;
	if (period >= BL_PWM_LEVELS)
		period -= period % BL_PWM_LEVELS;
	duty = period * CONFIG_HIBY_DISPLAY_BRIGHTNESS / 100;
	if (!duty || duty >= period) {
		/* 0 % or 100 %: a constant level, as soc_pwm does it */
		gpio_direction_output(PIN_BACKLIGHT, duty ? 1 : 0);
		return;
	}

	pwm_write(PWM_ENC, 1 << BL_PWM_CH);
	pwm_write(PWM_CCFG(BL_PWM_CH), div);
	pwm_set_bit(PWM_IDL, 0);
	pwm_set_bit(PWM_INL, 1);
	pwm_set_bit(PWM_MODE, 0);
	pwm_write(PWM_WCFG(BL_PWM_CH), ((period - duty) & 0xffff) | duty << 16);
	pwm_write(PWM_UPT, 1 << BL_PWM_CH);

	gpio_set_func(GPIO_PORT_C, GPIO_FUNC_0, 1 << (PIN_BACKLIGHT % 32));
	pwm_write(PWM_ENS, 1 << BL_PWM_CH);
	bl_pwm_on = 1;
	for (i = 0; i < 100 && (pwm_read(PWM_BUSY) & (1 << BL_PWM_CH)); i++)
		udelay(10);

	printf("display: backlight %d%% (PWM %lu Hz / %u, %u high)\n",
	       CONFIG_HIBY_DISPLAY_BRIGHTNESS, rate >> div, period, duty);
}

static void backlight_off(void)
{
	gpio_direction_output(PIN_BACKLIGHT, 0);
	if (bl_pwm_on)
		pwm_write(PWM_ENC, 1 << BL_PWM_CH);
	bl_pwm_on = 0;
}

/* --- drawing --------------------------------------------------------- */

#define RGB(r, g, b)	((((r) >> 3) << 11) | (((g) >> 2) << 5) | ((b) >> 3))
#define COL_BG		RGB(0, 0, 0)
#define COL_TITLE	RGB(200, 80, 255)	/* the LED's purple */
#define COL_TEXT	RGB(220, 220, 220)
#define COL_DIM		RGB(130, 130, 130)
#define COL_READY	RGB(80, 220, 80)
#define COL_BUSY	RGB(255, 180, 0)

static void fill_rect(int x, int y, int w, int h, u16 col)
{
	int i, j;

	for (j = y; j < y + h && j < yres; j++)
		for (i = x; i < x + w && i < xres; i++)
			fb[j * xres + i] = col;
}

static void draw_char(int x, int y, unsigned char c, int s, u16 fg)
{
	const unsigned char *glyph = &video_fontdata[c * VIDEO_FONT_HEIGHT];
	int row, col;

	for (row = 0; row < VIDEO_FONT_HEIGHT; row++)
		for (col = 0; col < VIDEO_FONT_WIDTH; col++)
			if (glyph[row] & (0x80 >> col))
				fill_rect(x + col * s, y + row * s, s, s, fg);
}

/* @str centred on line @y, @s times the 8x16 font */
static void draw_text(int y, const char *str, int s, u16 fg)
{
	int w = strlen(str) * VIDEO_FONT_WIDTH * s;
	int x = w < xres ? (xres - w) / 2 : 0;

	for (; *str && x + VIDEO_FONT_WIDTH * s <= xres; str++) {
		draw_char(x, y, *str, s, fg);
		x += VIDEO_FONT_WIDTH * s;
	}
}

/* laid out around the middle of the screen */
#define MID		(yres / 2)
#define STATUS_Y	(MID - 40)
#define STATUS_H	(VIDEO_FONT_HEIGHT * 3)

static void draw_status_text(const char *str, u16 col)
{
	fill_rect(0, STATUS_Y, xres, STATUS_H, COL_BG);
	draw_text(STATUS_Y, str, 3, col);
}

static void draw_status(int busy)
{
	if (busy)
		draw_status_text("Working...", COL_BUSY);
	else
		draw_status_text("Ready", COL_READY);
}

#define BATTERY_Y	(MID + 30)
#define BATTERY_H	(VIDEO_FONT_HEIGHT * 2)
#define BATTERY_POLL_MS	5000

static int shown_pct = -2, shown_chg = -1;	/* -2: not drawn yet */
static ulong battery_at;

/* "Battery 73%" (", charging"); nothing when it cannot be read */
static void draw_battery(void)
{
	int pct = hiby_battery_percent();
	int chg;
	char line[32];

	hiby_charge_limit_poll(pct);
	chg = pct >= 0 && hiby_battery_charging();

	battery_at = get_timer(0);
	if (pct == shown_pct && chg == shown_chg)
		return;
	shown_pct = pct;
	shown_chg = chg;

	fill_rect(0, BATTERY_Y, xres, BATTERY_H, COL_BG);
	if (pct < 0)
		return;
	sprintf(line, "Battery %d%%%s", pct, chg ? ", charging" : "");
	draw_text(BATTERY_Y, line, 2, pct <= 15 && !chg ? COL_BUSY : COL_TEXT);
}

/* The menu: Vol+/Vol- move the bar, a short press of Power picks */
static const struct {
	const char *label;
	const char *doing;	/* status once picked */
	int action;		/* FASTBOOT_BOARD_* */
} menu[] = {
	{ "Boot system",		"Booting...",		FASTBOOT_BOARD_REBOOT },
	{ "Boot recovery",	"Booting...",		FASTBOOT_BOARD_RECOVERY },
	{ "Power off",		"Powering off...",	FASTBOOT_BOARD_POWEROFF },
};
#define MENU_N		ARRAY_SIZE(menu)
#define MENU_STEP	44		/* a 32-pixel line and its bar */
#define MENU_PAD	6

/* The help at the bottom: a line at twice the font, one at its size */
#define HELP_Y		(yres - 84)
#define HELP2_Y		(yres - 40)

/* The menu's bars centred between the battery line and the help */
#define MENU_TOP	((BATTERY_Y + BATTERY_H + HELP_Y - MENU_N * MENU_STEP) / 2)
#define MENU_Y		(MENU_TOP + MENU_PAD)

static int menu_sel;

static void draw_menu_item(int i)
{
	int y = MENU_Y + i * MENU_STEP;
	int sel = i == menu_sel;

	fill_rect(16, y - MENU_PAD, xres - 32, MENU_STEP, sel ? COL_TITLE : COL_BG);
	draw_text(y, menu[i].label, 2, sel ? RGB(255, 255, 255) : COL_TEXT);
}

static void draw_screen(void)
{
	int i;

	fill_rect(0, 0, xres, yres, COL_BG);
	draw_text(MID - 240, "FASTBOOT", 6, COL_TITLE);
	draw_text(MID - 120, "HiBy " CONFIG_G_FASTBOOT_PRODUCT_NAME, 3, COL_TEXT);
	draw_status(0);
	for (i = 0; i < MENU_N; i++)
		draw_menu_item(i);
	draw_text(HELP_Y, "Vol+/Vol-: move  Power: select", 2, COL_DIM);
	draw_text(HELP2_Y,
		  "Or over USB: fastboot reboot. Hold power to force off.",
		  1, COL_DIM);
}

/* --- keys (the ADC ladder, as the SPL reads it) ---------------------- */

void hiby_r1_adc_poll_start(void);
int hiby_r1_adc_poll_mv(unsigned int ch);
void hiby_r1_adc_poll_stop(void);

enum { KEY_NONE, KEY_UP, KEY_DOWN, KEY_PLAY, KEY_OTHER };

#define KEY_DEBOUNCE_MS	40	/* the same key, read for this long */

static int keys_on;		/* the ADC is polled */
static int keys_armed;		/* the ladder was idle once: no key held from start */
static int key_raw = -1, key_state = -1;	/* -1: not read yet */
static ulong key_since;

static int in_window(int mv, int lo, int hi)
{
	return mv >= lo && mv <= hi;
}

static int key_from_mv(int mv)
{
	if (in_window(mv, CONFIG_HIBY_ADC_KEY_MIN_MV, CONFIG_HIBY_ADC_KEY_MAX_MV))
		return KEY_UP;
	if (in_window(mv, CONFIG_HIBY_NORMAL_KEY_MIN_MV,
		      CONFIG_HIBY_NORMAL_KEY_MAX_MV))
		return KEY_DOWN;
	if (in_window(mv, CONFIG_HIBY_FASTBOOT_KEY_MIN_MV,
		      CONFIG_HIBY_FASTBOOT_KEY_MAX_MV))
		return KEY_PLAY;	/* not used by the menu */
	/* idle is the reference (3300 mV); anything between keys is not one */
	return mv > CONFIG_HIBY_ADC_VREF_MV - 300 ? KEY_NONE : KEY_OTHER;
}

/*
 * The power key: PC31, low while pressed, on both boards
 * (/module_driver/keyboard_gpio_add.sh, KEY_POWER).
 */
#define PIN_POWER_KEY	GPIO_PC(31)
#define PWR_LONG_MS	1000	/* held this long: a forced power-off, not a pick */

static int pwr_raw = -1, pwr_state = -1;	/* 1: pressed; -1: not read yet */
static int pwr_counts;		/* the press began while the menu was up */
static ulong pwr_since, pwr_down_at;

/* 1 when a short press of Power has just been released */
static int power_key_picked(void)
{
	int v = !gpio_get_value(PIN_POWER_KEY);
	int prev;

	if (v != pwr_raw) {
		pwr_raw = v;
		pwr_since = get_timer(0);
		return 0;
	}
	if (v == pwr_state || get_timer(pwr_since) < KEY_DEBOUNCE_MS)
		return 0;
	prev = pwr_state;
	pwr_state = v;

	if (v) {
		/* held already when the menu came up: not a press of ours */
		pwr_counts = prev == 0;
		pwr_down_at = get_timer(0);
		return 0;
	}
	return pwr_counts && get_timer(pwr_down_at) < PWR_LONG_MS;
}

/* A key that was just pressed (debounced), or KEY_NONE */
static int key_pressed(void)
{
	int mv = hiby_r1_adc_poll_mv(CONFIG_HIBY_ADC_KEY_CHANNEL);
	int k;

	if (mv == -2)
		return KEY_NONE;
	k = mv < 0 ? KEY_OTHER : key_from_mv(mv);
	if (k != key_raw) {
		key_raw = k;
		key_since = get_timer(0);
		return KEY_NONE;
	}
	if (k == key_state || get_timer(key_since) < KEY_DEBOUNCE_MS)
		return KEY_NONE;
	key_state = k;

	if (!keys_armed) {
		/* a key held since fastboot started must end first */
		if (k == KEY_NONE)
			keys_armed = 1;
		return KEY_NONE;
	}
	return k;
}

/* --- entry points ---------------------------------------------------- */

static void *alloc_uncached(void **mem, size_t size)
{
	*mem = memalign(ARCH_DMA_MINALIGN, size);
	if (!*mem)
		return NULL;
	/* no dirty line of the cached view may land on it later */
	flush_cache((ulong)*mem, size);
	return (void *)CKSEG1ADDR((ulong)*mem);
}

static void release_pins(void)
{
	gpio_direction_output(PIN_PCLK, 0);
}

void hiby_r1_display_init(void)
{
	panel = hiby_panel_probe();
	if (!panel)
		return;
	xres = panel->xres;
	yres = panel->yres;

	fb = alloc_uncached(&fb_mem, xres * yres * sizeof(u16));
	desc = alloc_uncached(&desc_mem, 64);
	if (!fb || !desc) {
		printf("display: out of memory\n");
		goto err_mem;
	}
	draw_screen();

	gpio_direction_output(HIBY_LCD_RST, 0);
	gpio_direction_output(HIBY_LCD_SPI_CS, 1);
	gpio_direction_output(HIBY_LCD_SPI_SCK, 0);
	gpio_direction_output(HIBY_LCD_SPI_MOSI, 0);
	if (axp_setup())
		goto err_mem;

	lcdc_init();
	if (panel->power_on()) {
		printf("display: %s power-on failed\n", panel->name);
		goto err_panel;
	}
	lcd_on = 1;
	if (lcdc_start())
		goto err_start;

	draw_battery();
	printf("display: battery %d%%%s\n", shown_pct,
	       shown_chg ? ", charging" : "");
	backlight_on();
	shown_busy = 0;
	hiby_r1_adc_poll_start();
	gpio_direction_input(PIN_POWER_KEY);
	keys_on = 1;
	return;

err_start:
	hiby_r1_display_off();
	return;
err_panel:
	release_pins();
	cpm_outl(cpm_inl(CPM_CLKGR0) | CPM_CLKGR_LCD, CPM_CLKGR0);
err_mem:
	free(desc_mem);
	free(fb_mem);
	fb = NULL;
	desc = NULL;
}

/* "Working..." while a transfer, flash or erase runs; "Ready" otherwise */
void hiby_r1_display_busy(int busy)
{
	if (!lcd_on || shown_busy == busy)
		return;
	shown_busy = busy;
	draw_status(busy);
}

/*
 * The menu, polled from the fastboot main loop: what to do
 * (FASTBOOT_BOARD_*). Keys are ignored while a command runs.
 */
int hiby_r1_display_poll(int busy)
{
	int i;

	if (!keys_on || busy)
		return FASTBOOT_BOARD_STAY;

	if (get_timer(battery_at) >= BATTERY_POLL_MS)
		draw_battery();

	if (power_key_picked()) {
		draw_status_text(menu[menu_sel].doing, COL_BUSY);
		shown_busy = -1;
		return menu[menu_sel].action;
	}

	switch (key_pressed()) {
	case KEY_UP:
		i = menu_sel;
		menu_sel = (menu_sel + MENU_N - 1) % MENU_N;
		draw_menu_item(i);
		draw_menu_item(menu_sel);
		break;
	case KEY_DOWN:
		i = menu_sel;
		menu_sel = (menu_sel + 1) % MENU_N;
		draw_menu_item(i);
		draw_menu_item(menu_sel);
		break;
	}
	return FASTBOOT_BOARD_STAY;
}

/* As Linux's axp2101 pm_power_off: REG10h bit 0 */
#define AXP_COMM_CFG	0x10
#define  AXP_PWR_OFF	(1 << 0)

void hiby_r1_power_off(void)
{
	hiby_r1_display_off();
	i2c_init(&hiby_pmu_i2c);
	axp_update(AXP_COMM_CFG, AXP_PWR_OFF, AXP_PWR_OFF);
	mdelay(1000);
}

/* soc_fb's disable_fb(), in the order of the board's soc_fb */
void hiby_r1_display_off(void)
{
	/* first, while the PMIC's I2C is as fastboot had it */
	hiby_charge_limit_release();
	if (keys_on) {
		hiby_r1_adc_poll_stop();
		keys_on = 0;
	}
	if (!lcd_on)
		return;
	lcd_on = 0;

	backlight_off();
	lcdc_stop();
	if (panel->pins_off_first) {
		release_pins();
		panel->power_off();
	} else {
		panel->power_off();
		release_pins();
	}
	cpm_outl(cpm_inl(CPM_CLKGR0) | CPM_CLKGR_LCD, CPM_CLKGR0);
}
