#include <config.h>
#include <common.h>
#include <asm/io.h>
#include <ddr/ddr_common.h>


#define EFUSE_BASE	0xB3540000
#define EFUSE_CTRL	EFUSE_BASE + 0x0
#define EFUSE_CFG	EFUSE_BASE + 0x4
#define EFUSE_STATE	EFUSE_BASE + 0x8
#define EFUSE_DATA	EFUSE_BASE + 0xC

#define EFUSE_CTRL_ADDR_POS	(21)
#define EFUSE_CTRL_LEN_POS	(16)

#define EFUSE_CTRL_RDEN         (1 << 0)
#define EFUSE_STAT_RDDONE       (1 << 0)

#define EFUSE_CFG_RD_ADJ_POS	(20)
#define EFUSE_CFG_RD_STROBE_POS	(16)

#define SOCINFO_ADDR	        0x2B

#define REG32(addr) *(volatile unsigned int *)(addr)

static unsigned int read_socid()
{
	unsigned int val, data;

	REG32(EFUSE_CFG) = 0x2 << EFUSE_CFG_RD_ADJ_POS | 0x5 << EFUSE_CFG_RD_STROBE_POS;

	REG32(EFUSE_CTRL) = 0;
	REG32(EFUSE_CTRL) = SOCINFO_ADDR << EFUSE_CTRL_ADDR_POS | 2 << EFUSE_CTRL_LEN_POS | EFUSE_CTRL_RDEN;
	while(!(REG32(EFUSE_STATE) & EFUSE_STAT_RDDONE));

	data = REG32(EFUSE_DATA);
	val = data & 0xFFFF;

	return val;
}

#ifdef CONFIG_X1600_SOCID_TABLE
/*
 * Known X1600-family SoC IDs (eFuse word 0x2B, low 16 bits).
 * bits[14]=DDR type, bits[13:11]=DDR vendor, bits[10:8]=DDR capacity,
 * bits[7:0]=variant.
 */
static const struct {
	unsigned int socid;
	const char *name;
} x1600_soc_table[] = {
	{ 0x1b00, "X1600"   },
	{ 0x0a00, "X1600E"  },
	{ 0x4b00, "X1600N"  },
	{ 0x4a00, "X1600EN" },
	{ 0x4900, "X1600HN" },
	{ 0x6500, "X1600MN" },
	{ 0x1b01, "X1660"   },
	{ 0x4600, "X1660L"  },
};

/*
 * Look up the SoC in x1600_soc_table. On a match, stores the DDR chip ID
 * in *ddrid and the SoC name in *name (either may be NULL) and returns 0.
 * Returns -1 for a blank or unknown SoC ID.
 */
int check_socid_table(unsigned int *ddrid, const char **name)
{
	unsigned int socid = read_socid();
	int i;

	if (!socid)
		return -1;

	if (ddrid)
		*ddrid = DDR_CHIP_ID(socid >> 11 & 0x7, socid >> 14 & 0x1,
				     socid >> 8 & 0x7);

	for (i = 0; i < ARRAY_SIZE(x1600_soc_table); i++) {
		if (x1600_soc_table[i].socid == socid) {
			if (name)
				*name = x1600_soc_table[i].name;
			return 0;
		}
	}

	return -1;
}
#endif /* CONFIG_X1600_SOCID_TABLE */

unsigned int check_socid()
{
	unsigned int vendor = 0;
	unsigned int type = 0;
	unsigned int capacity = 0;
	unsigned int socid  = 0;
	unsigned int ddrid  = 0;

	socid = read_socid();
	vendor = socid >> 11 & 0x7;
	type   = socid >> 14 & 0x1;
	capacity = socid >> 8 & 0x7;
	ddrid = DDR_CHIP_ID(vendor, type, capacity);

	return ddrid;
}
