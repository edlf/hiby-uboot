/*
 * RAM log, see include/ramlog.h.
 *
 * Two ring buffers of CONFIG_RAMLOG_SIZE bytes (a power of two) behind a
 * small header at CONFIG_RAMLOG_BASE, an uncached (KSEG1) address: every
 * byte is in DRAM the moment it is written, nothing waits in the data
 * cache when the board resets. At start the buffer the previous run wrote
 * becomes "lastlog" and the other one is reused. DRAM keeps its contents
 * through a warm reset well enough for text (the SPL sets the controller
 * up again within a fraction of a second); after a power cut the header
 * does not check out and both logs start empty.
 *
 * SPDX-License-Identifier:	GPL-2.0+
 */

#include <common.h>
#include <ramlog.h>

DECLARE_GLOBAL_DATA_PTR;

#define RAMLOG_MAGIC	0x524c4f47	/* "RLOG" */
#define RAMLOG_MASK	(CONFIG_RAMLOG_SIZE - 1)

struct ramlog_hdr {
	u32 magic;
	u32 check;	/* ~magic */
	u32 cur;	/* buffer this run writes: 0 or 1 */
	u32 pos;	/* bytes written this run (wraps the ring) */
	u32 run;	/* runs since the log was last found invalid */
	u32 last_pos;
	u32 last_run;
};

#define HDR	((volatile struct ramlog_hdr *)CONFIG_RAMLOG_BASE)
#define BUF(n)	((volatile char *)CONFIG_RAMLOG_BASE + 0x100 + \
		 (n) * CONFIG_RAMLOG_SIZE)

/* in .data, not .bss: set before relocation, the copy keeps the value */
static int ramlog_ready = 0x55;		/* 0x55: not set up yet */
static int ramlog_bol = 1;		/* at the start of a line */

static void ramlog_init(void)
{
	volatile struct ramlog_hdr *h = HDR;
	u32 magic = h->magic, check = h->check, cur = h->cur, pos = h->pos;
	int found;

	ramlog_ready = 1;
	found = h->magic == RAMLOG_MAGIC && h->check == ~RAMLOG_MAGIC &&
	    h->cur <= 1;
	if (found) {
		h->last_pos = h->pos;
		h->last_run = h->run;
		h->cur ^= 1;
		h->run++;
	} else {
		h->magic = RAMLOG_MAGIC;
		h->check = ~RAMLOG_MAGIC;
		h->cur = 0;
		h->run = 1;
		h->last_pos = 0;
		h->last_run = 0;
	}
	h->pos = 0;
	if (!found)
		ramlog_printf("ramlog: no log in RAM from before this start "
			      "(header %08x %08x %08x %08x)\n",
			      magic, check, cur, pos);
	ramlog_printf("ramlog: run %u, previous run %u left %u bytes\n",
		      h->run, h->last_run,
		      h->last_pos > CONFIG_RAMLOG_SIZE ?
		      CONFIG_RAMLOG_SIZE : h->last_pos);
}

static void ramlog_store(char c)
{
	volatile struct ramlog_hdr *h = HDR;
	u32 pos = h->pos;

	BUF(h->cur)[pos & RAMLOG_MASK] = c;
	h->pos = pos + 1;
}

static void ramlog_stamp(void)
{
	char t[16];
	char *p;

	/* the timer is safe to read once U-Boot runs from its final place */
	if (!(gd->flags & GD_FLG_RELOC))
		return;
	sprintf(t, "[%7lu] ", get_timer(0));
	for (p = t; *p; p++)
		ramlog_store(*p);
}

void ramlog_putc(char c)
{
	if (ramlog_ready != 1)
		ramlog_init();
	if (ramlog_bol)
		ramlog_stamp();
	ramlog_store(c);
	ramlog_bol = (c == '\n');
}

void ramlog_puts(const char *s)
{
	while (*s)
		ramlog_putc(*s++);
}

void ramlog_printf(const char *fmt, ...)
{
	char buf[256];
	va_list args;

	va_start(args, fmt);
	vscnprintf(buf, sizeof(buf), fmt, args);
	va_end(args);

	if (ramlog_ready != 1)
		ramlog_init();
	if (!ramlog_bol)
		ramlog_putc('\n');
	/* "~" marks lines that went only here, not to the console */
	ramlog_putc('~');
	ramlog_putc(' ');
	ramlog_puts(buf);
}

unsigned long ramlog_size(int last)
{
	volatile struct ramlog_hdr *h = HDR;
	u32 pos;

	if (ramlog_ready != 1)
		ramlog_init();
	pos = last ? h->last_pos : h->pos;
	if (last && !h->last_run)
		return 0;
	return pos > CONFIG_RAMLOG_SIZE ? CONFIG_RAMLOG_SIZE : pos;
}

unsigned long ramlog_copy(int last, char *dst, unsigned long max)
{
	volatile struct ramlog_hdr *h = HDR;
	unsigned long n = ramlog_size(last), i;
	u32 pos = last ? h->last_pos : h->pos;
	u32 start = pos > CONFIG_RAMLOG_SIZE ? (pos & RAMLOG_MASK) : 0;
	volatile char *buf = BUF(last ? h->cur ^ 1 : h->cur);
	unsigned char c;

	if (n > max)
		n = max;
	for (i = 0; i < n; i++) {
		c = buf[(start + i) & RAMLOG_MASK];
		/* a log that sat in DRAM through a reset may have bit errors */
		if ((c < 0x20 && c != '\n' && c != '\t') || c >= 0x7f)
			c = '.';
		dst[i] = c;
	}
	return n;
}
