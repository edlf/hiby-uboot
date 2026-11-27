/*
 * HiBy X1600 battery, for the fastboot screen (full U-Boot only)
 *
 *  - the level: the CW2015 fuel gauge's percentage, read as Linux's cw2015
 *    module scales it (never written: Linux keeps it set up)
 *  - whether it is charging: from the AXP2101 on the R1, from the MP2731
 *    on the R3 Pro II (CONFIG_HIBY_CHARGER_MP2731)
 *  - the charge limit: the R1's is its charge voltage, which the AXP2101
 *    keeps from Linux, so nothing is done there. The R3 Pro II's is its
 *    driver's, not the chip's, so here the battery is held at 80 % as the
 *    mp2731 module holds it, and charging is given back before fastboot
 *    ends (hiby_charge_limit_release(), from display.c)
 *
 * Everything is on the PMIC's I2C0 (hiby_pmu_i2c). See battery.h.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation; either version 2 of
 * the License, or (at your option) any later version.
 */

#include <common.h>
#include <ingenic_soft_i2c.h>
#include "display.h"
#include "battery.h"

#define CW2015_ADDR		0x62	/* on the PMIC's I2C0 */
#define CW_REG_SOC		0x04	/* percent, then 1/256 percent */
#define CW_REG_CONFIG		0x08
#define  CW_CONFIG_UPDATE_FLG	0x02	/* the battery profile is loaded */
#define CW_REG_HIGH_SOC		0x4f	/* bit 7: full is bits 6..0, not 100 */
#define AXP_STATUS2		0x01	/* 6..5 current direction, 2..0 state */

static int cw_read(u8 reg, u8 *buf, int len)
{
	return i2c_read(&hiby_pmu_i2c, CW2015_ADDR, reg, 1, buf, len);
}

/*
 * The battery in percent as cw_get_capacity() works it out (without its
 * smoothing between readings), or -1: no gauge, or no profile loaded yet
 * (Linux has not run since the battery was connected).
 */
int hiby_battery_percent(void)
{
	static int no_gauge;	/* it did not answer: stop asking */
	u8 soc[2], high, cfg;
	int full, pct;

	if (no_gauge)
		return -1;
	if (cw_read(CW_REG_CONFIG, &cfg, 1)) {
		no_gauge = 1;
		return -1;
	}
	if (!(cfg & CW_CONFIG_UPDATE_FLG))
		return -1;
	if (cw_read(CW_REG_SOC, soc, 2) || cw_read(CW_REG_HIGH_SOC, &high, 1))
		return -1;
	if (soc[0] > 100)
		return -1;

	full = (high & 0x80) ? (high & 0x7f) : 100;
	if (!full)
		full = 100;
	pct = ((soc[0] << 8) + soc[1]) * 100 / (full << 8);
	return pct > 100 ? 100 : pct;
}

#ifdef CONFIG_HIBY_CHARGER_MP2731
/*
 * The R3 Pro II charges from an MP2731 (on the same I2C0, at 0x4b); its
 * AXP2101 does not charge the battery. As the mp2731 module's
 * POWER_SUPPLY_PROP_STATUS (kernel module_drivers/hiby/r3proii/mp2731):
 * charging or full, with an input in (VIN_STAT neither 0, nothing, nor 7,
 * OTG). A charge held off at the limit (CHG_CONFIG 0) reads as not
 * charging, as it is.
 */
#define MP2731_ADDR		0x4b
#define MP2731_REG_CHG		0x04	/* 5..4 CHG_CONFIG: 1 charge, 0 not */
#define  MP2731_CHG_CONFIG	(0x03 << 4)
#define MP2731_REG_STATUS	0x0c	/* 7..5 VIN_STAT, 4..3 CHG_STAT */

static int mp2731_read(u8 reg, u8 *val)
{
	return i2c_read(&hiby_pmu_i2c, MP2731_ADDR, reg, 1, val, 1);
}

/* The cable (VIN_STAT neither 0 nor 7, OTG), and charging or full */
static int mp2731_status(int *online, int *charging)
{
	u8 st;
	int vin;

	if (mp2731_read(MP2731_REG_STATUS, &st))
		return -1;
	vin = (st >> 5) & 0x07;
	*online = vin != 0 && vin != 7;
	*charging = *online && ((st >> 3) & 0x03) != 0;	/* 1 pre, 2 fast, 3 done */
	return 0;
}

int hiby_battery_charging(void)
{
	int online, charging;

	return mp2731_status(&online, &charging) ? 0 : charging;
}

/*
 * The charge limit, as the mp2731 module keeps it (its charge_limit_percent,
 * which the recovery loads at 80): with the cable in, charging stops at
 * CHARGE_LIMIT % with CHG_CONFIG, the input still supplying the system, and
 * starts again CHARGE_LIMIT_HYST % below the level it stopped at. Without a
 * cable charging is never held: the next cable must meet a charger that
 * charges. A hold Linux left (CHG_CONFIG 0 at the limit) is taken over, so
 * it too is released when fastboot ends. The module's 5 minute settling of
 * the level after a stop is left out: fastboot is not where a player sits.
 */
#define CHARGE_LIMIT		80
#define CHARGE_LIMIT_HYST	3

static int limit_held;		/* CHG_CONFIG is 0 and it is ours to undo */
static int limit_at;		/* the level it stopped at */
static int limit_checked;	/* the first poll looked at what Linux left */

static int mp2731_set_charging(int on)
{
	u8 v;

	if (mp2731_read(MP2731_REG_CHG, &v))
		return -1;
	v = (v & ~MP2731_CHG_CONFIG) | ((on ? 1 : 0) << 4);
	return i2c_write(&hiby_pmu_i2c, MP2731_ADDR, MP2731_REG_CHG, 1, &v, 1);
}

void hiby_charge_limit_release(void)
{
	if (!limit_held)
		return;
	if (mp2731_set_charging(1) == 0) {
		limit_held = 0;
		printf("display: charging released\n");
	}
}

void hiby_charge_limit_poll(int pct)
{
	int online, charging;
	u8 v;

	if (mp2731_status(&online, &charging))
		return;
	if (!limit_checked) {
		limit_checked = 1;
		if (!mp2731_read(MP2731_REG_CHG, &v) && !(v & MP2731_CHG_CONFIG)) {
			limit_held = 1;	/* Linux held it: ours now */
			limit_at = pct >= 0 ? pct : CHARGE_LIMIT;
		}
	}
	if (!online) {
		hiby_charge_limit_release();
		return;
	}
	if (pct < 0)
		return;		/* no level to judge by: nothing changes */
	if (!limit_held && pct >= CHARGE_LIMIT) {
		if (mp2731_set_charging(0) == 0) {
			limit_held = 1;
			limit_at = pct;
			printf("display: charging held at %d%% (limit %d%%)\n", pct, CHARGE_LIMIT);
		}
	} else if (limit_held && pct <= (limit_at < CHARGE_LIMIT ? limit_at : CHARGE_LIMIT) - CHARGE_LIMIT_HYST) {
		hiby_charge_limit_release();
	}
}
#else
/* As the axp2101 battery's POWER_SUPPLY_PROP_STATUS: charging (or done) */
int hiby_battery_charging(void)
{
	u8 st;
	int state;

	if (i2c_read(&hiby_pmu_i2c, AXP2101_ADDR, AXP_STATUS2, 1, &st, 1))
		return 0;
	state = st & 0x07;
	return (state >= 1 && state <= 4) ||
	       (state == 5 && ((st >> 5) & 0x03) == 1);
}

/* The R1 keeps its limit in the AXP2101 (its charge voltage): nothing to do */
void hiby_charge_limit_poll(int pct) {}
void hiby_charge_limit_release(void) {}
#endif /* CONFIG_HIBY_CHARGER_MP2731 */

