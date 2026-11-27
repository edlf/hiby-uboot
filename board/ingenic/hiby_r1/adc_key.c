/*
 * HiBy R1 SPL ADC key
 *
 * Samples one X1600 SADC aux channel once at boot. If the voltage falls in
 * the configured window (a key held down), the SPL boots kernel2/rootfs2
 * instead of kernel/rootfs, independent of the OTA flag.
 *
 * Register sequence follows the Ingenic adc_poll HAL
 * (kernel module_drivers/drivers/adc_poll/adc_poll_hal.c) and the stock
 * HiBy R1 SPL.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation; either version 2 of
 * the License, or (at your option) any later version.
 */

#include <common.h>
#include <asm/io.h>
#include <asm/arch/base.h>
#include <asm/arch/cpm.h>

#define ADENA		0x00
#define ADCFG		0x04
#define ADCTRL		0x08
#define ADSTATE		0x0c
#define ADADAT0		0x10
#define ADADAT1		0x14
#define ADCLK		0x20
#define ADSTB		0x24

#define ADENA_AUXEN(ch)	(1 << (ch))
#define ADENA_POWER	(1 << 15)	/* 1 = controller powered down */
#define ADC_ALL_IRQ	0xf
#define ADC_DATA_MASK	0xfff

#define ADC_TIMEOUT	100000

static inline u32 adc_readl(u32 reg)
{
	return readl(SADC_BASE + reg);
}

static inline void adc_writel(u32 val, u32 reg)
{
	writel(val, SADC_BASE + reg);
}

static inline void adc_setbits(u32 reg, u32 bits)
{
	adc_writel(adc_readl(reg) | bits, reg);
}

static inline void adc_clrbits(u32 reg, u32 bits)
{
	adc_writel(adc_readl(reg) & ~bits, reg);
}

static void adc_disable_controller(void)
{
	int count = ADC_TIMEOUT;

	do {
		adc_setbits(ADENA, ADENA_POWER);
	} while (!(adc_readl(ADENA) & ADENA_POWER) && --count);

	if (!count)
		printf("adc hal disable timeout\n");
}

static void adc_enable_controller(void)
{
	int count = ADC_TIMEOUT;

	do {
		adc_clrbits(ADENA, ADENA_POWER);
		mdelay(2);
	} while ((adc_readl(ADENA) & ADENA_POWER) && --count);

	mdelay(2);

	if (!count)
		printf("adc hal enable timeout\n");
}

static int adc_enable_channel(unsigned int ch)
{
	int count = ADC_TIMEOUT;

	do {
		adc_setbits(ADENA, ADENA_AUXEN(ch));
	} while (!(adc_readl(ADENA) & ADENA_AUXEN(ch)) && --count);

	if (!count) {
		printf("adc enable channel %d timeout\n", ch);
		return -1;
	}
	return 0;
}

static void adc_disable_channel(unsigned int ch)
{
	do {
		adc_clrbits(ADENA, ADENA_AUXEN(ch));
	} while (adc_readl(ADENA) & ADENA_AUXEN(ch));
}

static int adc_read_channel(unsigned int ch)
{
	switch (ch) {
	case 0: return adc_readl(ADADAT0) & ADC_DATA_MASK;
	case 1: return (adc_readl(ADADAT0) >> 16) & ADC_DATA_MASK;
	case 2: return adc_readl(ADADAT1) & ADC_DATA_MASK;
	case 3: return (adc_readl(ADADAT1) >> 16) & ADC_DATA_MASK;
	default: return -1;
	}
}

static void adc_init(void)
{
	u32 val;

	/* SADC clock gate; normally already open after reset */
	cpm_outl(cpm_inl(CPM_CLKGR0) & ~CPM_CLKGR_SADC, CPM_CLKGR0);

	adc_disable_controller();
	adc_setbits(ADCTRL, ADC_ALL_IRQ);	/* mask all interrupts */
	adc_setbits(ADSTATE, ADC_ALL_IRQ);	/* clear all ready flags */

	/* clkdiv = 0x77, clkdiv_us = 1, clkdiv_ms = 0x63 (stock values) */
	val = adc_readl(ADCLK);
	val = (val & ~0xff) | 0x77;
	val = (val & ~0xff00) | (1 << 8);
	val = (val & 0xffff) | (0x63 << 16);
	adc_writel(val, ADCLK);

	/* sampling stable time = 1 */
	adc_writel((adc_readl(ADSTB) & ~0xffff) | 1, ADSTB);
}

/*
 * Channel @ch in mV, or -1 on error. Also used by the full U-Boot
 * (display.c: the panel ID on channel 3).
 */
int hiby_r1_adc_read_mv(unsigned int ch)
{
	int raw;

	adc_init();
	adc_enable_controller();
	adc_setbits(ADSTATE, ADC_ALL_IRQ);
	adc_clrbits(ADCTRL, ADC_ALL_IRQ);

	if (adc_enable_channel(ch) < 0) {
		raw = -1;
	} else {
		mdelay(CONFIG_HIBY_ADC_KEY_SAMPLE_MS);
		raw = adc_read_channel(ch);
	}

	adc_disable_channel(ch);
	adc_disable_controller();
	adc_setbits(ADCTRL, ADC_ALL_IRQ);
	adc_setbits(ADSTATE, ADC_ALL_IRQ);

	if (raw < 0)
		return -1;

	return raw * CONFIG_HIBY_ADC_VREF_MV / 4096;
}

#ifndef CONFIG_SPL_BUILD
/*
 * Reads that never wait, for the fastboot menu (display.c), polled from
 * the fastboot main loop: as soc_adc.c does a conversion, the channel is
 * enabled, its ready flag is waited for, then the channel is disabled.
 */
static int poll_started = -1;	/* channel converting, or -1 */
static ulong poll_since;

#define ADC_POLL_TIMEOUT_MS	100

void hiby_r1_adc_poll_start(void)
{
	adc_init();
	adc_enable_controller();
	adc_setbits(ADSTATE, ADC_ALL_IRQ);
	adc_clrbits(ADCTRL, ADC_ALL_IRQ);
	poll_started = -1;
}

/* Channel @ch in mV once converted; -2 while it is converting */
int hiby_r1_adc_poll_mv(unsigned int ch)
{
	int raw;

	if (poll_started == (int)ch) {
		if (adc_readl(ADSTATE) & (1 << ch)) {
			raw = adc_read_channel(ch);
			adc_disable_channel(ch);
			adc_writel(1 << ch, ADSTATE);	/* clear the flag */
			poll_started = -1;
			return raw * CONFIG_HIBY_ADC_VREF_MV / 4096;
		}
		if (get_timer(poll_since) < ADC_POLL_TIMEOUT_MS)
			return -2;
		adc_disable_channel(ch);	/* lost: start again */
	}

	adc_writel(1 << ch, ADSTATE);
	if (adc_enable_channel(ch) < 0)
		return -1;
	poll_started = ch;
	poll_since = get_timer(0);
	return -2;
}

void hiby_r1_adc_poll_stop(void)
{
	if (poll_started >= 0)
		adc_disable_channel(poll_started);
	poll_started = -1;
	adc_disable_controller();
	adc_setbits(ADCTRL, ADC_ALL_IRQ);
	adc_setbits(ADSTATE, ADC_ALL_IRQ);
}
#endif /* !CONFIG_SPL_BUILD */

#ifdef CONFIG_SPL_BUILD

/* Returns the key channel voltage in mV, or -1 on error. */
int hiby_r1_adc_key_read_mv(void)
{
	return hiby_r1_adc_read_mv(CONFIG_HIBY_ADC_KEY_CHANNEL);
}

/* Sample once per boot; both hooks below share the reading. */
static int adc_key_mv(void)
{
	static int mv = -2;	/* -2: not sampled yet (kept in .data) */

	if (mv == -2) {
		mv = hiby_r1_adc_key_read_mv();
		printf("adc key:%d\n", mv);
	}
	return mv;
}

/* Overrides the weak hook in common/spl/spl_sfc_nand_v2.c */
int spl_board_boot_kernel2(void)
{
	int mv = adc_key_mv();

	return mv >= CONFIG_HIBY_ADC_KEY_MIN_MV &&
	       mv <= CONFIG_HIBY_ADC_KEY_MAX_MV;
}

#ifdef CONFIG_HIBY_NORMAL_KEY_MIN_MV
/* Overrides the weak hook in common/spl/spl_sfc_nand_v2.c */
int spl_board_boot_normal(void)
{
	int mv = adc_key_mv();

	return mv >= CONFIG_HIBY_NORMAL_KEY_MIN_MV &&
	       mv <= CONFIG_HIBY_NORMAL_KEY_MAX_MV;
}
#endif

#ifdef CONFIG_HIBY_FASTBOOT_KEY_MIN_MV
/* Overrides the weak hook in common/spl/spl_sfc_nand_v2.c */
int spl_board_boot_uboot(void)
{
	int mv = adc_key_mv();

	return mv >= CONFIG_HIBY_FASTBOOT_KEY_MIN_MV &&
	       mv <= CONFIG_HIBY_FASTBOOT_KEY_MAX_MV;
}
#endif

#endif /* CONFIG_SPL_BUILD */
