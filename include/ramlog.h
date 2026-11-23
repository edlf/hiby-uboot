/*
 * RAM log: a copy of everything U-Boot prints, plus trace lines that go
 * only to RAM, kept in a DRAM area nothing else uses. The previous run's
 * log survives a warm reset (watchdog, software reset) as "lastlog", so a
 * board without a serial console can still show what happened before it
 * reset. Read with "fastboot fetch log" / "fastboot oem log".
 *
 * SPDX-License-Identifier:	GPL-2.0+
 */
#ifndef __RAMLOG_H
#define __RAMLOG_H

#if defined(CONFIG_RAMLOG) && !defined(CONFIG_SPL_BUILD)

void ramlog_putc(char c);
void ramlog_puts(const char *s);
/* to the RAM log only, not the console; prefixed with the time in ms */
void ramlog_printf(const char *fmt, ...)
	__attribute__ ((format (__printf__, 1, 2)));
/* bytes in the log (lastlog: the previous run's), 0 if none */
unsigned long ramlog_size(int last);
/* copies the log out, oldest byte first; returns the bytes copied */
unsigned long ramlog_copy(int last, char *dst, unsigned long max);

#else

static inline void ramlog_putc(char c) {}
static inline void ramlog_puts(const char *s) {}
static inline void ramlog_printf(const char *fmt, ...) {}

#endif

#endif /* __RAMLOG_H */
