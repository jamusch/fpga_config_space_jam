#include <stdint.h>

/*
 * All structures are 64 bytes long and are expected
 * to live in an array, one for each interconnect.
 * Most fields of the structures are shared among the
 * various types, and most-specific fields are at the
 * beginning (for alignment reasons, and to keep the
 * magic number at the head of the interconnect record
 */

/* Product, 40 bytes at offset 24, 8-byte alignmed
 *
 * device_id is vendor-assigned; version is device-specific,
 * date is hex (e.g 0x20120501), name is UTF-8, blank-filled
 * and not terminated with a 0 byte.
 */
struct sdb_product {
	uint64_t		vendor_id;	/* 0x18..0x1f */
	uint32_t		device_id;	/* 0x20..0x23 */
	uint32_t		version;	/* 0x24..0x27 */
	uint32_t		date;		/* 0x28..0x2b */
	uint8_t			name[19];	/* 0x2c..0x3e */
	uint8_t			record_type;	/* 0x3f */
};

/*
 * Component, 56 bytes at offset 8, 8-byte aligned 
 *
 * The address range is first to last, inclusive
 * (for example 0x100000 - 0x10ffff)
 */
struct sdb_component {
	uint64_t		addr_first;	/* 0x08..0x0f */
	uint64_t		addr_last;	/* 0x10..0x17 */
	struct sdb_product	product;	/* 0x18..0x3f */
};

/* Type of the SDB record */
enum sdb_record_type {
	sdb_type_interconnect = 0x00,
	sdb_type_device       = 0x01,
	sdb_type_bridge       = 0x02,
	sdb_type_integration  = 0x80,
	sdb_type_empty        = 0xFF,
};

/* Type 0: interconnect (first of the array)
 *
 * magic ix 0x5344422d, sdb_records is the length of the table
 * including this first record, version is 1. The bus type
 * is enumerated later.
 */
struct sdb_interconnect {
	uint32_t		sdb_magic;	/* 0x00-0x03 */
	uint16_t		sdb_records;	/* 0x04-0x05 */
	uint8_t			sdb_version;	/* 0x06 */
	uint8_t			sdb_bus_type;	/* 0x07 */
	struct sdb_component	sdb_component;	/* 0x08-0x3f */
};

/* Type 1: device
 *
 * class is 0 for "custom device", other values are
 * to be standardized; ABI version is for the driver,
 * bus-specific bits are defined by each bus (see below)
 */
struct sdb_device {
	uint16_t		abi_class;	/* 0x00-0x01 */
	uint8_t			abi_ver_major;	/* 0x02 */
	uint8_t			abi_ver_minor;	/* 0x03 */
	uint32_t		bus_specific;	/* 0x04-0x07 */
	struct sdb_component	sdb_component;	/* 0x08-0x3f */
};

/* Type 2: bridge
 *
 * child is the address of the nested SDB table
 */
struct sdb_bridge {
	uint64_t		sdb_child;	/* 0x00-0x07 */
	struct sdb_component	sdb_component;	/* 0x08-0x3f */
};

/* Type 0x80: integration
 *
 * all types with by 7 set are meta-information, so
 * software can ignore the types it doesn't know. Here
 * we just provide product information for the overall FPGA
 */
struct sdb_integration {
	uint8_t			reserved[24];	/* 0x00-0x17 */
	struct sdb_product	product;	/* 0x08-0x3f */
};

/* Type 0xff: empty
 *
 * this allows keeping empty slots during development,
 * so they can be filled later with miminal efforts and
 * no misleading description is ever shipped -- hopefully
 */
struct sdb_integration {
	uint8_t			reserved[63];	/* 0x00-0x3e */
	uint8_t			record_type;	/* 0x3f */
};

/* The type of bus, for bus-specific flags (currently only Wishbone) */
enum sdb_bus_type {
	sdb_wishbone = 0x00
};

#define SDB_WB_WIDTH_MASK		0x0f
#define SDB_WB_ACCESS8		0x01
#define SDB_WB_ACCESS16		0x02
#define SDB_WB_ACCESS32		0x04
#define SDB_WB_ACCESS64		0x08

#define SDB_WB_LITTLE_ENDIAN		0x80
