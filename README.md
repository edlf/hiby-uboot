# HiBy R1 / R3 Pro II U-Boot

U-Boot for the HiBy R1 and R3 Pro II (Ingenic X1600, SPI NAND), built
from Ingenic's X1600 SDK (U-Boot 2013.07). It replaces the stock boot loader in the
`uboot` partition and boots the stock firmware exactly as before, and adds
a USB fastboot mode for flashing, dumping and recovering the device.

The stock loader is an SPL only: the boot ROM loads it, it sets up DDR,
and loads Linux straight from NAND (falcon mode). This build keeps that
path for normal boots, with the stock DDR tuning, NAND chip support and
OTA/recovery handling matched to the stock SPL, and stores a full U-Boot
right after the SPL that runs only when fastboot is asked for.

## Boards

| | R1 | R3 Pro II |
| --- | --- | --- |
| Build target | `hiby_r1_xImage_sfc_nand` | `hiby_r3proii_xImage_sfc_nand` |
| Config | `include/configs/hiby_r1.h` | `include/configs/hiby_r3proii.h` |
| Play/Pause (fastboot) | 0 mV | 0 mV |
| Volume down (normal, forced) | 1000 mV | 1115 mV |
| Volume up (recovery) | 1650 mV | 1700 mV |
| Fastboot LED | purple (red + blue GPIOs) | none (SGM31324 LED driver on I2C) |
| USB | `32bb:0101`, "HiBy R1" | `32bb:0101`, "HiBy R3 Pro II" |
| Status | in use | **built, never run; do not install yet** |

Both share everything else: the board code (`board/ingenic/hiby_x1600`), the
settings in `include/configs/hiby_x1600_common.h`, the OTA and recovery
scheme, the console (UART2, 3 Mbaud) and HiBy's USB IDs. The R3 Pro II
values come from its stock firmware 1.4 (`r3proii(v1.4).upt`: key
ladder, LED and USB scripts, device tree); their SPLs differ in the
Volume down window and the DDR table (below).

The R3 Pro II board has been compared with the R3 Pro II's own stock
loader, from a dump of its `uboot` partition (the update file does not
contain it; U-Boot SPL 2013.07-g2a44fbe of 30 Aug 2025):

- DDR: the stock SPL knows one chip, LPDDR2 `M54D5121632A` (64 MiB, the
  X1600E's own), and no SoC ID table. The R3 Pro II board is built the
  same way (the SDK's `CONFIG_X1600E_DDR`), and its DDR register values
  match the stock SPL's field for field.
- SPI NAND: all 50 chips the stock SPL knows are in the shared table.
- Kernel command lines and the OTA record are the R1's.
- Partition table: the same as the R1's (`partitions/r3proii.txt`,
  `partitions/r1.txt`), recovery root fs `rootfs2`.

> [!CAUTION]
> It has not run on an R3 Pro II yet. For the first install, have a way
> back ready (Ingenic's USB boot mode, [7.](#7-recovery-the-boot-roms-usb-mode)).

## Contents

1. [What it does at power-on](#1-what-it-does-at-power-on)
2. [Images](#2-images)
3. [Installing](#3-installing)
4. [Fastboot mode](#4-fastboot-mode)
5. [Safety checks when flashing](#5-safety-checks-when-flashing)
6. [Changes compared to the stock loader and the SDK](#6-changes-compared-to-the-stock-loader-and-the-sdk)
7. [Recovery: the boot ROM's USB mode](#7-recovery-the-boot-roms-usb-mode)
8. [Building](#8-building)

## 1. What it does at power-on

The SPL reads the keys once (the ADC key ladder, as the stock SPL does;
the voltages are per board, see [Boards](#boards)) and the `ota`
partition, then picks one of three paths:

| Key held at power-on | What boots |
| --- | --- |
| (none) | Normal system: kernel + rootfs, or kernel2 + rootfs2 when HiBy's OTA record says so |
| Volume down | Normal system, forced: overrides the OTA record, a fastboot request and a reboot target |
| Volume up | Recovery: kernel2 + rootfs2 (as stock) |
| Play/Pause | Fastboot (LED purple on the R1) |

HiBy's scheme is normal + recovery, not A/B: kernel/rootfs is the system,
kernel2/rootfs2 the recovery that installs updates. Page 0 of the `ota`
partition holds HiBy's record (`ota:kernel` or `ota:kernel2`, padded to
256 bytes); the SPL boots kernel2 when it says `ota:kernel2`.

Order of precedence:

1. Volume down
2. Fastboot: Play/Pause, the fastboot request in `ota` page 1,
   `fastboot reboot bootloader`
3. Recovery: `fastboot reboot recovery`, Volume up, the `ota:kernel2`
   record
4. The normal system

An `ota` block the SPL cannot make sense of (garbage, an unknown string,
an uncorrectable ECC error) is erased and the device boots normally, so a
corrupt record cannot keep it from booting. A block marked bad is left
alone.

### Partitions

From the burner partition table at 0x5800 in `uboot`
(`partitions/<model>.txt`, written into the `-withparts` images). The
recovery root fs is `rootfs2` on both players. A table dumped from an R1
may still name it a second `rootfs`; fastboot shows that one as `rootfs2`
too, and the kernel and the recovery treat the two names alike.

| Name | Offset | Size | Contents |
| --- | --- | --- | --- |
| `uboot` | `0x00000000` | `0x00080000` | SPL + U-Boot + partition table |
| `kernel` | `0x00080000` | `0x00500000` | Normal kernel (uImage) |
| `rootfs` | `0x00580000` | `0x02d00000` | Normal root fs (squashfs) |
| `kernel2` | `0x03280000` | `0x00500000` | Recovery kernel (uImage) |
| `rootfs2` | `0x03780000` | `0x01800000` | Recovery root fs (squashfs) |
| `ota` | `0x04f80000` | `0x00080000` | OTA record (page 0), fastboot request (page 1) |
| `userdata` | `0x05000000` | `0x03000000` | UBI (Linux uses 45 MiB of it) |

## 2. Images

Built into `out/` in the SDK top folder (see [8. Building](#8-building)).
The commands in this README are run from that folder.

| File | Use |
| --- | --- |
| `u-boot-hiby_r1.bin` | SPL + U-Boot, partition table area left blank. For `fastboot flash uboot`: U-Boot then keeps the table already in flash. **The image for updates.** |
| `u-boot-hiby_r1-withparts.bin` | The same with the stock partition table at 0x5800 (copied from the stock image). For writing the partition directly: the first install from Linux, a flash programmer, Ingenic's USB cloner. |
| `u-boot-hiby_r1-nouid.bin`, `u-boot-hiby_r1-nouid-withparts.bin` | The same without reading the NAND unique ID for the USB serial number (it is then `0123456789`). A fallback in case the ID read misbehaves on some NAND chip. Built only with `./build_images.sh --nouid`. |
| `u-boot-hiby_r3proii.bin`, `u-boot-hiby_r3proii-withparts.bin` (and the `-nouid` ones with `--nouid`) | The same for the R3 Pro II (see the caution under [Boards](#boards)), the `-withparts` ones with its stock partition table (`partitions/r3proii.txt`). |

All of them are about 410 KiB and fit the 512 KiB `uboot` partition. The
commands below use the R1's file names; on the R3 Pro II, use its own.

## 3. Installing

> [!WARNING]
> The boot ROM loads the SPL from SPI NAND and has no fallback. If the
> `uboot` partition ends up without a working SPL (interrupted write,
> wrong file, power loss), the R1 will not start and can only be
> recovered with Ingenic's USB boot mode. Charge the battery first and
> keep a backup of the stock partition.

### 3.1 First install, from the stock firmware (adb)

Needs a root shell on the R1 (adb). The stock firmware has `flash_erase`
and `nandwrite` (its own OTA scripts use them) and the `uboot` partition
is writable.

Back up the stock partition first (if you have no dump yet):

```sh
adb shell cat /proc/mtd             # mtd0 must be "uboot"
adb shell "cat /dev/mtd0 > /tmp/mtd0-stock.bin"
adb pull /tmp/mtd0-stock.bin
```

Write the new one (the `-withparts` image: it carries the partition
table):

```sh
adb push out/u-boot-hiby_r1-withparts.bin /tmp/u-boot.bin
adb shell flash_erase /dev/mtd0 0 0
adb shell nandwrite -p /dev/mtd0 /tmp/u-boot.bin
```

Check it before rebooting (both sums must match):

```sh
adb shell md5sum /tmp/u-boot.bin
adb shell 'head -c $(wc -c < /tmp/u-boot.bin) /dev/mtd0 | md5sum'
```

If they differ, write it again; do not reboot with a bad copy.

Then reboot. The R1 starts the stock firmware as before.

### 3.2 Updates, from fastboot

```sh
fastboot flash uboot out/u-boot-hiby_r1.bin
fastboot reboot
```

Before writing, U-Boot checks that the image starts with an SPL the boot
ROM accepts (header magic, length, the three CRC7 checksums) and that the
result has a valid partition table (see
[5. Safety checks](#5-safety-checks-when-flashing)). A file that fails is
not written.

### 3.3 Going back to stock

Flash the stock 26 KiB image:

```sh
fastboot flash uboot u-boot.bin
```

or, the same from a dump of the stock partition (it passes the same
checks and carries its own partition table):

```sh
fastboot flash uboot mtd0-stock.bin
```

or write the mtd0 backup from Linux as in
[3.1](#31-first-install-from-the-stock-firmware-adb).

## 4. Fastboot mode

### 4.1 Entering it

- Hold Play/Pause while powering on.
- `fastboot reboot bootloader` from fastboot.
- From Linux: write the request to page 1 of `ota` and reboot:

  ```sh
  printf "%-256s" ota:fastboot | nandwrite -s 2048 -p /dev/mtd5 -
  reboot
  ```

  (`mtd5` is `ota`, check `/proc/mtd`.) U-Boot clears the request when it
  starts, keeping HiBy's record in page 0, so the next boot is normal.

In fastboot the R1's LED is purple; it blinks while flashing, erasing or
fetching. `fastboot reboot` leaves it. The R3 Pro II has no fastboot LED.

The device shows up as USB `32bb:0101` (HiBy's IDs), "HiBy R1" or
"HiBy R3 Pro II", with the NAND unique ID as serial number, so several
players can be told apart with `fastboot -s`. On Linux, fastboot needs access to the device, for
example with a udev rule:

```
SUBSYSTEM=="usb", ATTR{idVendor}=="32bb", ATTR{idProduct}=="0101", TAG+="uaccess"
```

On Windows, bind a WinUSB driver to it (for example with Zadig).

### 4.2 Commands

| Command | What it does |
| --- | --- |
| `fastboot devices` | List connected devices |
| `fastboot getvar <name>` | Read a variable (see [4.3](#43-variables)) |
| `fastboot getvar all` | All variables |
| `fastboot oem partitions` | Partition table, one line each |
| `fastboot flash <part> <file>` | Write a partition (checks in [5.](#5-safety-checks-when-flashing)) |
| `fastboot erase <part>` | Erase a partition (not `uboot`) |
| `fastboot fetch <part> <file>` | Read a partition to the PC (see [4.4](#44-reading-flash-and-rom-back)) |
| `fastboot boot <kernel>` | Start a kernel from RAM without flashing it (see [4.5](#45-booting-a-kernel-from-ram)) |
| `fastboot reboot` | Normal boot |
| `fastboot reboot bootloader` | Back into fastboot |
| `fastboot reboot fastboot` | The same |
| `fastboot reboot recovery` | Boot kernel2 + rootfs2 once |
| `fastboot oem bootrom` | Reset into the SoC's USB boot mode (see [7.](#7-recovery-the-boot-roms-usb-mode)) |

The SDK's `continue` is not built for the R1; like any other unknown
command it gets a `FAIL` reply.

`fastboot erase uboot` is refused ("erasing uboot would leave nothing to
boot"): the boot ROM would find no SPL and the R1 would not start again
(see the warning in [3.](#3-installing)). Replace it with
`fastboot flash uboot` instead, which checks the image first.

Errors come back as `FAIL` with a reason, for example
"uImage data checksum mismatch", or "no partition 'boot'" with the list
of partitions on an `INFO` line before it.

### 4.3 Variables

| Variable | Value |
| --- | --- |
| `version-bootloader` | U-Boot version, board, build date and time |
| `version-hardware` | `R1` / `R3PROII` |
| `product` | `hiby_r1` / `hiby_r3proii` |
| `variant` | `x1600` |
| `serialno` | NAND unique ID, 16 hex digits |
| `max-download-size` | Largest flash image (about 50 MiB) |
| `max-fetch-size` | Largest single fetch read (the same) |
| `partitions` | The partition list |
| `partition-size:<p>` | Size of `<p>` (also for `nand`, `nand_raw`, `bootrom`) |
| `partition-type:<p>` | `raw` |
| `has-slot:<p>` | `no` |
| `is-logical:<p>` | `no` |

### 4.4 Reading flash and ROM back

`fetch` needs a fastboot new enough to have it (recent Android
platform-tools). It splits large reads into `max-fetch-size` chunks by
itself.

| Command | Reads |
| --- | --- |
| `fastboot fetch kernel kernel.bin` | Any partition |
| `fastboot fetch nand flash.bin` | The whole chip, bad blocks skipped (offsets as Linux `nandwrite` lays data out) |
| `fastboot fetch nand_raw flash-raw.bin` | The whole chip at physical offsets, bad blocks included |
| `fastboot fetch bootrom rom.bin` | The X1600 mask ROM, 32 KiB |

`nand`, `nand_raw` and `bootrom` can only be read, not flashed or erased.

### 4.5 Booting a kernel from RAM

`fastboot boot` starts a kernel without writing it to flash, for trying a
new kernel: the next reboot starts whatever is in flash again.

```sh
fastboot boot uImage                    # with the normal root fs
fastboot --cmdline "init=/bin/sh" boot uImage
fastboot --cmdline "root=/dev/mtdblock_bbt_ro4" boot uImage   # with rootfs2

# the recovery system, kernel2 + rootfs2, straight from its partition
fastboot fetch kernel2 kernel2.bin
fastboot --cmdline "root=/dev/mtdblock_bbt_ro4" boot kernel2.bin
```

The kernel is a uImage, as in the `kernel` partitions; it passes the same
checks as `fastboot flash kernel` (see
[5.](#5-safety-checks-when-flashing)), and it must end below the top of
the download area. U-Boot starts it exactly as the SPL does: the uImage
goes to its load address, and the kernel gets the SPL's command line for
the normal system (`root=/dev/mtdblock_bbt_ro2`, with `mem=` worked out
from the RAM size). Text given with `--cmdline` is appended, so it can
add arguments or replace earlier ones (the last `root=` wins). Host
fastboot wraps the file into an Android boot image before sending it;
U-Boot takes the kernel out of it (header versions 0 to 4) and ignores a
ramdisk. A bare uImage is taken as well.

Before the kernel starts, the device leaves the USB bus, as on a reboot,
and turns the LED off (R1).

## 5. Safety checks when flashing

| Partition | Check |
| --- | --- |
| `uboot` | The image must be an SPL the boot ROM will load: magic `06 05 04 03 02 55 aa 55`, a sane SPL length, and matching header, parameter and SPL CRC7 checksums. Then the partition table at 0x5800 (below). |
| `kernel`, `kernel2` | The image must be a uImage the SPL can boot: header and data CRC32 correct, Linux/MIPS/kernel/uncompressed, load range inside RAM above `0x80008000`, entry point inside the image. |
| All | The image must fit the partition. Erase and write go block by block and skip bad blocks, as Linux `nandwrite` does. |

The partition table of a `uboot` image:

- an image too short to reach it is padded with `0xff`;
- a blank table is replaced by the one in flash (if that one is valid);
- a table in the image must be valid itself (magic `nand`, named
  entries, one at offset 0 big enough for the image, a `kernel` entry).

`fastboot erase` refuses the partition at offset 0 (`uboot`) outright.

## 6. Changes compared to the stock loader and the SDK

### Board and boot

`board/ingenic/hiby_x1600`, `common/spl/spl_sfc_nand_v2.c`

- New boards `hiby_r1` and `hiby_r3proii` (`boards.cfg` targets
  `hiby_r1_xImage_sfc_nand`, `hiby_r3proii_xImage_sfc_nand`), sharing the
  board code in `board/ingenic/hiby_x1600` and the settings in
  `include/configs/hiby_x1600_common.h`; UART2 at 3 Mbaud as stock.
- ADC key driver (`adc_key.c`): SADC channel 0, Volume down / Volume up /
  Play/Pause windows per board (see [Boards](#boards)).
- SPL boot selection: OTA record, fastboot request, CPM scratchpad
  signatures from "reboot bootloader" (`0x0666`) and "reboot recovery"
  (`0x1a1a`), and the keys, in the order given in
  [1.](#1-what-it-does-at-power-on)
- Invalid `ota` blocks are erased (raw page reads; bad blocks left
  alone).
- The SPL loads the full U-Boot from the `uboot` partition when fastboot
  is asked for.

### DDR

`arch/mips/cpu/xburst`, `tools/ingenic-tools/ddr_creator_x1600`

- DDR parameters chosen by the eFuse SoC ID for the known X1600 parts
  (8 IDs), otherwise by the burned DDR ID, as stock.
- X1600HN DDR2 tuning matched byte for byte to the stock SPL (CL7,
  tWTR 10 ns, tMRD 4 tCK, DLL off, ODT and drive strength settings),
  DLL reset off.

### SPI NAND

`tools/ingenic-tools/nand_device`, `drivers/mtd/devices/jz_sfc_v2`

- Added the chips the stock loader knows: Winbond W25N04KV (ID `AA23`)
  and Zbit ZB35Q01CYIG (ID `C1`), in the SPL table and the U-Boot driver,
  with their ECC status handling.
- Unique ID read through the standard OTP area (any chip that has one),
  used for the USB serial number.

### Fastboot

`drivers/usb/gadget/f_fastboot.c`, `g_fastboot.c`

- SPI NAND backend for flash and erase, with the checks in
  [5.](#5-safety-checks-when-flashing); erase refuses the boot partition.
- `fetch`, with the `nand`, `nand_raw` and `bootrom` read-only targets.
- `getvar` partitions / partition-size / partition-type / max-fetch-size
  / has-slot / is-logical / serialno / all; `oem partitions`;
  `oem bootrom`.
- `reboot`, `reboot bootloader` / `fastboot` / `recovery`: the reply goes
  out first, then the device disconnects and resets (no more "status read
  failed").
- `boot`: starts a downloaded kernel from RAM the way the SPL starts one
  from flash (see [4.5](#45-booting-a-kernel-from-ram)); replaces the
  SDK's, which ran a `bootm` not set up for the R1.
- The SDK's `continue` removed (it only ran fastboot again).
- Clear `FAIL` reasons; several SDK bugs fixed (an unanswered unknown
  command, a hang after a failed download, an uninitialised name in
  erase, a missing final `OKAY` in `getvar all`).
- LED: purple in fastboot, blinking during long operations (R1).
- USB IDs `32bb:0101`; names "HiBy" / "R1" (or "R3 Pro II") for the
  device and "HiBy R1" (or "HiBy R3 Pro II") for the fastboot interface
  (the SDK said "Google" in both places).

### USB device controller

`drivers/usb/gadget/jz47xx_dwc2_udc.c`, `arch/mips/cpu/xburst/x1600/clk.c`,
`composite.c`

- Clean start: the device drops off the bus at U-Boot start and connects
  once fastboot is up, so a reboot from Linux re-enumerates cleanly.
- PHY set up as Linux does it (VBUS and B-session always valid, the same
  tuning values).
- Bus reset after enumeration handled (requests given back, endpoints and
  FIFOs cleaned up).
- Answers the standard `GET_STATUS` and feature requests. The host sends
  `GET_STATUS` after every resume from autosuspend; unanswered, it waited
  5 s and reset the device. That was why the first command after a pause
  was slow and the device re-enumerated.
- Request queueing fixed (requests were dropped or started twice).

### Debugging (off by default)

- RAM log (`common/ramlog.c`): with `CONFIG_RAMLOG` in
  `hiby_x1600_common.h`,
  everything U-Boot prints plus a USB trace is kept in RAM, survives a
  warm reset, and can be read with `fastboot fetch log` /
  `fastboot oem log` (and `lastlog` for the run before the reset). Meant
  for debugging without a serial console.

## 7. Recovery: the boot ROM's USB mode

`fastboot oem bootrom` resets the SoC into its mask ROM's USB boot mode
(USB `a108:eaef`), from which Ingenic's USB cloner tool can write the
flash with the `-withparts` image. This only works while U-Boot itself
still runs.

The ROM chooses the boot source from two pins at reset
(`boot_sel = PC28 << 1 | PC27`: 1 = SPI NAND, the normal setting;
3 = USB). With no working SPL in flash, reaching USB boot means driving
those pins.

On both players PC28 is a key (Next on the R1, Previous on the R3 Pro II;
the stock key scripts give it `active_level=1`, high when pressed). Held
at power-on, it should therefore give boot_sel 3, the ROM's USB boot,
without opening the player. This has not been tried.

## 8. Building

```sh
export PATH=$HOME/mips-gcc720-glibc229_br_linux_r5.0.2-gcc720-fp32/bin:$PATH
export CROSS_COMPILE=mips-linux-gnu-
cd u-boot
make distclean
make hiby_r1_xImage_sfc_nand        # or hiby_r3proii_xImage_sfc_nand
```

The result is `u-boot-with-spl.bin` (SPL padded to 26624 bytes, then
U-Boot). The `-withparts` copy gets bytes 0x5800–0x5bff (the partition
table) from the stock image, or from a dump of the stock `uboot`
partition (mtd0), which holds the same table; each player needs its own.
For the `-nouid` images, comment out `CONFIG_FASTBOOT_SERIAL_NAND_UID` in
`include/configs/hiby_x1600_common.h`.

With `make -j`, the SDK's Makefile sometimes stops with "multiple target
patterns" (a race in its dependency files): run `make distclean` and
build again.

Settings worth knowing. Per board, in `hiby_r1.h` / `hiby_r3proii.h`:

| Setting | What it sets |
| --- | --- |
| `CONFIG_HIBY_ADC_KEY_MIN_MV` / `MAX_MV` | Recovery key window (mV) |
| `CONFIG_HIBY_FASTBOOT_KEY_*`, `CONFIG_HIBY_NORMAL_KEY_*` | Fastboot and forced-normal key windows (mV) |
| `CONFIG_HIBY_LED_*` | LED pins, and the level that lights them (R1) |
| `CONFIG_FASTBOOT_BOARD_NAME`, `CONFIG_HIBY_HARDWARE_VER` | `getvar product` and version string, `getvar version-hardware` |
| `CONFIG_G_FASTBOOT_PRODUCT_NAME`, `CONFIG_G_FASTBOOT_INTERFACE_NAME` | USB names |

Shared, in `hiby_x1600_common.h`:

| Setting | What it sets |
| --- | --- |
| `CONFIG_FASTBOOT_SERIAL_NAND_UID` | Serial number from the NAND ID |
| `CONFIG_FASTBOOT_BOOTROM_SIZE` | `bootrom` fetch size |
| `CONFIG_RAMLOG` | The RAM log (off) |
