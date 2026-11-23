/*
 * HiBy R1 fastboot support (full U-Boot only)
 *
 * - partition_info[] is built at runtime from the burner partition table
 *   the SFC NAND driver read from flash (0x5800), so fastboot uses the same
 *   names and offsets as the SPL. A repeated name gets a numeric suffix
 *   (the stock table names partition 4 "rootfs" again -> "rootfs2").
 * - max-download-size is whatever RAM is free between the download buffer
 *   and U-Boot's relocated malloc/stack area.
 * - an "ota:fastboot" request in the "ota" partition is cleared, so the
 *   next boot (or a power cycle) goes back to Linux.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation; either version 2 of
 * the License, or (at your option) any later version.
 */

#include <common.h>
#include <command.h>
#include <malloc.h>
#include <fastboot.h>
#include <asm/gpio.h>
#include <asm/arch/spinand.h>

DECLARE_GLOBAL_DATA_PTR;

extern struct jz_sfcnand_burner_param jz_sfc_nand_burner_param;

struct partition_info partition_info[PARTITION_NUM];

struct board_info board_info = {
	.product	= "hiby_r1",
	.variant	= "x1600",
	.hardware_ver	= "R1",
	.max_download_size = 0,		/* set in hiby_r1_fastboot_init() */
};

/* Room U-Boot keeps below its relocated code: boot params, malloc, gd/bd, stack */
#define FB_RESERVED_BELOW_UBOOT	(CONFIG_SYS_BOOTPARAMS_LEN + \
				 CONFIG_SYS_MALLOC_LEN + CONFIG_ENV_SIZE + \
				 (1 << 20))
#define FB_PAD_SLACK		2048	/* flash pads the image to a page */

static int name_used(const char *name, int upto)
{
	int i;

	for (i = 0; i < upto; i++)
		if (!strcmp(partition_info[i].pname, name))
			return 1;
	return 0;
}

static void fill_partitions(void)
{
	struct jz_sfcnand_burner_param *p = &jz_sfc_nand_burner_param;
	int i, n = 0;

	memset(partition_info, 0, sizeof(partition_info));

	if (!p->partition || p->partition_num <= 0) {
		printf("fastboot: no partition table found at 0x%x\n",
		       CONFIG_SPIFLASH_PART_OFFSET);
		return;
	}

	for (i = 0; i < p->partition_num && n < PARTITION_NUM; i++, n++) {
		struct partition_info *pi = &partition_info[n];
		const int max = sizeof(pi->pname) - 1;
		int suffix = 2;

		strncpy(pi->pname, p->partition[i].name, max);
		pi->pname[max] = '\0';
		while (name_used(pi->pname, n)) {
			/* "rootfs" -> "rootfs2", "rootfs3", ... */
			snprintf(pi->pname, max + 1, "%.*s%d", max - 1,
				 p->partition[i].name, suffix++);
		}
		pi->offset = p->partition[i].offset;
		pi->size = p->partition[i].size;
		printf("fastboot: %-10s 0x%08lx 0x%08lx\n",
		       pi->pname, pi->offset, pi->size);
	}
}

static void set_max_download_size(void)
{
	ulong top = gd->relocaddr - FB_RESERVED_BELOW_UBOOT;

	if (top > CONFIG_FASTBOOT_BUF_ADDR + FB_PAD_SLACK)
		board_info.max_download_size =
			top - CONFIG_FASTBOOT_BUF_ADDR - FB_PAD_SLACK;
	else
		board_info.max_download_size = 0;

	printf("fastboot: buffer 0x%08x, max-download-size %lu KiB\n",
	       CONFIG_FASTBOOT_BUF_ADDR, board_info.max_download_size >> 10);
}

/*
 * Clear the fastboot request (page 1 of "ota") while keeping page 0, which
 * holds HiBy's A/B record: save page 0, erase the block, write page 0 back.
 */
static void clear_ota_request(void)
{
	const char *flag = CONFIG_SPL_OTA_UBOOT_FLAG;
	const ulong pg = CONFIG_SPL_OTA_UBOOT_FLAG_OFFSET;	/* one page */
	char command[96];
	char *buf;
	ulong off;
	int i;

	for (i = 0; i < PARTITION_NUM; i++)
		if (!strcmp(partition_info[i].pname, CONFIG_SPL_OTA_NAME))
			break;
	if (i == PARTITION_NUM)
		return;
	off = partition_info[i].offset;

	buf = malloc(2 * pg);
	if (!buf)
		return;

	/* pages 0 and 1 */
	sprintf(command, "nand read 0x%lx 0x%lx 0x%lx", (ulong)buf, off, 2 * pg);
	if (run_command(command, 0) || strncmp(buf + pg, flag, strlen(flag)))
		goto out;

	printf("fastboot: clearing %s request (keeping \"%.12s\")\n",
	       flag, buf);
	sprintf(command, "nand erase 0x%lx 0x%x", off,
		CONFIG_SYS_NAND_BLOCK_SIZE);
	if (run_command(command, 0))
		goto out;
	sprintf(command, "nand write 0x%lx 0x%lx 0x%lx", (ulong)buf, off, pg);
	run_command(command, 0);
out:
	free(buf);
}

/*
 * LED: steady purple (red + blue) for as long as U-Boot, i.e. fastboot,
 * runs; blinking purple while a flash, erase or fetch is in progress.
 */
#define LED_BLINK_MS	200	/* on and off phases */

static int led_lit;
static ulong led_since;

static void led_set(int lit)
{
#ifdef CONFIG_HIBY_R1_LED_RED_GPIO
	const int v = lit ? CONFIG_HIBY_R1_LED_ON : !CONFIG_HIBY_R1_LED_ON;

	gpio_direction_output(CONFIG_HIBY_R1_LED_RED_GPIO, v);
	gpio_direction_output(CONFIG_HIBY_R1_LED_BLUE_GPIO, v);
#endif
	led_lit = lit;
	led_since = get_timer(0);
}

static void fastboot_led_on(void)
{
	led_set(1);
}

/* Overrides the weak hooks in drivers/usb/gadget/f_fastboot.c */
void fastboot_led_busy(void)
{
	if (get_timer(led_since) >= LED_BLINK_MS)
		led_set(!led_lit);
}

void fastboot_led_idle(void)
{
	if (!led_lit)
		led_set(1);
}

void fastboot_led_off(void)
{
	led_set(0);
}

void hiby_r1_fastboot_init(void)
{
	fastboot_led_on();
	fill_partitions();
	set_max_download_size();
	clear_ota_request();
}
