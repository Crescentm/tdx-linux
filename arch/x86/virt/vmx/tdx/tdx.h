/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _X86_VIRT_TDX_H
#define _X86_VIRT_TDX_H

#include <linux/types.h>
#include <linux/stddef.h>
#include <linux/compiler_attributes.h>

/*
 * This file contains both macros and data structures defined by the TDX
 * architecture and Linux defined software data structures and functions.
 * The two should not be mixed together for better readability.  The
 * architectural definitions come first.
 */

/*
 * TDX module SEAMCALL leaf functions
 */
#define TDH_PHYMEM_PAGE_RDMD	24
#define TDH_SYS_KEY_CONFIG	31
#define TDH_SYS_INFO		32
#define TDH_SYS_INIT		33
#define TDH_SYS_RD		34
#define TDH_SYS_LP_INIT		35
#define TDH_SYS_TDMR_INIT	36
#define TDH_SYS_CONFIG		45

/* TDX page types */
#define	PT_NDA		0x0
#define	PT_RSVD		0x1

/* CPUID induced SEAMCALL error */
#define TDX_INCORRECT_CPUID_VALUE	0xC000090000000000ULL

struct cmr_info {
	u64	base;
	u64	size;
} __packed;

#define MAX_CMRS	32

struct tdmr_reserved_area {
	u64 offset;
	u64 size;
} __packed;

#define TDMR_INFO_ALIGNMENT	512
#define TDMR_INFO_PA_ARRAY_ALIGNMENT	512

struct tdmr_info {
	u64 base;
	u64 size;
	u64 pamt_1g_base;
	u64 pamt_1g_size;
	u64 pamt_2m_base;
	u64 pamt_2m_size;
	u64 pamt_4k_base;
	u64 pamt_4k_size;
	/*
	 * Actual number of reserved areas depends on
	 * 'struct tdsysinfo_struct'::max_reserved_per_tdmr.
	 */
	DECLARE_FLEX_ARRAY(struct tdmr_reserved_area, reserved_areas);
} __packed __aligned(TDMR_INFO_ALIGNMENT);

/*
 * TDX module metadata identifiers
 */
#define TDX_MD_FEATURES0			0x0A00000300000008

/*
 * Do not put any hardware-defined TDX structure representations below
 * this comment!
 */

/* Kernel defined TDX module status during module initialization. */
enum tdx_module_status_t {
	TDX_MODULE_UNKNOWN,
	TDX_MODULE_INITIALIZED,
	TDX_MODULE_ERROR
};

struct tdx_memblock {
	struct list_head list;
	unsigned long start_pfn;
	unsigned long end_pfn;
	int nid;
};

/* Warn if kernel has less than TDMR_NR_WARN TDMRs after allocation */
#define TDMR_NR_WARN 4

struct tdmr_info_list {
	void *tdmrs;	/* Flexible array to hold 'tdmr_info's */
	int nr_consumed_tdmrs;	/* How many 'tdmr_info's are in use */

	/* Metadata for finding target 'tdmr_info' and freeing @tdmrs */
	int tdmr_sz;	/* Size of one 'tdmr_info' */
	int max_tdmrs;	/* How many 'tdmr_info's are allocated */
};

/* TDX metadata base field id. */
#define TDX_METADATA_ATTRIBUTES_FIXED0		0x1900000300000000ULL
#define TDX_METADATA_ATTRIBUTES_FIXED1		0x1900000300000001ULL
#define TDX_METADATA_XFAM_FIXED0		0x1900000300000002ULL
#define TDX_METADATA_XFAM_FIXED1		0x1900000300000003ULL
#define TDX_METADATA_NUM_CPUID_CONFIG		0x9900000100000004ULL
#define TDX_METADATA_CPUID_LEAVES		0x9900000300000400ULL
#define TDX_METADATA_CPUID_VALUES		0x9900000300000500ULL

/* File name for sysfs: hex with lower case. */
#define TDX_METADATA_ATTRIBUTES_FIXED0_NAME	"1900000300000000"
#define TDX_METADATA_ATTRIBUTES_FIXED1_NAME	"1900000300000001"
#define TDX_METADATA_XFAM_FIXED0_NAME		"1900000300000002"
#define TDX_METADATA_XFAM_FIXED1_NAME		"1900000300000003"
#define TDX_METADATA_NUM_CPUID_CONFIG_NAME	"9900000100000004"
#define TDX_METADATA_CPUID_LEAVES_NAME		"9900000300000400"
#define TDX_METADATA_CPUID_VALUES_NAME		"9900000300000500"

#endif
