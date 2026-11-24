/* Ingenic JZ Fastboot Command Explain Function Driver
 *
 *  Copyright (C) 2013 Ingenic Semiconductor Co., LTD.
 *  Sun Jiwei <jwsun@ingenic.cn>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation; either version 2 of
 * the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston,
 * MA 02111-1307 USA
 */

#include <config.h>
#include <malloc.h>
#include <common.h>
#include <errno.h>

#include <version.h>
#include <asm/unaligned.h>
#include <linux/err.h>
#include <linux/usb/ch9.h>
#include <linux/usb/gadget.h>
#include <linux/usb/composite.h>
#include <usb/lin_gadget_compat.h>
#include <g_fastboot.h>

#include <boot_img.h>
#include <mmc.h>
#include <asm/arch/cpm.h>
#include <asm/io.h>
#include <fastboot.h>
#include <fb_mmc.h>
#include <ramlog.h>
#ifdef CONFIG_FASTBOOT_BUF_ADDR
/* Every download goes to this fixed RAM buffer (never malloc'd/freed). */
#define DATA_BUFFER	CONFIG_FASTBOOT_BUF_ADDR
#else
#define DATA_BUFFER	0x30000000
#endif
#define BOOT_START_ADDRESS 0x80f00000
#define RET_LENGTH	64

#define CMD_COUNT	23
#define MMC_BYTE_PER_BLOCK 512
#ifndef PARTITION_NUM
#define PARTITION_NUM 16
#endif
#define	CMD_BUFF_SIZE 128
#define FASTBOOT_PAGE 2048

#define _DEBUG	0

struct boot_img_hdr bootimginfo;
char *boot_buf;
extern struct partition_info partition_info[PARTITION_NUM];
extern struct board_info board_info;
#ifdef CONFIG_FASTBOOT_SFC_NAND
#include <nand.h>
#include <image.h>
#include <linux/crc7.h>
DECLARE_GLOBAL_DATA_PTR;
static int fb_find_partition(const char *name);
static const struct partition_info *fb_fetch_part(const char *name);
static void fb_set_busy(int busy);
extern const char *g_fastboot_serial(void);	/* g_fastboot.c */
/*
 * "version-bootloader": the build, not just the U-Boot release. Boards
 * that share a board directory name themselves with
 * CONFIG_FASTBOOT_BOARD_NAME.
 */
#ifndef CONFIG_FASTBOOT_BOARD_NAME
#define CONFIG_FASTBOOT_BOARD_NAME	CONFIG_SYS_BOARD
#endif
#define FB_BOOTLOADER_VER	PLAIN_VERSION "-" CONFIG_FASTBOOT_BOARD_NAME " " \
				U_BOOT_DATE " " U_BOOT_TIME
#endif
static unsigned int fastboot_flash_session_id;
static int need_boot_kernel;
static int need_reboot_bootloader;
#ifdef CONFIG_FASTBOOT_SFC_NAND
/*
 * "oem bootrom": reset into the X1600 mask ROM's USB boot mode (Ingenic
 * a108:eaef, for the cloner/burner tool). The ROM boots USB once after a
 * watchdog reset when CPM_SLPC holds "BWS" (0x425753) in bits 31..8 with
 * bit 1 set, and clears that bit; U-Boot's reset is a watchdog reset.
 */
#define FB_BOOTROM_USB_FLAG	0x42575302
static int need_reboot_bootrom;
#endif
static char boot_kernel_cmd[CMD_BUFF_SIZE];

struct fastboot_common {
	struct usb_gadget	*gadget;	/*Copy of cdev->gadget*/
	struct usb_function     usb_function;
	struct usb_ep		*bulk_out;
	struct usb_ep		*bulk_in;
	struct usb_ep		*ep0;		/*Copy of gadget->ep0*/
	struct usb_request	*ep0req;	/*Copy of cdev->req*/
	bool			enum_done;
};

struct fastboot_dev {
	struct usb_request	*cmd_req;
	struct usb_request	*data_req;
	struct usb_request	*ret_req;
	char			*data_buf;
	char			*ret_buf;
	bool			explain_cmd_status;
	u32			data_length;

	int			locked;
	int			leave;
	int			cmd_count;
};


struct fastboot_common *fastboot_common;

/* the fastboot interface's name (iInterface); a board config may set it */
#ifndef CONFIG_G_FASTBOOT_INTERFACE_NAME
#define CONFIG_G_FASTBOOT_INTERFACE_NAME	"Google, Inc"
#endif
static const char fastboot_name[] = CONFIG_G_FASTBOOT_INTERFACE_NAME;

static struct usb_string fastboot_string_desc[] = {
	[0].s = fastboot_name,
	{}
};

static struct usb_gadget_strings  fastboot_gadget_string = {
	.language = 0x0409, /* en-us */
	.strings = fastboot_string_desc,
};

static struct usb_gadget_strings  *fastboot_gadget_string_tab[] = {
	&fastboot_gadget_string,
	NULL,
};

static struct usb_interface_descriptor fastboot_intf_desc = {
	.bLength =              sizeof(fastboot_intf_desc),
	.bDescriptorType =      USB_DT_INTERFACE,
	.bNumEndpoints =        2,

	.bInterfaceClass =	USB_CLASS_VENDOR_SPEC,
	.bInterfaceSubClass =	0x42,
	.bInterfaceProtocol =	0x03,
	/* .iInterface = DYNAMIC */
};

static struct usb_endpoint_descriptor fastboot_fs_bulk_in_desc = {
	.bLength                = USB_DT_ENDPOINT_SIZE,
	.bDescriptorType        = USB_DT_ENDPOINT,
	.bEndpointAddress       = USB_DIR_IN,
	.bmAttributes           = USB_ENDPOINT_XFER_BULK,
};

static struct usb_endpoint_descriptor fastboot_fs_bulk_out_desc = {
	.bLength                = USB_DT_ENDPOINT_SIZE,
	.bDescriptorType        = USB_DT_ENDPOINT,
	.bEndpointAddress       = USB_DIR_OUT,
	.bmAttributes           = USB_ENDPOINT_XFER_BULK,
};

static struct usb_endpoint_descriptor fastboot_hs_bulk_in_desc = {
	.bLength                = USB_DT_ENDPOINT_SIZE,
	.bDescriptorType        = USB_DT_ENDPOINT,
	.bEndpointAddress       = USB_DIR_IN,
	.bmAttributes           = USB_ENDPOINT_XFER_BULK,
	.wMaxPacketSize         = __constant_cpu_to_le16(512),
};

static struct usb_endpoint_descriptor fastboot_hs_bulk_out_desc = {
	.bLength                = USB_DT_ENDPOINT_SIZE,
	.bDescriptorType        = USB_DT_ENDPOINT,
	.bEndpointAddress       = USB_DIR_OUT,
	.bmAttributes           = USB_ENDPOINT_XFER_BULK,
	.wMaxPacketSize         = __constant_cpu_to_le16(512),
};

static struct usb_descriptor_header *fastboot_fs_desc_header[] = {
	(struct usb_descriptor_header *) &fastboot_intf_desc,
	(struct usb_descriptor_header *) &fastboot_fs_bulk_out_desc,
	(struct usb_descriptor_header *) &fastboot_fs_bulk_in_desc,
	NULL,
};

static struct usb_descriptor_header *fastboot_hs_desc_header[] = {
	(struct usb_descriptor_header *) &fastboot_intf_desc,
	(struct usb_descriptor_header *) &fastboot_hs_bulk_out_desc,
	(struct usb_descriptor_header *) &fastboot_hs_bulk_in_desc,
	NULL,
};

static inline struct fastboot_common *fastboot_from_func(struct usb_function *f)
{
	return container_of(f, struct fastboot_common, usb_function);
}

static int fastboot_bind(struct usb_configuration *c, struct usb_function *f)
{
	struct fastboot_common	*fastboot_common = fastboot_from_func(f);
	struct usb_gadget	*gadget = c->cdev->gadget;
	struct usb_ep		*ep;
	int			i;

	fastboot_common->gadget = gadget;
	fastboot_common->ep0 = c->cdev->gadget->ep0;
	fastboot_common->ep0req = c->cdev->req;

	/* New interface */
	i = usb_interface_id(c, f);
	if (i < 0)
		return i;
	fastboot_intf_desc.bInterfaceNumber = i;

	ep = usb_ep_autoconfig(gadget, &fastboot_fs_bulk_in_desc);
	if (!ep)
		goto autoconf_fail;
	ep->driver_data = fastboot_common;
	fastboot_common->bulk_in = ep;

	ep = usb_ep_autoconfig(gadget, &fastboot_fs_bulk_out_desc);
	if (!ep)
		goto autoconf_fail;
	ep->driver_data = fastboot_common;
	fastboot_common->bulk_out = ep;

	if (gadget_is_dualspeed(c->cdev->gadget)) {
		fastboot_hs_bulk_in_desc.bEndpointAddress =
			fastboot_fs_bulk_in_desc.bEndpointAddress;
		fastboot_hs_bulk_out_desc.bEndpointAddress =
			fastboot_fs_bulk_out_desc.bEndpointAddress;
	}

	return 0;
autoconf_fail:
	printf("unable to autoconfigure all endpoints\n");
	return -ENOTSUPP;
}

static void fastboot_unbind(struct usb_configuration *c, struct usb_function *f)
{
	struct fastboot_common	*fastboot_common = fastboot_from_func(f);

	debug("unbind\n");

	kfree(fastboot_common);
	return;
}

static int fastboot_set_alt(struct usb_function *f, unsigned intf, unsigned alt)
{
	struct fastboot_common *fastboot_common = fastboot_from_func(f);
	struct usb_ep *in_ep = fastboot_common->bulk_in;
	struct usb_ep *out_ep = fastboot_common->bulk_out;
	const struct usb_endpoint_descriptor *epdesc = NULL;
	int status = 0;

	debug("set interface %d alt %d\n",intf,alt);

	/*Bulk in endpoint set alt*/
	if (in_ep->driver_data) {
		debug("set alt when ep in enable\n");
		status = usb_ep_disable(in_ep);
		if (status) {
			printf("usb disable ep in failed\n");
			goto failed;
		}
		in_ep->driver_data = NULL;
	}

	epdesc = ep_choose(fastboot_common->gadget, &fastboot_hs_bulk_in_desc,
			&fastboot_fs_bulk_in_desc);

	status = usb_ep_enable(in_ep, epdesc);
	if (status < 0) {
		printf("usb enable ep in failed\n");
		goto failed;
	}

	in_ep->driver_data = fastboot_common;

	/*Bulk out endpoint set alt*/
	if (out_ep->driver_data) {
		debug("set alt when ep out enable\n");
		status = usb_ep_disable(out_ep);
		if (status) {
			printf("usb disable ep out failed\n");
			goto failed;
		}

		out_ep->driver_data = NULL;
	}

	epdesc = ep_choose(fastboot_common->gadget, &fastboot_hs_bulk_out_desc,
			&fastboot_fs_bulk_out_desc);

	status = usb_ep_enable(out_ep, epdesc);
	if (status < 0) {
		printf("usb enable ep out failed\n");
		goto failed;
	}

	out_ep->driver_data = fastboot_common;
	fastboot_common->enum_done = 1;
	ramlog_printf("fastboot: configured by host\n");
failed:
	return status;
}

/* TODO: is this really what we need here? */
static void fastboot_disable(struct usb_function *f)
{
	return;
}

	static int
fastboot_handle(struct usb_function *f, const struct usb_ctrlrequest *ctrl)
{
	struct usb_gadget *gadget = f->config->cdev->gadget;
	struct fastboot_common *fastboot_common = f->config->cdev->req->context;
	struct usb_request *req = fastboot_common->ep0req;
#if 0
	u16 wLength = le16_to_cpu(ctrl->wLength);
	u16 wValue = le16_to_cpu(ctrl->wValue);
	u16 wIndex = le16_to_cpu(ctrl->wIndex);
#endif
	int length = 0;

	req->length = 0;
	if (length >= 0) {
		length = usb_ep_queue(gadget->ep0, req, 0);
		if (length) {
			debug("ep_queue --> %d\n", length);
			req->status = 0;
		}
	}
	return length;
}

static int fastboot_bind_config(struct usb_configuration *c)
{
	int status;

	fastboot_common = calloc(sizeof(*fastboot_common), 1);
	if (!fastboot_common)
		return -ENOMEM;

	fastboot_common->usb_function.name = "fastboot";
	fastboot_common->usb_function.hs_descriptors = fastboot_hs_desc_header;
	fastboot_common->usb_function.descriptors = fastboot_fs_desc_header;
	fastboot_common->usb_function.bind = fastboot_bind;
	fastboot_common->usb_function.unbind = fastboot_unbind;
	fastboot_common->usb_function.set_alt = fastboot_set_alt;
	fastboot_common->usb_function.disable = fastboot_disable;
	fastboot_common->usb_function.strings = fastboot_gadget_string_tab,
		fastboot_common->usb_function.setup = fastboot_handle,

		status = usb_add_function(c, &fastboot_common->usb_function);
	if (status)
		free(fastboot_common);

	return status;
}

int fastboot_add(struct usb_configuration *c)
{
	int id;

	id = usb_string_id(c->cdev);
	if (id < 0)
		return id;
	fastboot_string_desc[0].id = id;
	fastboot_intf_desc.iInterface = id;

	debug("%s: cdev: 0x%p gadget:0x%p gadget->ep0: 0x%p\n", __func__,
			c->cdev, c->cdev->gadget, c->cdev->gadget->ep0);

	return fastboot_bind_config(c);
}

static int fastboot_cmd_return(struct fastboot_dev *fastboot,
		void (*complete)(struct usb_ep *ep, struct usb_request *req),
		int status, unsigned actual)
{
	int	ret;

	fastboot->ret_req->buf = fastboot->ret_buf;
	fastboot->ret_req->length = strlen(fastboot->ret_buf);
	fastboot->ret_req->complete = complete;
	fastboot->ret_req->status = status;
	fastboot->ret_req->actual = actual;
	fastboot->ret_req->context = fastboot;

	ret = usb_ep_queue(fastboot_common->bulk_in, fastboot->ret_req, 0);
	if (ret) {
		printf("%s: Error in usb_ep_queue,%d\n", __func__, ret);
		return ret;
	}
	return 0;
}

static void return_buf(struct fastboot_dev *fastboot,
		void (*complete)(struct usb_ep *ep, struct usb_request *req))
{
	u32	actual = 0;
	int	status = 0;

	fastboot_cmd_return(fastboot, complete, status, actual);
}

static void return_complete(struct usb_ep *ep, struct usb_request *req)
{
	struct fastboot_dev *fastboot = req->context;
	memset(fastboot->ret_buf, 0, 64);
	debug("return status is over\n");
	fastboot->explain_cmd_status = 1;
}

#ifdef CONFIG_FASTBOOT_SFC_NAND
/*
 * Failure replies. fb_fail() records why the current command failed (and
 * prints it on the console); fb_reply_fail() sends it as "FAIL<reason>",
 * preceded by an "INFO<text>" line when fb_info is set, which the host
 * prints as "(bootloader) <text>". A reply is at most 64 bytes.
 */
#define FB_MSG_MAX	(RET_LENGTH - 5)	/* after "FAIL"/"INFO", before NUL */

static char fb_reason[FB_MSG_MAX + 1];
static char fb_info[FB_MSG_MAX + 1];
static char fb_final[RET_LENGTH];

static void fb_clear_reason(void)
{
	fb_reason[0] = '\0';
	fb_info[0] = '\0';
}

/* No snprintf here (CONFIG_SYS_VSNPRINTF is off): callers keep %s short. */
static void fb_fail(const char *fmt, ...)
{
	char tmp[256];
	va_list ap;

	va_start(ap, fmt);
	vsprintf(tmp, fmt, ap);
	va_end(ap);
	strncpy(fb_reason, tmp, FB_MSG_MAX);
	fb_reason[FB_MSG_MAX] = '\0';
	printf("fastboot: %s\n", tmp);
}

/* "no partition 'name'", plus the partition names as an INFO line. */
static void fb_no_partition(const char *name)
{
	size_t n, l;
	int i;

	fb_fail("no partition '%.24s'", name);
	strcpy(fb_info, "parts:");
	n = strlen(fb_info);
	for (i = 0; i < PARTITION_NUM; i++) {
		l = strlen(partition_info[i].pname);
		if (!l || n + 1 + l > FB_MSG_MAX)
			continue;
		fb_info[n++] = ' ';
		memcpy(fb_info + n, partition_info[i].pname, l + 1);
		n += l;
	}
}

static void fb_info_sent(struct usb_ep *ep, struct usb_request *req)
{
	struct fastboot_dev *fastboot = req->context;

	strcpy(fastboot->ret_buf, fb_final);
	return_buf(fastboot, return_complete);
}

static void fb_reply_fail(struct fastboot_dev *fastboot)
{
	strcpy(fb_final, "FAIL");
	strcat(fb_final, fb_reason[0] ? fb_reason : "failed");

	if (fb_info[0]) {
		strcpy(fastboot->ret_buf, "INFO");
		strcat(fastboot->ret_buf, fb_info);
		fb_info[0] = '\0';
		fastboot_cmd_return(fastboot, fb_info_sent, 0, 0);
		return;
	}
	strcpy(fastboot->ret_buf, fb_final);
	return_buf(fastboot, return_complete);
}

/*
 * Multi-line replies: fb_lines[] go out as "INFO..." one after the other
 * (each queued once the previous one has been sent), then fb_final.
 */
#define FB_MAX_LINES	(2 * PARTITION_NUM + 12)

static char fb_lines[FB_MAX_LINES][RET_LENGTH];
static int fb_nlines, fb_line;
static int fb_multiline;	/* the getvar reply is fb_lines[] + fb_final */

static void fb_lines_sent(struct usb_ep *ep, struct usb_request *req);

static void fb_lines_step(struct fastboot_dev *fastboot)
{
	if (fb_line < fb_nlines) {
		strcpy(fastboot->ret_buf, fb_lines[fb_line++]);
		fastboot_cmd_return(fastboot, fb_lines_sent, 0, 0);
		return;
	}
	strcpy(fastboot->ret_buf, fb_final);
	return_buf(fastboot, return_complete);
}

static void fb_lines_sent(struct usb_ep *ep, struct usb_request *req)
{
	fb_lines_step(req->context);
}

static void fb_add_line(const char *name, ulong off, ulong size,
			const char *note)
{
	if (fb_nlines < FB_MAX_LINES)
		sprintf(fb_lines[fb_nlines++], "INFO%-10.15s 0x%08lx 0x%08lx%s",
			name, off, size, note);
}

/* An INFO line of free text, cut to fit one reply. */
static void fb_add_text(const char *fmt, ...)
{
	char tmp[256];
	va_list ap;

	if (fb_nlines >= FB_MAX_LINES)
		return;
	va_start(ap, fmt);
	vsprintf(tmp, fmt, ap);
	va_end(ap);
	strcpy(fb_lines[fb_nlines], "INFO");
	strncat(fb_lines[fb_nlines], tmp, FB_MSG_MAX);
	fb_nlines++;
}

static void fb_send_lines(struct fastboot_dev *fastboot, const char *final)
{
	strcpy(fb_final, final);
	fb_line = 0;
	fb_lines_step(fastboot);
}

/*
 * "getvar all": the host prints every INFO line as "(bootloader) ...".
 * Replaces the SDK's fixed list, which used Android partition names and
 * never sent its final OKAY (the host waited forever).
 */
static void fb_getvar_all(struct fastboot_dev *fastboot)
{
	const struct partition_info *pi;
	int i;

	fb_nlines = 0;
	fb_add_text("version-bootloader: %s", FB_BOOTLOADER_VER);
	fb_add_text("version-hardware: %s", board_info.hardware_ver);
	fb_add_text("product: %s", board_info.product);
	fb_add_text("variant: %s", board_info.variant);
	fb_add_text("serialno: %s", g_fastboot_serial());
	fb_add_text("max-download-size: 0x%08lx", board_info.max_download_size);
	fb_add_text("max-fetch-size: 0x%08lx", board_info.max_download_size);
	for (i = 0; i < PARTITION_NUM; i++) {
		if (!partition_info[i].pname[0])
			continue;
		fb_add_text("partition-size:%.15s: 0x%08lx",
			    partition_info[i].pname, partition_info[i].size);
		fb_add_text("partition-type:%.15s: raw", partition_info[i].pname);
	}
	pi = fb_fetch_part("nand");
	if (pi)
		fb_add_text("partition-size:nand: 0x%08lx (good blocks, fetch only)",
			    pi->size);
	pi = fb_fetch_part("nand_raw");
	if (pi)
		fb_add_text("partition-size:nand_raw: 0x%08lx (fetch only)",
			    pi->size);
	pi = fb_fetch_part("bootrom");
	if (pi)
		fb_add_text("partition-size:bootrom: 0x%08lx (fetch only)", pi->size);
	fb_send_lines(fastboot, "OKAY");
}

/* "oem partitions": the partition table, one line per partition. */
static void fb_oem_partitions(struct fastboot_dev *fastboot)
{
	const struct partition_info *pi;
	int i;

	fb_nlines = 0;
	fb_add_text("%-10s %-10s %s", "name", "offset", "size");
	for (i = 0; i < PARTITION_NUM; i++)
		if (partition_info[i].pname[0])
			fb_add_line(partition_info[i].pname,
				    partition_info[i].offset,
				    partition_info[i].size, "");
	pi = fb_fetch_part("nand");
	if (pi)
		fb_add_line(pi->pname, 0, pi->size, " fetch, good blks");
	pi = fb_fetch_part("nand_raw");
	if (pi)
		fb_add_line(pi->pname, 0, pi->size, " fetch, all blks");
	pi = fb_fetch_part("bootrom");
	if (pi)
		fb_add_line(pi->pname, 0, pi->size, " fetch only");
#ifdef CONFIG_RAMLOG
	fb_add_line("log", 0, ramlog_size(0), " fetch, RAM log");
	fb_add_line("lastlog", 0, ramlog_size(1), " fetch, before reset");
#endif
	fb_send_lines(fastboot, "OKAY");
}
#endif /* CONFIG_FASTBOOT_SFC_NAND */

static void download_data_fail(struct fastboot_dev *fastboot)
{
	strcpy(fastboot->ret_buf, "FAILED");
	return_buf(fastboot, return_complete);
}

static void download_data_over(struct usb_ep *ep,
		struct usb_request *req)
{
	struct fastboot_dev *fastboot = req->context;

	usb_ep_free_request(fastboot_common->bulk_out, fastboot->data_req);
	fastboot->data_req = NULL;
#ifdef CONFIG_FASTBOOT_SFC_NAND
	fb_set_busy(0);
#endif
	strcpy(fastboot->ret_buf, "OKAY");
	return_buf(fastboot, return_complete);
}

static void handle_download_complete(struct usb_ep *ep,
		struct usb_request *req)
{
	struct fastboot_dev *fastboot = req->context;

	debug("%s %d\n",__func__, __LINE__);
	fastboot->data_req = usb_ep_alloc_request(fastboot_common->bulk_out, 0);
	if (!fastboot->data_req) {
		printf("%s: Error, usb alloc request\n", __func__);
		return;
	}

	fastboot->cmd_req = NULL;
	fastboot->data_req->buf = fastboot->data_buf;
	fastboot->data_req->length = fastboot->data_length;
	debug("length is 0x%x = %u\n", fastboot->data_req->length, fastboot->data_req->length);
	fastboot->data_req->complete = download_data_over;
	fastboot->data_req->status = 0;
	fastboot->data_req->actual = 0;
	fastboot->data_req->context = fastboot;
	usb_ep_queue(fastboot_common->bulk_out, fastboot->data_req, 0);
}

#define CONFIG_FASTBOOT_BOOTLOADER_VER	U_BOOT_VERSION
#define CONFIG_FASTBOOT_BASEBAND_VER	"N/A"
#define	CONFIG_FASTBOOT_CDMA_VER	"N/A"

#define	CONFIG_FASTBOOT_SERIALNO	"0123456789abcdef"
#define	CONFIG_FASTBOOT_SECURE		"no"
#define	CONFIG_FASTBOOT_UNLOCKED	"yes"
#define	CONFIG_FASTBOOT_UART_ON		"NO"
#define CONFIG_FASTBOOT_PART_TYPE_BOOTLOADER	"emmc"
#define CONFIG_FASTBOOT_PART_TYPE_RECOVERY	"emmc"
#define CONFIG_FASTBOOT_PART_TYPE_BOOT		"emmc"
#define CONFIG_FASTBOOT_PART_TYPE_SYSTEM	"ext4"
#define CONFIG_FASTBOOT_PART_TYPE_CACHE		"ext4"
#define CONFIG_FASTBOOT_PART_TYPE_USERDATA	"ext4"

static unsigned long get_partition_size(char *partition_name)
{
	int i;
	for(i = 0; i < PARTITION_NUM ; i ++) {
		if(!strncmp(partition_info[i].pname,
			    partition_name,
			    (strlen(partition_info[i].pname)))) {
			break;
		}
	}
	if(i == PARTITION_NUM) {
		printf("There is not a partition named : %s\n", partition_name);
		return 0;
	}

	return partition_info[i].size;
}

static int get_all_var(struct fastboot_dev *fastboot)
{
	char partition_name[RET_LENGTH];
	unsigned long size;

	switch (fastboot->cmd_count) {
		case 0:
			strcpy(fastboot->ret_buf, "INFO");
			strcat(fastboot->ret_buf, "version-bootloader: ");
			strcat(fastboot->ret_buf, CONFIG_FASTBOOT_BOOTLOADER_VER);
			break;
		case 1:
			strcpy(fastboot->ret_buf, "INFO");
			strcat(fastboot->ret_buf, "version-baseband: ");
			strcat(fastboot->ret_buf, CONFIG_FASTBOOT_BASEBAND_VER);
			break;
		case 2:
			strcpy(fastboot->ret_buf, "INFO");
			strcat(fastboot->ret_buf, "version-hardware: ");
			strcat(fastboot->ret_buf, board_info.hardware_ver);
			break;
		case 3:
			strcpy(fastboot->ret_buf, "INFO");
			strcat(fastboot->ret_buf, "version-cdma: ");
			strcat(fastboot->ret_buf, CONFIG_FASTBOOT_CDMA_VER);
			break;
		case 4:
			strcpy(fastboot->ret_buf, "INFO");
			strcat(fastboot->ret_buf, "variant: ");
			strcat(fastboot->ret_buf, board_info.variant);
			break;
		case 5:
			strcpy(fastboot->ret_buf, "INFO");
			strcat(fastboot->ret_buf, "serialno: ");
			strcat(fastboot->ret_buf, CONFIG_FASTBOOT_SERIALNO);
			break;
		case 6:
			strcpy(fastboot->ret_buf, "INFO");
			strcat(fastboot->ret_buf, "product: ");
			strcat(fastboot->ret_buf, board_info.product);
			break;
		case 7:
			strcpy(fastboot->ret_buf, "INFO");
			strcat(fastboot->ret_buf, "secure: ");
			strcat(fastboot->ret_buf, CONFIG_FASTBOOT_SECURE);
			break;
		case 8:
			strcpy(fastboot->ret_buf, "INFO");
			strcat(fastboot->ret_buf, "unlocked: ");
			strcat(fastboot->ret_buf, CONFIG_FASTBOOT_UNLOCKED);
			break;
		case 9:
			strcpy(fastboot->ret_buf, "INFO");
			strcat(fastboot->ret_buf, "uart-on: ");
			strcat(fastboot->ret_buf, CONFIG_FASTBOOT_UART_ON);
			break;
		case 10:
			strcpy(fastboot->ret_buf, "INFO");
			strcat(fastboot->ret_buf, "partition-size:bootloader: ");
			strcpy(partition_name, "bootloader\0");
		        size = get_partition_size(partition_name);
			sprintf(fastboot->ret_buf, "%s0x%08lx", fastboot->ret_buf, size);
			break;
		case 11:
			strcpy(fastboot->ret_buf, "INFO");
			strcat(fastboot->ret_buf, "partition-type:bootloader: ");
			strcat(fastboot->ret_buf, CONFIG_FASTBOOT_PART_TYPE_BOOTLOADER);
			break;
		case 12:
			strcpy(fastboot->ret_buf, "INFO");
			strcat(fastboot->ret_buf, "partition-size:recovery: ");
			strcpy(partition_name, "recovery\0");
		        size = get_partition_size(partition_name);
			sprintf(fastboot->ret_buf, "%s0x%08lx", fastboot->ret_buf, size);
			break;
		case 13:
			strcpy(fastboot->ret_buf, "INFO");
			strcat(fastboot->ret_buf, "partition-type:recovery: ");
			strcat(fastboot->ret_buf, CONFIG_FASTBOOT_PART_TYPE_RECOVERY);
			break;
		case 14:
			strcpy(fastboot->ret_buf, "INFO");
			strcat(fastboot->ret_buf, "partition-size:boot: ");
			strcpy(partition_name, "boot\0");
		        size = get_partition_size(partition_name);
			sprintf(fastboot->ret_buf, "%s0x%08lx", fastboot->ret_buf, size);
			break;
		case 15:
			strcpy(fastboot->ret_buf, "INFO");
			strcat(fastboot->ret_buf, "partition-type:boot: ");
			strcat(fastboot->ret_buf, CONFIG_FASTBOOT_PART_TYPE_BOOT);
			break;
		case 16:
			strcpy(fastboot->ret_buf, "INFO");
			strcat(fastboot->ret_buf, "partition-size:system: ");
			strcpy(partition_name, "system\0");
		        size = get_partition_size(partition_name);
			sprintf(fastboot->ret_buf, "%s0x%08lx", fastboot->ret_buf, size);
			break;
		case 17:
			strcpy(fastboot->ret_buf, "INFO");
			strcat(fastboot->ret_buf, "partition-type:system: ");
			strcat(fastboot->ret_buf, CONFIG_FASTBOOT_PART_TYPE_SYSTEM);
			break;
		case 18:
			strcpy(fastboot->ret_buf, "INFO");
			strcat(fastboot->ret_buf, "partition-size:cache: ");
			strcpy(partition_name, "cache\0");
		        size = get_partition_size(partition_name);
			sprintf(fastboot->ret_buf, "%s0x%08lx", fastboot->ret_buf, size);
			break;
		case 19:
			strcpy(fastboot->ret_buf, "INFO");
			strcat(fastboot->ret_buf, "partition-type:cache: ");
			strcat(fastboot->ret_buf, CONFIG_FASTBOOT_PART_TYPE_CACHE);
			break;
		case 20:
			strcpy(fastboot->ret_buf, "INFO");
			strcat(fastboot->ret_buf, "partition-size:userdata: ");
			strcpy(partition_name, "data\0");
		        size = get_partition_size(partition_name);
			sprintf(fastboot->ret_buf, "%s0x%08lx", fastboot->ret_buf, size);
			break;
		case 21:
			strcpy(fastboot->ret_buf, "INFO");
			strcat(fastboot->ret_buf, "partition-type:userdata: ");
			strcat(fastboot->ret_buf, CONFIG_FASTBOOT_PART_TYPE_USERDATA);
			break;
		case 22:
			strcpy(fastboot->ret_buf, "INFO");
			strcat(fastboot->ret_buf, "max-download-size: ");
			sprintf(fastboot->ret_buf, "%s0x%08lx", fastboot->ret_buf, board_info.max_download_size);
			break;
		case 23:
			strcpy(fastboot->ret_buf, "OKAY");
			break;
		case 24:
			strcpy(fastboot->ret_buf, "INFO");
			break;
		default:
			printf("%s:Error the command display over\n", __func__);
			return -1;
	}
	return 0;
}

static void return_getvar_all_complete(struct usb_ep *ep,
		struct usb_request *req)
{
	struct fastboot_dev *fastboot = req->context;

	debug("%s %d\n",__func__, __LINE__);
	memset(fastboot->ret_buf, 0, 64);
	if(fastboot->cmd_count < CMD_COUNT) {
		fastboot->cmd_count++;
		if (get_all_var(fastboot))
			strcpy(fastboot->ret_buf, "FAIL");

		debug("%s %d\n",__func__, __LINE__);
		return_buf(fastboot, return_getvar_all_complete);
	} else {
		fastboot->cmd_count = 0;
		strcpy(fastboot->ret_buf, "OKAY");
		debug("return status is over\n");
		fastboot->explain_cmd_status = 1;
	}
}

static void __maybe_unused explain_cmd_getvar_all(struct fastboot_dev *fastboot)
{
	fastboot->cmd_count = 0;
	if (get_all_var(fastboot))
		strcpy(fastboot->ret_buf, "FAIL");

	return_buf(fastboot, return_getvar_all_complete);
}

static int handle_cmd_getvar(struct fastboot_dev *fastboot)
{
#ifdef CONFIG_FASTBOOT_SFC_NAND
	{
		/* exact matches on the board's own partition names */
		const char *var = (const char *)fastboot->cmd_req->buf + 7;

		if (!strcmp(var, "version-bootloader")) {
			strcpy(fastboot->ret_buf, "OKAY" FB_BOOTLOADER_VER);
			return 0;
		}

		if (!strcmp(var, "serialno")) {
			strcpy(fastboot->ret_buf, "OKAY");
			strcat(fastboot->ret_buf, g_fastboot_serial());
			return 0;
		}

		/*
		 * "partitions": one INFO line per partition (name, offset,
		 * size), the fetch-only pseudo-partitions, then the count.
		 */
		if (!strcmp(var, "partitions")) {
			const struct partition_info *pi;
			int i, n = 0;

			fb_nlines = 0;
			for (i = 0; i < PARTITION_NUM; i++) {
				if (!partition_info[i].pname[0])
					continue;
				fb_add_line(partition_info[i].pname,
					    partition_info[i].offset,
					    partition_info[i].size, "");
				n++;
			}
			pi = fb_fetch_part("nand");
			if (pi)
				fb_add_line(pi->pname, 0, pi->size, " fetch, good blks");
			pi = fb_fetch_part("nand_raw");
			if (pi)
				fb_add_line(pi->pname, 0, pi->size, " fetch, all blks");
			sprintf(fb_final, "OKAY%d", n);
			fb_multiline = 1;
			return 0;
		}

		if (!strcmp(var, "max-fetch-size")) {
			sprintf(fastboot->ret_buf, "OKAY0x%08lx",
				board_info.max_download_size);
			return 0;
		}
		if (!strncmp(var, "partition-size:", 15)) {
			const struct partition_info *pi = fb_fetch_part(var + 15);

			if (!pi) {
				fb_no_partition(var + 15);
				return -1;
			}
			sprintf(fastboot->ret_buf, "OKAY0x%08lx", pi->size);
			return 0;
		}
		if (!strncmp(var, "partition-type:", 15)) {
			if (!fb_fetch_part(var + 15)) {
				fb_no_partition(var + 15);
				return -1;
			}
			strcpy(fastboot->ret_buf, "OKAYraw");
			return 0;
		}
		if (!strncmp(var, "has-slot:", 9) ||
		    !strncmp(var, "is-logical:", 11)) {
			strcpy(fastboot->ret_buf, "OKAYno");
			return 0;
		}
	}
#endif
	if (strstr(fastboot->cmd_req->buf, "version-bootloader")) {
		strcpy(fastboot->ret_buf, "OKAY");
		strcat(fastboot->ret_buf, CONFIG_FASTBOOT_BOOTLOADER_VER);
		return 0;
	}

	if (strstr(fastboot->cmd_req->buf, "version-baseband")) {
		strcpy(fastboot->ret_buf, "OKAY");
		strcat(fastboot->ret_buf, CONFIG_FASTBOOT_BASEBAND_VER);
		return 0;
	}

	if (strstr(fastboot->cmd_req->buf, "version-hardware")) {
		strcpy(fastboot->ret_buf, "OKAY");
		strcat(fastboot->ret_buf, board_info.hardware_ver);
		return 0;
	}

	if (strstr(fastboot->cmd_req->buf, "version-cdma")) {
		strcpy(fastboot->ret_buf, "OKAY");
		strcat(fastboot->ret_buf, CONFIG_FASTBOOT_CDMA_VER);
		return 0;
	}

	if (strstr(fastboot->cmd_req->buf, "variant")) {
		strcpy(fastboot->ret_buf, "OKAY");
		strcat(fastboot->ret_buf, board_info.variant);
		return 0;
	}

	if (strstr(fastboot->cmd_req->buf, "serialno")) {
		strcpy(fastboot->ret_buf, "OKAY");
		strcat(fastboot->ret_buf, CONFIG_FASTBOOT_SERIALNO);
		return 0;
	}

	if (strstr(fastboot->cmd_req->buf, "product")) {
		strcpy(fastboot->ret_buf, "OKAY");
		strcat(fastboot->ret_buf, board_info.product);
		return 0;
	}

	if (strstr(fastboot->cmd_req->buf, "secure")) {
		strcpy(fastboot->ret_buf, "OKAY");
		strcat(fastboot->ret_buf, CONFIG_FASTBOOT_SECURE);
		return 0;
	}

	if (strstr(fastboot->cmd_req->buf, "unlocked")) {
		strcpy(fastboot->ret_buf, "OKAY");
		strcat(fastboot->ret_buf, CONFIG_FASTBOOT_UNLOCKED);
		return 0;
	}

	if (strstr(fastboot->cmd_req->buf, "uart-on")) {
		strcpy(fastboot->ret_buf, "OKAY");
		strcat(fastboot->ret_buf, CONFIG_FASTBOOT_UART_ON);
		return 0;
	}

	if (strstr(fastboot->cmd_req->buf, "partition-size:bootloader")) {
		char partition_name[RET_LENGTH] = "bootloader\0";
		sprintf(fastboot->ret_buf, "OKAY0x%08lx", get_partition_size(partition_name));
		return 0;
	}

	if (strstr(fastboot->cmd_req->buf, "partition-type:bootloader")) {
		strcpy(fastboot->ret_buf, "OKAY");
		strcat(fastboot->ret_buf, CONFIG_FASTBOOT_PART_TYPE_BOOTLOADER);
		return 0;
	}

	if (strstr(fastboot->cmd_req->buf, "partition-size:recovery")) {
		char partition_name[RET_LENGTH] = "recovery\0";
		sprintf(fastboot->ret_buf, "OKAY0x%08lx", get_partition_size(partition_name));
		return 0;
	}

	if (strstr(fastboot->cmd_req->buf, "partition-type:recovery")) {
		strcpy(fastboot->ret_buf, "OKAY");
		strcat(fastboot->ret_buf, CONFIG_FASTBOOT_PART_TYPE_RECOVERY);
		return 0;
	}

	if (strstr(fastboot->cmd_req->buf, "partition-size:boot")) {
		char partition_name[RET_LENGTH] = "boot\0";
		sprintf(fastboot->ret_buf, "OKAY0x%08lx", get_partition_size(partition_name));
		return 0;
	}

	if (strstr(fastboot->cmd_req->buf, "partition-type:boot")) {
		strcpy(fastboot->ret_buf, "OKAY");
		strcat(fastboot->ret_buf, CONFIG_FASTBOOT_PART_TYPE_BOOT);
		return 0;
	}

	if (strstr(fastboot->cmd_req->buf, "partition-size:system")) {
		char partition_name[RET_LENGTH] = "system\0";
		sprintf(fastboot->ret_buf, "OKAY0x%08lx", get_partition_size(partition_name));
		return 0;
	}

	if (strstr(fastboot->cmd_req->buf, "partition-type:system")) {
		strcpy(fastboot->ret_buf, "OKAY");
		strcat(fastboot->ret_buf, CONFIG_FASTBOOT_PART_TYPE_SYSTEM);
		return 0;
	}

	if (strstr(fastboot->cmd_req->buf, "partition-size:cache")) {
		char partition_name[RET_LENGTH] = "cache\0";
		sprintf(fastboot->ret_buf, "OKAY0x%08lx", get_partition_size(partition_name));
		return 0;
	}

	if (strstr(fastboot->cmd_req->buf, "partition-type:cache")) {
		strcpy(fastboot->ret_buf, "OKAY");
		strcat(fastboot->ret_buf, CONFIG_FASTBOOT_PART_TYPE_CACHE);
		return 0;
	}

	if (strstr(fastboot->cmd_req->buf, "partition-size:userdata")) {
		char partition_name[RET_LENGTH] = "data\0";
		sprintf(fastboot->ret_buf, "OKAY0x%08lx", get_partition_size(partition_name));
		return 0;
	}

	if (strstr(fastboot->cmd_req->buf, "partition-type:userdata")) {
		strcpy(fastboot->ret_buf, "OKAY");
		strcat(fastboot->ret_buf, CONFIG_FASTBOOT_PART_TYPE_USERDATA);
		return 0;
	}

	if (strstr(fastboot->cmd_req->buf, "max-download-size")) {
		sprintf(fastboot->ret_buf, "OKAY0x%08lx", board_info.max_download_size);
		fastboot_flash_session_id = 0;
		return 0;
	}

	printf("%s:Error, The variant is not recongized\n", __func__);
	return -1;
}

static void explain_cmd_getvar(struct fastboot_dev *fastboot)
{
#ifdef CONFIG_FASTBOOT_SFC_NAND
	fb_multiline = 0;
	if (handle_cmd_getvar(fastboot)) {
		if (!fb_reason[0])
			fb_fail("unknown variable '%.40s'",
				(char *)fastboot->cmd_req->buf + 7);
		fb_reply_fail(fastboot);
		return;
	}
	if (fb_multiline) {
		fb_line = 0;
		fb_lines_step(fastboot);
		return;
	}
#else
	if (handle_cmd_getvar(fastboot))
		strcpy(fastboot->ret_buf, "FAIL");
#endif

	return_buf(fastboot, return_complete);
}

static int handle_cmd_download(struct fastboot_dev *fastboot)
{
#ifdef CONFIG_FASTBOOT_BUF_ADDR
	if (fastboot->data_length > board_info.max_download_size) {
		fb_fail("%u bytes is over max-download-size %lu",
			fastboot->data_length, board_info.max_download_size);
		return -ENOMEM;
	}
	fastboot->data_buf = (unsigned char *)DATA_BUFFER;
	return 0;
#endif
	if (fastboot->data_buf && (fastboot->data_buf != DATA_BUFFER)) {
		free(fastboot->data_buf);
		debug("%s:Warning, fastboot->data_buf is not empty,"
				" now free it\n",
				__func__);
	}
	if (fastboot->data_length < CONFIG_SYS_MALLOC_LEN) {
		fastboot->data_buf = malloc(fastboot->data_length);
		if (!fastboot->data_buf) {
			printf("%s: malloc for data buffer failed\n",__func__);
			return -ENOMEM;
		}
	} else {
		debug("Please add the TLB function\n");
		fastboot->data_buf = (unsigned char*)DATA_BUFFER;
		return 0;
	}
	memset(fastboot->data_buf, 0, fastboot->data_length);
	return 0;
}

/* Called after flash command explain complete */
static void flash_download_data(struct fastboot_dev *fastboot)
{
#ifdef CONFIG_FASTBOOT_BUF_ADDR
	fastboot->data_length = 0;
	fastboot->data_buf = NULL;
#else
	memset(fastboot->data_buf, 0, fastboot->data_length);
	fastboot->data_length = 0;
	free(fastboot->data_buf);
	fastboot->data_buf = NULL;
#endif
}

static int explain_cmd_download(struct fastboot_dev *fastboot)
{
	char	buf_length[9];
	int	status = 0;
	u32	actual = 0;
	int	ret;

	strcpy(fastboot->ret_buf, "DATA");

	memcpy(buf_length, (char *)(fastboot->cmd_req->buf + 9), 8);
	buf_length[8] = '\0';

	strcat(fastboot->ret_buf, buf_length);

	fastboot->data_length = (u32)simple_strtol(buf_length, NULL, 16);

	ret = handle_cmd_download(fastboot);
	if (ret != 0) {
		printf("%s: Error in the handle download cmd:%d\n", __func__, ret);
#ifdef CONFIG_FASTBOOT_SFC_NAND
		/* answer, or the host and this loop wait for each other forever */
		fb_reply_fail(fastboot);
		return 0;
#endif
		return ret;
	}

#ifdef CONFIG_FASTBOOT_SFC_NAND
	fb_set_busy(1);		/* until download_data_over() */
#endif
	ret = fastboot_cmd_return(fastboot, handle_download_complete, status, actual);
	if (ret != 0) {
		printf("%s: Error in the fastboot_cmd_return,%d\n",__func__, ret);
		return ret;
	}
	return 0;
}

int nand_flash(unsigned char *pt_name,struct fastboot_dev *fastboot)
{
	int curr_device = 0;
	u32 length = fastboot->data_length;
	void *databuf = (void *)fastboot->data_buf;

	char command[128];
	memset(command,0,128);
	sprintf(command,"nand_zm write %s 0 %x %p",pt_name,length,databuf);
	printf("command:%s\n",command);
	run_command(command,0);

	return 0;
}

int spi_flash(u32 offset, struct fastboot_dev *fastboot)
{
	u32 length = fastboot->data_length;
	void *databuf = (void *)fastboot->data_buf;

	char command[128];
	memset(command,0,128);
	/* sfcnor write  [src:nor flash addr] [bytes:0x..] [dst:der address] [force erase:1, nor erase:0] */
	sprintf(command,"sfcnor write 0x%x 0x%x 0x%x 1",offset,length,databuf);
	printf("command:%s\n",command);
	run_command(command,0);

	return 0;
}

#ifdef CONFIG_FASTBOOT_SFC_NAND
/*
 * SPI NAND (SFC) backend: raw partition writes through the "nand" command,
 * which skips bad blocks. Partition names are matched exactly.
 */
#define FB_NAND_PAGE		2048
#define FB_PART_TABLE_SIZE	0x400

static int fb_find_partition(const char *name)
{
	int i;

	for (i = 0; i < PARTITION_NUM; i++)
		if (partition_info[i].pname[0] &&
		    !strcmp(partition_info[i].pname, name))
			return i;
	return -1;
}

/*
 * Activity LED hooks for the board: fastboot_led_busy() is called often
 * (per NAND block, and per polling loop pass while a transfer runs) and
 * may blink; fastboot_led_idle() restores the steady fastboot colour;
 * fastboot_led_off() turns it off before "boot" hands over to a kernel.
 */
void __weak fastboot_led_busy(void) {}
void __weak fastboot_led_idle(void) {}
void __weak fastboot_led_off(void) {}

static int fb_busy;	/* a download/fetch transfer is in progress */

static void fb_set_busy(int busy)
{
	fb_busy = busy;
	if (!busy)
		fastboot_led_idle();
}

/* Erase every good block of @part (bad blocks are left alone). */
static int fb_sfcnand_erase(const struct partition_info *part)
{
	nand_info_t *nand = &nand_info[0];
	const u32 bs = nand->erasesize;
	loff_t phys;
	int ret;

	printf("erase: %s 0x%lx+0x%lx\n", part->pname, part->offset, part->size);
	for (phys = part->offset; phys < part->offset + part->size; phys += bs) {
		fastboot_led_busy();
		if (nand_block_isbad(nand, phys))
			continue;
		ret = nand_erase(nand, phys, bs);
		if (ret) {
			fb_fail("erase error %d at 0x%lx", ret, (ulong)phys);
			return -1;
		}
	}
	return 0;
}

/*
 * Write @len (page aligned) bytes at the start of @part into its good
 * blocks in order, the layout "nand write" produces.
 */
static int fb_sfcnand_write(const struct partition_info *part,
			    unsigned char *buf, u32 len)
{
	nand_info_t *nand = &nand_info[0];
	const u32 bs = nand->erasesize;
	const loff_t end = part->offset + part->size;
	loff_t phys = part->offset;
	int ret;

	printf("write: %s 0x%x bytes\n", part->pname, len);
	while (len) {
		size_t n = min(len, bs);

		fastboot_led_busy();
		if (phys >= end) {
			fb_fail("out of good blocks in %s", part->pname);
			return -1;
		}
		if (nand_block_isbad(nand, phys)) {
			phys += bs;
			continue;
		}
		ret = nand_write(nand, phys, &n, buf);
		if (ret) {
			fb_fail("write error %d at 0x%lx", ret, (ulong)phys);
			return -1;
		}
		buf += n;
		len -= n;
		phys += bs;
	}
	return 0;
}

/*
 * Burner partition table at CONFIG_SPIFLASH_PART_OFFSET, read by the SPL
 * (get_partitions()) and U-Boot: "nand", a partition count, then entries of
 * name[32], size, offset, mask_flags, manager_mode (little endian).
 */
#define FB_PT_HDR		8
#define FB_PT_ENTRY		48
#define FB_PT_MAX		((FB_PART_TABLE_SIZE - FB_PT_HDR) / FB_PT_ENTRY)

static u32 fb_le32(const unsigned char *p)
{
	return p[0] | (p[1] << 8) | (p[2] << 16) | ((u32)p[3] << 24);
}

static int fb_part_table_blank(const unsigned char *t)
{
	int i;

	for (i = 0; i < FB_PART_TABLE_SIZE; i++)
		if (t[i] != 0xff)
			return 0;
	return 1;
}

/*
 * A table the SPL can boot from: magic, 1..FB_PT_MAX named entries with a
 * size, one at offset 0 that holds @boot_len bytes, and a kernel entry.
 */
static int fb_part_table_valid(const unsigned char *t, u32 boot_len)
{
	int n = (int)fb_le32(t + 4);
	int i, boot = 0, kernel = 0;

	if (memcmp(t, "nand", 4) || n < 1 || n > FB_PT_MAX)
		return 0;

	for (i = 0; i < n; i++) {
		const unsigned char *e = t + FB_PT_HDR + i * FB_PT_ENTRY;
		const char *name = (const char *)e;
		u32 size = fb_le32(e + 32);
		u32 off = fb_le32(e + 36);

		if (!name[0] || !memchr(name, '\0', 32) || !size)
			return 0;
		if (off == 0 && size >= boot_len)
			boot = 1;
#ifdef CONFIG_SPL_OS_NAME
		if (!strcmp(name, CONFIG_SPL_OS_NAME))
			kernel = 1;
#else
		kernel = 1;
#endif
	}
	return boot && kernel;
}

/*
 * Make sure an image for the partition at offset 0 (SPL + U-Boot) carries
 * a partition table the SPL can boot from, so the write cannot leave the
 * board without one:
 *  - an image too short to reach the table is padded with 0xff up to it;
 *  - a blank table gets the one currently in flash, if that one is valid;
 *  - a table the image brings must be valid itself.
 * @len is updated when the image is padded.
 */
static int fb_prepare_part_table(unsigned char *buf, u32 *len)
{
	const u32 end = CONFIG_SPIFLASH_PART_OFFSET + FB_PART_TABLE_SIZE;
	unsigned char *img = buf + CONFIG_SPIFLASH_PART_OFFSET;
	unsigned char *page;
	char command[128];
	int ok;

	if (*len < end) {
		memset(buf + *len, 0xff, end - *len);
		*len = end;
	}

	if (!fb_part_table_blank(img)) {
		if (!fb_part_table_valid(img, *len)) {
			fb_fail("the image's partition table is not valid");
			return -1;
		}
		printf("flash: using the image's partition table\n");
		return 0;
	}

	page = malloc(FB_NAND_PAGE);
	if (!page) {
		fb_fail("out of memory");
		return -1;
	}
	sprintf(command, "nand read 0x%lx 0x%x 0x%x", (ulong)page,
		CONFIG_SPIFLASH_PART_OFFSET, FB_NAND_PAGE);
	ok = !run_command(command, 0) && fb_part_table_valid(page, *len);
	if (ok)
		memcpy(img, page, FB_PART_TABLE_SIZE);
	free(page);

	if (!ok) {
		fb_fail("no valid partition table in the image or in flash");
		return -1;
	}
	printf("flash: kept the partition table from flash\n");
	return 0;
}

/*
 * The partition at offset 0 is what the boot ROM loads: refuse an image it
 * would reject, so a wrong file cannot leave the board without an SPL.
 * Header as tools/ingenic-tools/spi_checksum.c writes it for SFC NAND:
 *   0..7   06 05 04 03 02 55 aa 55
 *   9      crc7 of the SPL from byte 2048 to its length
 *   12..13 SPL length
 *   14     crc7 of the 256-byte params block at 0x100
 *   15     crc7 of bytes 0..14
 */
#define FB_SPL_SKIP		2048
#define FB_SPL_PARAMS		0x100

static int fb_check_spl_image(const unsigned char *b, u32 len)
{
	static const unsigned char magic[8] = {
		0x06, 0x05, 0x04, 0x03, 0x02, 0x55, 0xaa, 0x55 };
	u32 spl_len;

	if (len < FB_SPL_SKIP || memcmp(b, magic, sizeof(magic))) {
		fb_fail("not an SPI NAND boot image (bad magic)");
		return -1;
	}
	spl_len = b[12] | (b[13] << 8);
	if (spl_len <= FB_SPL_SKIP || spl_len > len ||
	    spl_len > CONFIG_SPIFLASH_PART_OFFSET) {
		fb_fail("bad SPL length %u (image %u bytes)", spl_len, len);
		return -1;
	}
	if (crc7(0, b, 15) != b[15]) {
		fb_fail("SPL header checksum mismatch");
		return -1;
	}
	if (crc7(0, b + FB_SPL_PARAMS, 256) != b[14]) {
		fb_fail("SPL params checksum mismatch");
		return -1;
	}
	if (crc7(0, b + FB_SPL_SKIP, spl_len - FB_SPL_SKIP) != b[9]) {
		fb_fail("SPL checksum mismatch");
		return -1;
	}
	printf("flash: boot image ok (SPL %u bytes, image %u bytes)\n",
	       spl_len, len);
	return 0;
}

#ifdef CONFIG_SPL_OS_BOOT
/*
 * The partitions the SPL boots Linux from. It reads the legacy uImage
 * header, copies header + data to load - 64 and jumps to the entry point,
 * without checking CRCs or decompressing. Refuse an image it cannot boot.
 */
#define FB_SPL_RAM_START	0x80008000	/* SPL code and stack below */

static int fb_is_kernel_part(const char *name)
{
	if (!strcmp(name, CONFIG_SPL_OS_NAME))
		return 1;
#ifdef CONFIG_SPL_OS_NAME2
	if (!strcmp(name, CONFIG_SPL_OS_NAME2))
		return 1;
#endif
	return 0;
}

static int fb_check_kernel_image(const unsigned char *buf, u32 len)
{
	const image_header_t *hdr = (const image_header_t *)buf;
	ulong start, end, ep, ram_end;
	u32 size;

	if (len < image_get_header_size() || !image_check_magic(hdr)) {
		fb_fail("not a uImage (bad magic)");
		return -1;
	}
	if (!image_check_hcrc(hdr)) {
		fb_fail("uImage header checksum mismatch");
		return -1;
	}
	size = image_get_data_size(hdr);
	if (size > len - image_get_header_size()) {
		fb_fail("uImage truncated (%u of %u data bytes)",
			len - (u32)image_get_header_size(), size);
		return -1;
	}
	if (!image_check_dcrc(hdr)) {
		fb_fail("uImage data checksum mismatch");
		return -1;
	}
	if (image_get_os(hdr) != IH_OS_LINUX ||
	    image_get_arch(hdr) != IH_ARCH_MIPS ||
	    image_get_type(hdr) != IH_TYPE_KERNEL ||
	    image_get_comp(hdr) != IH_COMP_NONE) {
		fb_fail("need os/arch/type/comp 5/5/2/0, got %d/%d/%d/%d",
			image_get_os(hdr), image_get_arch(hdr),
			image_get_type(hdr), image_get_comp(hdr));
		return -1;
	}

	/* where the SPL puts it: header + data at load - 64 */
	start = image_get_load(hdr) - image_get_header_size();
	end = image_get_load(hdr) + size;
	ep = image_get_ep(hdr);
	ram_end = CONFIG_SYS_SDRAM_BASE + gd->ram_size;
	if (start < FB_SPL_RAM_START || end > ram_end || end < start) {
		fb_fail("load 0x%08lx-0x%08lx outside 0x%08x-0x%08lx",
			start, end, FB_SPL_RAM_START, ram_end);
		return -1;
	}
	if (ep < image_get_load(hdr) || ep >= end) {
		fb_fail("uImage entry 0x%08lx outside the image", ep);
		return -1;
	}

	printf("flash: kernel image ok (%s, %u bytes, load 0x%08lx)\n",
	       image_get_name(hdr), size, (ulong)image_get_load(hdr));
	return 0;
}
#endif /* CONFIG_SPL_OS_BOOT */

static int fb_sfcnand_flash(const struct partition_info *part,
			    struct fastboot_dev *fastboot)
{
	unsigned char *buf = (unsigned char *)fastboot->data_buf;
	u32 len = fastboot->data_length;
	u32 padded;

	if (!len) {
		fb_fail("nothing downloaded: run download first");
		return -1;
	}

	/* boot partition: valid SPL, and a partition table it can boot from */
	if (part->offset == 0 &&
	    (fb_check_spl_image(buf, len) || fb_prepare_part_table(buf, &len)))
		return -1;

#ifdef CONFIG_SPL_OS_BOOT
	/* kernel partitions: an image the SPL can boot */
	if (fb_is_kernel_part(part->pname) && fb_check_kernel_image(buf, len))
		return -1;
#endif

	padded = (len + FB_NAND_PAGE - 1) & ~(FB_NAND_PAGE - 1);
	if (padded > part->size) {
		fb_fail("%u bytes too big for %s (%lu)",
			len, part->pname, part->size);
		return -1;
	}

	memset(buf + len, 0xff, padded - len);

	if (fb_sfcnand_erase(part))
		return -1;

	return fb_sfcnand_write(part, buf, padded);
}

/*
 * Read @len bytes at logical offset @off of @part. Offsets count good
 * blocks from the start of the partition, the layout "nand write" (and
 * Linux nandwrite) produce; space past the last good block reads as 0xff.
 * Uncorrectable ECC errors are reported on the console and the data is
 * still returned.
 */
static int fb_sfcnand_read(const struct partition_info *part, u32 off,
			   u32 len, unsigned char *buf)
{
	nand_info_t *nand = &nand_info[0];
	const u32 bs = nand->erasesize;
	loff_t phys = part->offset;
	const loff_t end = part->offset + part->size;
	u32 skip = off;

	if (!bs)
		return -1;

	/* first good block holding logical offset @off */
	while (phys < end) {
		if (nand_block_isbad(nand, phys)) {
			phys += bs;
			continue;
		}
		if (skip < bs)
			break;
		skip -= bs;
		phys += bs;
	}

	while (len) {
		size_t n, rd;
		int ret;

		fastboot_led_busy();
		if (phys >= end) {
			memset(buf, 0xff, len);	/* lost to bad blocks */
			break;
		}
		if (nand_block_isbad(nand, phys)) {
			phys += bs;
			continue;
		}

		n = min(len, bs - skip);
		rd = n;
		ret = nand_read(nand, phys + skip, &rd, buf);
		if (ret && ret != -EUCLEAN)
			printf("fetch: read error %d at 0x%lx\n", ret,
			       (ulong)(phys + skip));

		buf += n;
		len -= n;
		skip = 0;
		phys += bs;
	}
	return 0;
}

static void fetch_data_over(struct usb_ep *ep, struct usb_request *req)
{
	struct fastboot_dev *fastboot = req->context;
	int status = req->status;

	usb_ep_free_request(fastboot_common->bulk_in, req);
	fastboot->data_req = NULL;
	fastboot->data_length = 0;	/* "flash" must not reuse fetched data */
	fb_set_busy(0);
	strcpy(fastboot->ret_buf, status ? "FAILtransfer error" : "OKAY");
	return_buf(fastboot, return_complete);
}

/* Called once "DATAxxxxxxxx" has gone out: send the data itself. */
static void fetch_send_data(struct usb_ep *ep, struct usb_request *req)
{
	struct fastboot_dev *fastboot = req->context;
	struct usb_request *dreq;

	dreq = usb_ep_alloc_request(fastboot_common->bulk_in, 0);
	if (!dreq) {
		strcpy(fastboot->ret_buf, "FAILout of requests");
		return_buf(fastboot, return_complete);
		return;
	}
	fastboot->data_req = dreq;
	dreq->buf = fastboot->data_buf;
	dreq->length = fastboot->data_length;
	dreq->complete = fetch_data_over;
	dreq->status = 0;
	dreq->actual = 0;
	dreq->context = fastboot;
	usb_ep_queue(fastboot_common->bulk_in, dreq, 0);
}

/*
 * Read-only pseudo-partition "nand": the whole chip's good blocks, back to
 * back, for "fastboot fetch nand flash.bin" (like nanddump's skipbad). Bad
 * blocks are left out, so after the first one the offsets in the dump are
 * no longer flash offsets; the bad blocks are listed on the console. Data
 * only, no OOB. Its size, the good blocks, is counted from the chip.
 */
static struct partition_info fb_nand_part = { .pname = "nand" };

/* The whole chip as a partition: the physical span fb_sfcnand_read() walks. */
static struct partition_info fb_nand_chip = { .pname = "nand" };

/* Good bytes on the chip; @list prints the bad blocks on the console. */
static ulong fb_nand_good_size(int list)
{
	nand_info_t *nand = &nand_info[0];
	const ulong bs = nand->erasesize;
	ulong off, good = 0;
	int bad = 0;

	if (!bs)
		return 0;
	for (off = 0; off < (ulong)nand->size; off += bs) {
		if (nand_block_isbad(nand, off)) {
			if (list)
				printf("fetch: nand: bad block at 0x%08lx skipped\n",
				       off);
			bad++;
		} else {
			good += bs;
		}
	}
	if (list)
		printf("fetch: nand: %d bad blocks, dump is 0x%08lx bytes\n",
		       bad, good);
	return good;
}

/*
 * Read-only pseudo-partition "nand_raw": the whole chip at physical
 * offsets, bad blocks included and read as the chip returns them, for
 * "fastboot fetch nand_raw flash.bin". Byte N of the dump is byte N of the
 * flash. Bad blocks are listed on the console; their data may carry
 * uncorrectable ECC errors, which are reported and do not stop the dump.
 * Data only, no OOB.
 */
static struct partition_info fb_nand_raw_part = { .pname = "nand_raw" };

static void fb_nand_read_physical(u32 off, u32 len, unsigned char *buf)
{
	nand_info_t *nand = &nand_info[0];
	const u32 bs = nand->erasesize;

	while (len) {
		u32 blk = off & ~(bs - 1);
		size_t n = min(len, bs - (off - blk));
		size_t rd = n;
		int ret;

		fastboot_led_busy();
		if (off == blk && nand_block_isbad(nand, blk))
			printf("fetch: nand_raw: bad block at 0x%08x, read anyway\n",
			       blk);
		ret = nand_read(nand, off, &rd, buf);
		if (ret && ret != -EUCLEAN)
			printf("fetch: nand_raw: read error %d in block 0x%08x\n",
			       ret, blk);
		buf += n;
		off += n;
		len -= n;
	}
}

#ifdef CONFIG_FASTBOOT_BOOTROM_BASE
/*
 * Read-only pseudo-partition "bootrom": the SoC's mask ROM, for
 * "fastboot fetch bootrom rom.bin". Not in partition_info[], so flash and
 * erase cannot name it. CONFIG_FASTBOOT_BOOTROM_SIZE is the size found
 * on the X1600 (32 KiB; above it the ROM repeats): a read the bus does
 * not complete would stop U-Boot for good.
 */
static const struct partition_info fb_bootrom_part = {
	.pname	= "bootrom",
	.offset	= 0,
	.size	= CONFIG_FASTBOOT_BOOTROM_SIZE,
};

/* 32-bit reads, the safe access width for a ROM; the LED ticks every 4 KiB. */
static void fb_read_bootrom(ulong off, ulong size, u32 *dst)
{
	const volatile u32 *src =
		(const volatile u32 *)(CONFIG_FASTBOOT_BOOTROM_BASE + off);
	ulong i;

	for (i = 0; i < (size + 3) / 4; i++) {
		if (!((off + i * 4) & 0xfff))
			fastboot_led_busy();
		dst[i] = src[i];
	}
}
#endif /* CONFIG_FASTBOOT_BOOTROM_BASE */

#ifdef CONFIG_RAMLOG
/*
 * Read-only pseudo-partitions "log" (this run) and "lastlog" (the run
 * before the last warm reset): the RAM log, see include/ramlog.h. Their
 * size is the log's size when fetch starts.
 */
static struct partition_info fb_log_part = { .pname = "log" };
static struct partition_info fb_lastlog_part = { .pname = "lastlog" };

/* what an empty log reads as: fetch needs a size, and this says why */
#define FB_NO_LASTLOG	"no log from before the last reset: the RAM log " \
			"was not found (power-on start, or RAM lost its " \
			"contents); see the ramlog line of \"log\"\n"
#define FB_NO_LOG	"log is empty\n"

static ulong fb_log_size(int last)
{
	ulong n = ramlog_size(last);

	return n ? n : strlen(last ? FB_NO_LASTLOG : FB_NO_LOG);
}

static int fb_is_log(const struct partition_info *part)
{
	return part == &fb_log_part || part == &fb_lastlog_part;
}

static void fb_read_log(const struct partition_info *part, ulong off,
			ulong size, char *dst)
{
	int last = part == &fb_lastlog_part;
	ulong n;

	if (!ramlog_size(last)) {
		const char *msg = last ? FB_NO_LASTLOG : FB_NO_LOG;

		memset(dst, ' ', off + size);
		n = strlen(msg);
		memcpy(dst, msg, n < off + size ? n : off + size);
		if (off)
			memmove(dst, dst + off, size);
		return;
	}
	n = ramlog_copy(last, dst, off + size);

	if (n < off + size)		/* the ring wrapped meanwhile */
		memset(dst + n, ' ', off + size - n);
	if (off)
		memmove(dst, dst + off, size);
}

/*
 * "oem log" / "oem lastlog": the end of the log as INFO lines, as many as
 * one reply list holds; long lines are split. The whole log is copied to
 * the download buffer first.
 */
static void fb_oem_log(struct fastboot_dev *fastboot, int last)
{
	char *buf = (char *)DATA_BUFFER;
	ulong n = ramlog_copy(last, buf, CONFIG_RAMLOG_SIZE);
	ulong start[FB_MAX_LINES], len[FB_MAX_LINES];
	ulong i, s = 0;
	int cnt = 0, first, k;

	/* split into segments, keep the last FB_MAX_LINES - 1 in a ring */
	for (i = 0; i <= n; i++) {
		if (i == n || buf[i] == '\n' || i - s == FB_MSG_MAX) {
			if (i > s || (i < n && buf[i] == '\n')) {
				start[cnt % (FB_MAX_LINES - 1)] = s;
				len[cnt % (FB_MAX_LINES - 1)] = i - s;
				cnt++;
			}
			s = (i < n && buf[i] == '\n') ? i + 1 : i;
		}
	}

	fb_nlines = 0;
	if (!n) {
		fb_add_text(last ? "no log from before the last reset (power-on"
			    " start, or RAM lost); see oem log" :
			    "log is empty");
	} else {
		if (cnt > FB_MAX_LINES - 1)
			fb_add_text("... last %d lines of %lu bytes, "
				    "fetch %s for all", FB_MAX_LINES - 1, n,
				    last ? "lastlog" : "log");
		first = cnt > FB_MAX_LINES - 1 ? cnt - (FB_MAX_LINES - 1) : 0;
		for (k = first; k < cnt; k++) {
			char *l = fb_lines[fb_nlines++];
			int j = k % (FB_MAX_LINES - 1);

			strcpy(l, "INFO");
			memcpy(l + 4, buf + start[j], len[j]);
			l[4 + len[j]] = '\0';
		}
	}
	fb_send_lines(fastboot, "OKAY");
}
#endif /* CONFIG_RAMLOG */

/* Everything fetch can read: the partitions, the whole chip, the boot ROM. */
static const struct partition_info *fb_fetch_part(const char *name)
{
	int i = fb_find_partition(name);

	if (i >= 0)
		return &partition_info[i];
	if (!strcmp(name, fb_nand_part.pname)) {
		fb_nand_part.size = fb_nand_good_size(0);
		return fb_nand_part.size ? &fb_nand_part : NULL;
	}
	if (!strcmp(name, fb_nand_raw_part.pname)) {
		fb_nand_raw_part.size = (ulong)nand_info[0].size;
		return fb_nand_raw_part.size ? &fb_nand_raw_part : NULL;
	}
#ifdef CONFIG_FASTBOOT_BOOTROM_BASE
	if (!strcmp(name, fb_bootrom_part.pname))
		return &fb_bootrom_part;
#endif
#ifdef CONFIG_RAMLOG
	if (!strcmp(name, fb_log_part.pname)) {
		fb_log_part.size = fb_log_size(0);
		return &fb_log_part;
	}
	if (!strcmp(name, fb_lastlog_part.pname)) {
		fb_lastlog_part.size = fb_log_size(1);
		return &fb_lastlog_part;
	}
#endif
	return NULL;
}

/* fetch:<partition>[:<offset>[:<size>]] (host: fastboot fetch) */
static void explain_cmd_fetch(struct fastboot_dev *fastboot)
{
	char arg[64];
	char *name = arg, *p, *e;
	const struct partition_info *part;
	ulong off = 0, size;

	/* the command buffer is freed when this returns: copy it */
	strncpy(arg, (char *)fastboot->cmd_req->buf + 6, sizeof(arg) - 1);
	arg[sizeof(arg) - 1] = '\0';
	p = strchr(arg, ':');
	if (p)
		*p++ = '\0';

	part = fb_fetch_part(name);
	if (!part) {
		fb_no_partition(name);
		goto fail;
	}
	size = part->size;

	if (p) {
		off = simple_strtoul(p, &e, 0);
		size = part->size > off ? part->size - off : 0;
		if (*e == ':')
			size = simple_strtoul(e + 1, NULL, 0);
	}

	if (off > part->size || size > part->size - off || !size) {
		fb_fail("0x%lx+0x%lx beyond the end of %s", off, size,
			part->pname);
		goto fail;
	}
	if (size > board_info.max_download_size) {
		fb_fail("0x%lx bytes is over max-fetch-size 0x%lx",
			size, board_info.max_download_size);
		goto fail;
	}

	printf("fetch: %s 0x%lx+0x%lx\n", part->pname, off, size);
	fb_set_busy(1);		/* until fetch_data_over() */
	fastboot->data_buf = (char *)DATA_BUFFER;
#ifdef CONFIG_FASTBOOT_BOOTROM_BASE
	if (part == &fb_bootrom_part) {
		if (off & 3) {
			fb_fail("bootrom offset must be 4-byte aligned");
			goto fail;
		}
		fb_read_bootrom(off, size, (u32 *)fastboot->data_buf);
	} else
#endif
#ifdef CONFIG_RAMLOG
	if (fb_is_log(part)) {
		fb_read_log(part, off, size, fastboot->data_buf);
	} else
#endif
	if (part == &fb_nand_raw_part) {
		fb_nand_read_physical(off, size,
				      (unsigned char *)fastboot->data_buf);
	} else if (part == &fb_nand_part) {
		/* offsets count good blocks over the whole chip */
		if (!off)
			fb_nand_good_size(1);
		fb_nand_chip.offset = 0;
		fb_nand_chip.size = (ulong)nand_info[0].size;
		fb_sfcnand_read(&fb_nand_chip, off, size,
				(unsigned char *)fastboot->data_buf);
	} else if (fb_sfcnand_read(part, off, size, (unsigned char *)fastboot->data_buf)) {
		fb_fail("read error in %s", part->pname);
		goto fail;
	}
	fastboot->data_length = size;

	sprintf(fastboot->ret_buf, "DATA%08lx", size);
	fastboot_cmd_return(fastboot, fetch_send_data, 0, 0);
	return;
fail:
	fb_set_busy(0);
	fb_reply_fail(fastboot);
}
#endif /* CONFIG_FASTBOOT_SFC_NAND */

static int handle_cmd_flash(struct fastboot_dev *fastboot)
{
	int i;
	char *cmd = boot_buf;
	struct disk_storge_msg storge_msg;
	u32 blk,offset,size_blk;

	strsep(&cmd, ":");
	if (!cmd) {
#ifdef CONFIG_FASTBOOT_SFC_NAND
		fb_fail("missing partition name (flash:<name>)");
#else
		printf("FAILmissing partition namei\n");
#endif
		return -1;
	}

#ifdef CONFIG_FASTBOOT_SFC_NAND
	i = fb_find_partition(cmd);
	if (i < 0) {
		fb_no_partition(cmd);
		return -1;
	}
	return fb_sfcnand_flash(&partition_info[i], fastboot);
#endif

	for(i = 0; i < PARTITION_NUM ; i ++){
		if(!strncmp(partition_info[i].pname, cmd, (strlen(partition_info[i].pname)))){
			size_blk = partition_info[i].size / MMC_BYTE_PER_BLOCK;
			offset = partition_info[i].offset;
			blk = partition_info[i].offset / MMC_BYTE_PER_BLOCK;
			break;
		}
	}

	if(i == PARTITION_NUM){
		printf("There is not a partition named : %s\n",boot_buf + 6);
		return -1;
	}

	storge_msg.start = blk;
	storge_msg.size = size_blk;
	storge_msg.blksz = MMC_BYTE_PER_BLOCK;
	storge_msg.name = cmd;

//#ifdef CONFIG_SPL_MMC_SUPPORT
#if defined(CONFIG_SPL_MMC_SUPPORT) || defined(CONFIG_SPL_JZMMC_SUPPORT)
	if(!fb_mmc_flash_write(&storge_msg, fastboot_flash_session_id,
			        (void *)fastboot->data_buf,fastboot->data_length)){
		fastboot_flash_session_id++;
		return 0;
	}
#else
#ifdef CONFIG_JZ_NAND_MGR
	if(!nand_flash(cmd,fastboot))
		return 0;
#endif
#ifdef CONFIG_JZ_SFC
	if(!spi_flash(offset,fastboot))
		return 0;
#endif
#endif
	return -1;
}

static void explain_cmd_flash(struct fastboot_dev *fastboot)
{

#ifdef CONFIG_FASTBOOT_SFC_NAND
	if (handle_cmd_flash(fastboot)) {
		fb_reply_fail(fastboot);
		return;
	}
	strcpy(fastboot->ret_buf, "OKAY");
	return_buf(fastboot, return_complete);
	return;
#endif
	if (handle_cmd_flash(fastboot))
		strcpy(fastboot->ret_buf, "FAILED");
	else
		strcpy(fastboot->ret_buf, "OKAY");

	return_buf(fastboot, return_complete);
}

static int fastboot_mmc_erase(u32 blk,u32 blk_cnt,struct fastboot_dev *fastboot)
{
	int curr_device = 0;

	char commond[128] ;
	memset(commond,0,128);
	sprintf(commond,"mmc erase %x %x",blk,blk_cnt);
	run_command(commond,"0");

	debug("mmc erase ok\n");
	return 0;
}

static int fastboot_nand_erase(unsigned char *pname,struct fastboot_dev *fastboot)
{
	char command[128];
	memset(command,0,128);
	sprintf(command,"nand_zm erase %s",pname);
	printf("command:%s\n",command);
	run_command(command,0);
}

static int fastboot_spi_erase(u32 offset, u32 size, struct fastboot_dev *fastboot)
{
	char command[128];
	memset(command,0,128);
	/* sfcnor erase  [src:nor flash addr] [bytes:0x..] */
	sprintf(command,"sfcnor erase 0x%x 0x%x",offset,size);
	printf("command:%s\n",command);
	run_command(command,0);
}

static int handle_cmd_erase(struct fastboot_dev *fastboot)
{
	debug("please add the erase cmd explain roution\n");
	u32 blk,cnt,offset,size;
	int i;
	unsigned char pname[sizeof(partition_info[0].pname)];
	memset(pname, 0, sizeof(pname));

#ifdef CONFIG_FASTBOOT_SFC_NAND
	i = fb_find_partition((char *)boot_buf + 6);	/* after "erase:" */
	if (i < 0) {
		fb_no_partition((char *)boot_buf + 6);
		return -1;
	}
	/*
	 * The partition at offset 0 holds the SPL the boot ROM loads, and the
	 * ROM has no fallback from SPI NAND: erased, the board cannot start
	 * again. "flash" replaces it safely (it checks the image first).
	 */
	if (partition_info[i].offset == 0) {
		fb_fail("erasing %.15s would leave nothing to boot",
			partition_info[i].pname);
		return -1;
	}
	return fb_sfcnand_erase(&partition_info[i]);
#endif

	for(i = 0; i < PARTITION_NUM ; i ++){
		if(!strncmp(partition_info[i].pname, boot_buf + 6,(strlen(partition_info[i].pname)))){
			strcpy(pname,partition_info[i].pname);
			offset = partition_info[i].offset;
			size = partition_info[i].size;
			blk = partition_info[i].offset / MMC_BYTE_PER_BLOCK;
			cnt = (partition_info[i].size + MMC_BYTE_PER_BLOCK - 1) / MMC_BYTE_PER_BLOCK;
			goto do_erase;
			break;
		}
	}
	if(i == PARTITION_NUM){
		printf("There is not a partition named : %s\n",boot_buf + 6);
		return -1;
	}

do_erase:
//#ifdef CONFIG_SPL_MMC_SUPPORT
#if defined(CONFIG_SPL_MMC_SUPPORT) || defined(CONFIG_SPL_JZMMC_SUPPORT)
	if(!fastboot_mmc_erase(blk , cnt ,fastboot))
		return 0;
#else
#ifdef CONFIG_JZ_NAND_MGR
	if(!fastboot_nand_erase(pname,fastboot))
		return 0;
#endif
#ifdef CONFIG_JZ_SFC
	if(!fastboot_spi_erase(offset,size,fastboot))
		return 0;
#endif
#endif

	return -1;
}

static void explain_cmd_erase(struct fastboot_dev *fastboot)
{
#ifdef CONFIG_FASTBOOT_SFC_NAND
	if (handle_cmd_erase(fastboot)) {
		fb_reply_fail(fastboot);
		return;
	}
	strcpy(fastboot->ret_buf, "OKAY");
	return_buf(fastboot, return_complete);
	return;
#endif
	if (handle_cmd_erase(fastboot))
		strcpy(fastboot->ret_buf, "FAILED");
	else
		strcpy(fastboot->ret_buf, "OKAY");

	return_buf(fastboot, return_complete);
}

static int handle_cmd_reboot_bootloader(struct fastboot_dev *fastboot)
{
	cpm_set_scrpad(FASTBOOT_SIGNATURE);
	return 0;
}

static void explain_cmd_reboot_bootloader(struct fastboot_dev *fastboot)
{
	if (handle_cmd_reboot_bootloader(fastboot))
		strcpy(fastboot->ret_buf, "FAILED");
	else {
		strcpy(fastboot->ret_buf, "OKAY");
		need_reboot_bootloader = 1;
	}

	return_buf(fastboot, return_complete);
}

/*
 * "reboot": answer OKAY first and reset from the main loop once the reply
 * has gone out (need_reboot). Resetting here, before the reply, left the
 * host with "status read failed" and a device that vanished mid-transfer.
 */
static int need_reboot;

static void explain_cmd_reboot(struct fastboot_dev *fastboot)
{
	strcpy(fastboot->ret_buf, "OKAY");
	need_reboot = 1;
	return_buf(fastboot, return_complete);
}

#ifdef CONFIG_FASTBOOT_REBOOT_RECOVERY
/*
 * "reboot-recovery" (host: fastboot reboot recovery): leave
 * RECOVERY_SIGNATURE in the low half of the CPM scratchpad, which survives
 * the reset; the SPL clears it and boots the recovery kernel once.
 */
static void explain_cmd_reboot_recovery(struct fastboot_dev *fastboot)
{
	cpm_set_scrpad((cpm_get_scrpad() & ~0xffff) | RECOVERY_SIGNATURE);
	strcpy(fastboot->ret_buf, "OKAY");
	need_reboot = 1;
	return_buf(fastboot, return_complete);
}
#endif

/* Drop off the bus cleanly, so the host sees a disconnect, then reset. */
static void fb_reset_after_reply(void)
{
	usb_gadget_disconnect(fastboot_common->gadget);
	mdelay(100);
	run_command("reset", 0);
}

#ifndef CONFIG_FASTBOOT_SFC_NAND
/*
 * "continue" and "boot" are the SDK's; not built for SFC NAND boards
 * (the HiBy R1): "continue" only ran fastboot again, "boot" a bootm that
 * is not set up there. SFC NAND boards have their own "boot" below;
 * "continue" gets the "unknown command" reply.
 */
static void return_continue_complete(struct usb_ep *ep,
		struct usb_request *req)
{
	struct fastboot_dev *fastboot = req->context;
	fastboot->leave = 1;
	run_command(CONFIG_BOOTCOMMAND, 0);
}

static int handle_cmd_continue(struct fastboot_dev *fastboot)
{

#if 0
#ifdef CONFIG_SPL_MMC_SUPPORT
	run_command("boota mmc 0 0x80f00000 6144", "0");
#else
#ifdef CONFIG_JZ_NAND_MGR
	run_command("boota nand 0x80f00000 6144", "0");
#endif
#endif
#endif
	return 0;
}

static void explain_cmd_continue(struct fastboot_dev *fastboot)
{
	if (handle_cmd_continue(fastboot))
		strcpy(fastboot->ret_buf, "FAILED");
	else
		strcpy(fastboot->ret_buf, "OKAY");

	return_buf(fastboot, return_continue_complete);
}

static int handle_cmd_boot(struct fastboot_dev *fastboot)
{
	printf("please add the boot cmd explain roution\n");
	memset(boot_kernel_cmd,0,CMD_BUFF_SIZE);
#ifdef CONFIG_BOOT_ANDROID
	memcpy((char *)(BOOT_START_ADDRESS),fastboot->data_buf,fastboot->data_length);
	sprintf(boot_kernel_cmd,"boota mem %x",BOOT_START_ADDRESS);
#else
	memcpy((char *)(BOOT_START_ADDRESS),fastboot->data_buf + FASTBOOT_PAGE,fastboot->data_length - FASTBOOT_PAGE);
	sprintf(boot_kernel_cmd,"bootm 0x%x",BOOT_START_ADDRESS);
#endif
	printf("boot_kernel_cmd:%s\n",boot_kernel_cmd);

	return 0;
}

static void explain_cmd_boot(struct fastboot_dev *fastboot)
{
	if (handle_cmd_boot(fastboot))
		strcpy(fastboot->ret_buf, "FAILED");
	else {
		strcpy(fastboot->ret_buf, "OKAY");
		need_boot_kernel = 1;
	}

	return_buf(fastboot, return_complete);
}
#endif /* !CONFIG_FASTBOOT_SFC_NAND */

#if defined(CONFIG_FASTBOOT_SFC_NAND) && defined(CONFIG_SPL_OS_BOOT)
/*
 * "boot" (host: fastboot boot <kernel>): start the downloaded kernel from
 * RAM exactly as the SPL starts one from flash, without writing anything:
 * the uImage, header included, goes to its load address - 64, and the
 * entry point gets argc 2, argv { 0, cmdline } at CONFIG_PARAM_BASE (see
 * arch/mips/lib/spl.c). The command line is the SPL's for the normal
 * system (rootfs), with "mem=" filled in from the RAM size as
 * common/spl/spl_auto_args_mem.c does it.
 *
 * Host fastboot wraps a kernel that is not an Android boot image into one
 * (header v0, the kernel at page_size); both that and a bare uImage are
 * taken. The boot image's command line ("fastboot --cmdline ...") is
 * appended, so it can add arguments or override earlier ones (the last
 * root= wins). A ramdisk in it is ignored: the SPL path has none.
 */
#define FB_BOOT_MAGIC		"ANDROID!"
#define FB_BOOT_HDR_MIN		1580		/* v3/v4 header size */
#define FB_BOOT_HDR_V0		1632		/* v0-v2: the fields used */
#define FB_BOOT_ARGS_ADDR	(CONFIG_PARAM_BASE + 0x100)
#define FB_BOOT_ARGS_MAX	0x700		/* up to 0x80001000 */

extern void flush_cache_all(void);	/* arch/mips/cpu/xburst/cpu.c */

static const image_header_t *fb_boot_kernel;
static char fb_boot_args[FB_BOOT_ARGS_MAX];
static int need_fb_boot;

/* append at most @max bytes of a NUL-padded header field to @dst */
static void fb_boot_add_field(char *dst, const unsigned char *f, int max)
{
	int n = 0;

	while (n < max && f[n])
		n++;
	strncat(dst, (const char *)f, n);
}

/* SPL command line for the normal system + @extra, into fb_boot_args */
static int fb_boot_cmdline(const char *extra)
{
	const char *base = CONFIG_SYS_SPL_ARGS_ADDR;
	const char *ms = strstr(base, "[mem-start");
	const char *me = ms ? strstr(ms, "mem-end]") : NULL;
	char mem[128];
	char *p = mem;
	ulong n;

	mem[0] = '\0';
	if (ms && me) {
		unsigned int real = gd->ram_size >> 20;
		unsigned int size = real > 256 ? 256 : real;

		size -= CONFIG_NMEM_MB + CONFIG_RMEM_MB + CONFIG_RTOS_SIZE_MB;
		p += sprintf(p, "mem=%uM@0x0", size);
		if (CONFIG_RMEM_MB)
			p += sprintf(p, " rmem=%uM@0x%x", CONFIG_RMEM_MB,
				     size << 20);
		if (CONFIG_NMEM_MB)
			p += sprintf(p, " nmem=%uM@0x%x", CONFIG_NMEM_MB,
				     (size + CONFIG_RMEM_MB) << 20);
		if (CONFIG_RTOS_SIZE_MB)
			p += sprintf(p, " rtos_size=%uM@0x%x",
				     CONFIG_RTOS_SIZE_MB, (size + CONFIG_RMEM_MB +
				     CONFIG_NMEM_MB) << 20);
		if (real > 256)
			p += sprintf(p, " mem=%uM@0x%x", real - 256, 768 << 20);
		me += strlen("mem-end]");
	} else {
		ms = base + strlen(base);
		me = ms;
	}

	n = (ms - base) + strlen(mem) + strlen(me) +
	    (*extra ? 1 + strlen(extra) : 0) + 1;
	if (n > sizeof(fb_boot_args)) {
		fb_fail("command line too long (%lu bytes)", n);
		return -1;
	}
	memcpy(fb_boot_args, base, ms - base);
	fb_boot_args[ms - base] = '\0';
	strcat(fb_boot_args, mem);
	strcat(fb_boot_args, me);
	if (*extra) {
		strcat(fb_boot_args, " ");
		strcat(fb_boot_args, extra);
	}
	return 0;
}

static int fb_boot_prepare(const unsigned char *buf, u32 len)
{
	static char extra[1536 + 1];
	const unsigned char *k = buf;
	const image_header_t *hdr;
	u32 klen = len;
	ulong end, top;

	extra[0] = '\0';
	if (len >= 8 && !memcmp(buf, FB_BOOT_MAGIC, 8)) {
		u32 ver, page, ramdisk;

		if (len < FB_BOOT_HDR_MIN) {
			fb_fail("boot image header truncated");
			return -1;
		}
		klen = fb_le32(buf + 8);
		ver = fb_le32(buf + 40);
		if (ver >= 3) {
			page = 4096;
			ramdisk = fb_le32(buf + 12);
			fb_boot_add_field(extra, buf + 44, 1536);
		} else {
			if (len < FB_BOOT_HDR_V0) {
				fb_fail("boot image header truncated");
				return -1;
			}
			page = fb_le32(buf + 36);
			ramdisk = fb_le32(buf + 16);
			fb_boot_add_field(extra, buf + 64, 512);
			fb_boot_add_field(extra, buf + 608, 1024);
		}
		if (page < 2048 || page > 65536 || (page & (page - 1))) {
			fb_fail("bad boot image page size %u", page);
			return -1;
		}
		if (page > len || klen > len - page) {
			fb_fail("boot image truncated");
			return -1;
		}
		if (ramdisk)
			printf("fastboot: boot: ramdisk ignored\n");
		k = buf + page;
	}

	if (fb_check_kernel_image(k, klen))
		return -1;

	/* the copy must not reach U-Boot: stay below the download area top */
	hdr = (const image_header_t *)k;
	end = image_get_load(hdr) + image_get_data_size(hdr);
	top = CONFIG_FASTBOOT_BUF_ADDR + board_info.max_download_size;
	if (end > top) {
		fb_fail("kernel ends at 0x%08lx, above 0x%08lx", end, top);
		return -1;
	}

	if (fb_boot_cmdline(extra))
		return -1;
	fb_boot_kernel = hdr;
	return 0;
}

static void explain_cmd_boot_ram(struct fastboot_dev *fastboot)
{
	if (!fastboot->data_length) {
		fb_fail("nothing downloaded: run download first");
		fb_reply_fail(fastboot);
		return;
	}
	if (fb_boot_prepare((const unsigned char *)fastboot->data_buf,
			    fastboot->data_length)) {
		fb_reply_fail(fastboot);
		return;
	}
	/* reply first; the main loop starts the kernel once it has gone out */
	strcpy(fastboot->ret_buf, "OKAY");
	need_fb_boot = 1;
	return_buf(fastboot, return_complete);
}

static void fb_boot_go(void)
{
	const image_header_t *hdr = fb_boot_kernel;
	ulong hsize = image_get_header_size();
	void *dst = (void *)(image_get_load(hdr) - hsize);
	u32 *params = (u32 *)CONFIG_PARAM_BASE;
	char *args = (char *)FB_BOOT_ARGS_ADDR;
	void (*entry)(int, char **, void *) =
		(void (*)(int, char **, void *))image_get_ep(hdr);

	printf("fastboot: booting %.*s from RAM\n", IH_NMLEN,
	       image_get_name(hdr));
	printf("fastboot: cmdline: %s\n", fb_boot_args);

	/* leave the bus, as for a reboot: Linux brings up its own gadget */
	usb_gadget_disconnect(fastboot_common->gadget);
	mdelay(100);
	/* fastboot's colour off: the kernel starts with the LEDs dark */
	fastboot_led_off();

	strcpy(args, fb_boot_args);
	memmove(dst, hdr, hsize + image_get_data_size(hdr));
	params[0] = 0;
	params[1] = (u32)args;
	flush_cache_all();
	entry(2, (char **)params, NULL);
}
#endif /* CONFIG_FASTBOOT_SFC_NAND && CONFIG_SPL_OS_BOOT */

static int handle_cmd_oem(struct fastboot_dev *fastboot)
{
	printf("please add the oem cmd explain roution\n");
	return -1;
}

static void explain_cmd_oem(struct fastboot_dev *fastboot)
{
#ifdef CONFIG_FASTBOOT_SFC_NAND
	if (!strcmp((char *)fastboot->cmd_req->buf, "oem partitions")) {
		fb_oem_partitions(fastboot);
		return;
	}
	if (!strcmp((char *)fastboot->cmd_req->buf, "oem bootrom")) {
		/* reply first, reset from the main loop once it has gone out */
		strcpy(fastboot->ret_buf, "OKAY");
		need_reboot_bootrom = 1;
		return_buf(fastboot, return_complete);
		return;
	}
#ifdef CONFIG_RAMLOG
	if (!strcmp((char *)fastboot->cmd_req->buf, "oem log")) {
		fb_oem_log(fastboot, 0);
		return;
	}
	if (!strcmp((char *)fastboot->cmd_req->buf, "oem lastlog")) {
		fb_oem_log(fastboot, 1);
		return;
	}
	fb_fail("unknown oem command (partitions bootrom log lastlog)");
#else
	fb_fail("unknown oem command (have: partitions, bootrom)");
#endif
	fb_reply_fail(fastboot);
	return;
#endif
	if (handle_cmd_oem(fastboot))
		strcpy(fastboot->ret_buf, "FAILED");
	else
		strcpy(fastboot->ret_buf, "OKAY");

	return_buf(fastboot, return_complete);
}

static void explain_cmd_else(struct fastboot_dev *fastboot)
{
	strcpy(fastboot->ret_buf, "FAILED: The command is not recongized\n");
	return_buf(fastboot, return_complete);
}

static void handle_fastboot_cmd_complete(struct usb_ep *ep,
		struct usb_request *req)
{
	struct fastboot_dev *fastboot = req->context;

	debug("The received command is:<%s>\n",(char *)req->buf);

#ifdef CONFIG_FASTBOOT_SFC_NAND
	/*
	 * Not a command: the request was cancelled (set_alt disables the
	 * endpoints when the host re-enumerates, giving it back with
	 * -ESHUTDOWN) or carried nothing. Answering it queued a stray
	 * "FAILunknown command" that the host then took as the reply to
	 * its next command. Just read the next command.
	 */
	if (req->status || !req->actual) {
		fastboot->explain_cmd_status = 1;
		goto cmd_finish;
	}

	/* steady LED unless the command below starts a transfer */
	fb_set_busy(0);
	fb_clear_reason();
#endif

	if (!strncmp(req->buf, "getvar:all", 10)) {
#ifdef CONFIG_FASTBOOT_SFC_NAND
		fb_getvar_all(fastboot);
#else
		explain_cmd_getvar_all(fastboot);
#endif
		goto cmd_finish_2;
	}

	if (!strncmp(req->buf, "getvar:", 7)) {
		explain_cmd_getvar(fastboot);
		goto cmd_finish;
	}

	if (!strncmp(req->buf, "download:", 9)) {
		if (explain_cmd_download(fastboot) != 0) {
			goto cmd_finish;
		}
		goto cmd_finish_2;
	}

#ifdef CONFIG_FASTBOOT_SFC_NAND
	if (!strncmp(req->buf, "fetch:", 6)) {
		explain_cmd_fetch(fastboot);
		goto cmd_finish_2;
	}
#endif

	if (!strncmp(req->buf, "flash:", 6)) {
		boot_buf = req->buf;
		explain_cmd_flash(fastboot);
#ifdef CONFIG_FASTBOOT_SFC_NAND
		fb_set_busy(0);		/* blinked per NAND block */
#endif
		goto cmd_finish;
	}

	if (!strncmp(req->buf, "erase:", 6)) {
		boot_buf = req->buf;
		explain_cmd_erase(fastboot);
#ifdef CONFIG_FASTBOOT_SFC_NAND
		fb_set_busy(0);
#endif
		goto cmd_finish;
	}

	if (!strncmp(req->buf, "reboot-bootloader", 17)) {
		explain_cmd_reboot_bootloader(fastboot);
		goto cmd_finish;
	}

#ifdef CONFIG_FASTBOOT_REBOOT_RECOVERY
	/* before the "reboot" prefix match below, which would take them */
	if (!strcmp(req->buf, "reboot-recovery")) {
		explain_cmd_reboot_recovery(fastboot);
		goto cmd_finish;
	}

	/* fastbootd's "reboot fastboot": this is the only fastboot there is */
	if (!strcmp(req->buf, "reboot-fastboot")) {
		explain_cmd_reboot_bootloader(fastboot);
		goto cmd_finish;
	}
#endif

	if ((strcmp(req->buf, "reboot-bootloader")) && !strncmp(req->buf, "reboot",6)) {
		explain_cmd_reboot(fastboot);
		goto cmd_finish;
	}

#ifndef CONFIG_FASTBOOT_SFC_NAND
	if (!strcmp(req->buf, "continue")) {
		explain_cmd_continue(fastboot);
		goto cmd_finish_2;
	}

	if (!strcmp(req->buf, "boot")) {
		explain_cmd_boot(fastboot);
		goto cmd_finish;
	}
#elif defined(CONFIG_SPL_OS_BOOT)
	if (!strcmp(req->buf, "boot")) {
		explain_cmd_boot_ram(fastboot);
		goto cmd_finish;
	}
#endif

	if (!strncmp(req->buf, "oem:", 3)) {
		explain_cmd_oem(fastboot);
		goto cmd_finish;
	}

#ifdef CONFIG_FASTBOOT_SFC_NAND
	/* answer it: no reply left the host waiting until it timed out */
	fb_fail("unknown command '%.40s'", (char *)req->buf);
	fb_reply_fail(fastboot);
	goto cmd_finish;
#endif
//	explain_cmd_else(fastboot);
	fastboot->explain_cmd_status = 1;
	goto cmd_finish;
cmd_finish:
cmd_finish_2:
	memset(fastboot->cmd_req->buf, 0, fastboot->cmd_req->length);
	free(fastboot->cmd_req->buf);
	fastboot->cmd_req->buf = NULL;
	usb_ep_free_request(fastboot_common->bulk_out, fastboot->cmd_req);
	fastboot->cmd_req = NULL;
	return;
}

static int handle_fastboot_init(struct fastboot_dev *fastboot)
{
	fastboot->ret_req = usb_ep_alloc_request(fastboot_common->bulk_in, 0);
	if (!fastboot->ret_req) {
		debug("%s: Error, usb alloc request for return\n", __func__);
		return -ENOMEM;
	}

	fastboot->ret_buf = malloc(RET_LENGTH * sizeof(char));
	if (!fastboot->ret_buf) {
		printf("%s: Error in malloc RAM for ret_buf\n", __func__);
		return -ENOMEM;
	}

	fastboot_common->enum_done = 0;
	fastboot->explain_cmd_status = 0;
	return 0;
}

static void handle_fastboot_complete(struct fastboot_dev *fastboot)
{
	free(fastboot->ret_buf);
	fastboot->ret_buf = NULL;
	usb_ep_free_request(fastboot_common->bulk_in, fastboot->ret_req);
}

/* add somes functions(Start, Power off, Restart bootloader,
 * Recovery mode, they are selected by power key and changed by
 * volume key, and display some images */
static void function_add(struct fastboot_dev *fastboot)
{
#ifdef CONFIG_FASTBOOT_SFC_NAND
	if (fb_busy)
		fastboot_led_busy();	/* USB data transfer in progress */
#endif
	return;
}

void handle_fastboot_cmd(void)
{
	struct fastboot_dev *fastboot;
	int	ret;
	fastboot = malloc(sizeof(struct fastboot_dev));
	if (!fastboot) {
		printf("%s:Error malloc for fastboot_dev\n", __func__);
		return;
	}
	memset(fastboot, 0, sizeof(struct fastboot_dev));

	ret = handle_fastboot_init(fastboot);
	if (ret != 0){
		printf("%s:Error init,%d\n", __func__, ret);
		goto over1;
	};

	while (1) {
		function_add(fastboot);

		if (fastboot->leave) {
			break;
		}

		if (fastboot_common->enum_done || fastboot->explain_cmd_status) {
			int ret_value;
			fastboot_common->enum_done = 0;
			fastboot->explain_cmd_status = 0;

#ifdef CONFIG_FASTBOOT_SFC_NAND
			if (need_reboot_bootrom) {
				need_reboot_bootrom = 0;
				printf("fastboot: rebooting into the boot ROM's USB mode\n");
				cpm_outl(FB_BOOTROM_USB_FLAG, CPM_SLPC);
				need_reboot = 1;
			}
#endif
#if defined(CONFIG_FASTBOOT_SFC_NAND) && defined(CONFIG_SPL_OS_BOOT)
			/* the OKAY reply to "boot" has been sent: start it */
			if (need_fb_boot) {
				need_fb_boot = 0;
				fb_boot_go();
			}
#endif
			/* the OKAY reply has been sent: now it is safe to reset */
			if (need_reboot || need_reboot_bootloader) {
				need_reboot = 0;
				need_reboot_bootloader = 0;
				fb_reset_after_reply();
				printf("reboot failed\n");
			}

			if(need_boot_kernel) {
				need_boot_kernel = 0;
				if(run_command(boot_kernel_cmd, 0))
					printf("boot kernel failed\n");
			}

			fastboot->cmd_req = usb_ep_alloc_request(fastboot_common->bulk_out, 0);
			if (!fastboot->cmd_req) {
				printf("%s: Error, usb alloc request for command\n", __func__);
				goto over1;
			}

			fastboot->cmd_req->length = 64;
			fastboot->cmd_req->buf = malloc(fastboot->cmd_req->length);
			if (!fastboot->cmd_req->buf) {
				printf("%s:Error malloc for command buffer\n", __func__);
				goto over2;
			}

			fastboot->cmd_req->complete = handle_fastboot_cmd_complete;
			memset(fastboot->cmd_req->buf, 0, 64);

			fastboot->cmd_req->status = 0;
			fastboot->cmd_req->context = fastboot;

			ret_value = usb_ep_queue(fastboot_common->bulk_out,
					fastboot->cmd_req, 0);
			if (ret_value != 0) {
				printf("%s: Warning, usb_ep_queue return %d\n",
						__func__, ret_value);
				goto over2;
			}
		}

		usb_gadget_handle_interrupts();
	}

over2:
	handle_fastboot_complete(fastboot);
over1:
	free(fastboot);
	fastboot = NULL;
}

