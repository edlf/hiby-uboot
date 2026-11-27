/*
 * HiBy X1600 battery, for the fastboot screen: see battery.c
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation; either version 2 of
 * the License, or (at your option) any later version.
 */

#ifndef __HIBY_BATTERY_H__
#define __HIBY_BATTERY_H__

/* The level in percent, or -1: no gauge, or no profile loaded yet */
int hiby_battery_percent(void);

/* 1 while charging (or done), as Linux's battery status has it */
int hiby_battery_charging(void);

/*
 * The charge limit, at every battery reading (the level, or -1), and its
 * release, before fastboot ends (reset, boot, power off). Nothing on the R1.
 */
void hiby_charge_limit_poll(int pct);
void hiby_charge_limit_release(void);

#endif /* __HIBY_BATTERY_H__ */
