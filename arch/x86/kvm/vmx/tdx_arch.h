/* SPDX-License-Identifier: GPL-2.0 */
/* architectural constants/data definitions for TDX SEAMCALLs */

#ifndef __KVM_X86_TDX_ARCH_H
#define __KVM_X86_TDX_ARCH_H

#include <linux/types.h>

/*
 * TDX SEAMCALL API function leaves
 */
#define TDH_VP_ENTER			0
#define TDH_MNG_ADDCX			1
#define TDH_MEM_PAGE_ADD		2
#define TDH_MEM_SEPT_ADD		3
#define TDH_VP_ADDCX			4
#define TDH_MEM_PAGE_RELOCATE		5
#define TDH_MEM_PAGE_AUG		6
#define TDH_MEM_RANGE_BLOCK		7
#define TDH_MNG_KEY_CONFIG		8
#define TDH_MNG_CREATE			9
#define TDH_VP_CREATE			10
#define TDH_MNG_RD			11
#define TDH_MEM_RD			12
#define TDH_MEM_WR			14
#define TDH_MEM_PAGE_DEMOTE		15
#define TDH_MR_EXTEND			16
#define TDH_MR_FINALIZE			17
#define TDH_VP_FLUSH			18
#define TDH_MNG_VPFLUSHDONE		19
#define TDH_MNG_KEY_FREEID		20
#define TDH_MNG_INIT			21
#define TDH_VP_INIT			22
#define TDH_MEM_PAGE_PROMOTE		23
#define TDH_MEM_SEPT_RD			25
#define TDH_VP_RD			26
#define TDH_MNG_KEY_RECLAIMID		27
#define TDH_PHYMEM_PAGE_RECLAIM		28
#define TDH_MEM_PAGE_REMOVE		29
#define TDH_MEM_SEPT_REMOVE		30
#define TDH_SYS_RD			34
#define TDH_MEM_TRACK			38
#define TDH_MEM_RANGE_UNBLOCK		39
#define TDH_PHYMEM_CACHE_WB		40
#define TDH_PHYMEM_PAGE_WBINVD		41
#define TDH_VP_WR			43
#define TDH_SYS_LP_SHUTDOWN		44
#define TDH_SERVTD_BIND			48
#define TDH_SERVTD_PREBIND		49
#define TDH_EXPORT_ABORT		64
#define TDH_EXPORT_BLOCKW		65
#define TDH_EXPORT_RESTORE		66
#define TDH_EXPORT_MEM			68
#define TDH_EXPORT_PAUSE		70
#define TDH_EXPORT_TRACK		71
#define TDH_EXPORT_STATE_IMMUTABLE	72
#define TDH_EXPORT_STATE_TD		73
#define TDH_EXPORT_STATE_VP		74
#define TDH_EXPORT_UNBLOCKW		75
#define TDH_IMPORT_END			81
#define TDH_IMPORT_COMMIT		82
#define TDH_IMPORT_MEM			83
#define TDH_IMPORT_TRACK		84
#define TDH_IMPORT_STATE_IMMUTABLE	85
#define TDH_IMPORT_STATE_TD		86
#define TDH_IMPORT_STATE_VP		87
#define TDH_MIG_STREAM_CREATE		96

#define TDG_VP_VMCALL_GET_TD_VM_CALL_INFO		0x10000
#define TDG_VP_VMCALL_MAP_GPA				0x10001
#define TDG_VP_VMCALL_GET_QUOTE				0x10002
#define TDG_VP_VMCALL_REPORT_FATAL_ERROR		0x10003
#define TDG_VP_VMCALL_SETUP_EVENT_NOTIFY_INTERRUPT	0x10004
#define TDG_VP_VMCALL_SERVICE				0x10005

#define TD_EXIT_OTHER_SMI_IS_MSMI	BIT(1)

/* TDX control structure (TDR/TDCS/TDVPS) field access codes */
#define TDX_NON_ARCH			BIT_ULL(63)
#define TDX_CLASS_SHIFT			56
#define TDX_FIELD_MASK			GENMASK_ULL(31, 0)

#define __BUILD_TDX_FIELD(non_arch, class, field)	\
	(((non_arch) ? TDX_NON_ARCH : 0) |		\
	 ((u64)(class) << TDX_CLASS_SHIFT) |		\
	 ((u64)(field) & TDX_FIELD_MASK))

#define BUILD_TDX_FIELD(class, field)			\
	__BUILD_TDX_FIELD(false, (class), (field))

#define BUILD_TDX_FIELD_NON_ARCH(class, field)		\
	__BUILD_TDX_FIELD(true, (class), (field))


/* Class code for TD */
#define TD_CLASS_EXECUTION_CONTROLS	17ULL

/* Class code for TDVPS */
#define TDVPS_CLASS_VMCS		0ULL
#define TDVPS_CLASS_GUEST_GPR		16ULL
#define TDVPS_CLASS_OTHER_GUEST		17ULL
#define TDVPS_CLASS_MANAGEMENT		32ULL

enum tdx_tdcs_execution_control {
	TD_TDCS_EXEC_TSC_OFFSET = 10,
};

/* @field is any of enum tdx_tdcs_execution_control */
#define TDCS_EXEC(field)		BUILD_TDX_FIELD(TD_CLASS_EXECUTION_CONTROLS, (field))

/* @field is the VMCS field encoding */
#define TDVPS_VMCS(field)		BUILD_TDX_FIELD(TDVPS_CLASS_VMCS, (field))

/* @field is enum tdx_vcpu_guest_gpr_state field encoding */
#define TDVPS_GPR(gpr)			BUILD_TDX_FIELD(TDVPS_CLASS_GUEST_GPR, (gpr))

enum tdx_vcpu_guest_other_state {
	TD_VCPU_DR0 = 0,
	TD_VCPU_DR1 = 1,
	TD_VCPU_DR2 = 2,
	TD_VCPU_DR3 = 3,
	TD_VCPU_DR6 = 6,
	TD_VCPU_XCR0 = 0x20,
	TD_VCPU_CR2 = 0x28,
	TD_VCPU_STATE_DETAILS_NON_ARCH = 0x100,
};

union tdx_vcpu_state_details {
	struct {
		u64 vmxip	: 1;
		u64 reserved	: 63;
	};
	u64 full;
};

/* @field is any of enum tdx_guest_other_state */
#define TDVPS_STATE(field)		BUILD_TDX_FIELD(TDVPS_CLASS_OTHER_GUEST, (field))
#define TDVPS_STATE_NON_ARCH(field)	BUILD_TDX_FIELD_NON_ARCH(TDVPS_CLASS_OTHER_GUEST, (field))

/* Management class fields */
enum tdx_vcpu_guest_management {
	TD_VCPU_PEND_NMI = 11,
};

/* @field is any of enum tdx_vcpu_guest_management */
#define TDVPS_MANAGEMENT(field)		BUILD_TDX_FIELD(TDVPS_CLASS_MANAGEMENT, (field))

#define TDX_EXTENDMR_CHUNKSIZE		256

struct tdx_cpuid_value {
	u32 eax;
	u32 ebx;
	u32 ecx;
	u32 edx;
} __packed;

#define TDX_TD_ATTRIBUTE_DEBUG		BIT_ULL(0)
#define TDX_TD_ATTRIBUTE_MIG		BIT_ULL(29)
#define TDX_TD_ATTRIBUTE_PKS		BIT_ULL(30)
#define TDX_TD_ATTRIBUTE_KL		BIT_ULL(31)
#define TDX_TD_ATTRIBUTE_PERFMON	BIT_ULL(63)

/*
 * TODO: Once XFEATURE_CET_{U, S} in arch/x86/include/asm/fpu/types.h is
 * defined, Replace these with define ones.
 */
#define TDX_TD_XFAM_CET	(BIT(11) | BIT(12))

/*
 * TD_PARAMS is provided as an input to TDH_MNG_INIT, the size of which is 1024B.
 */
#define TDX_MAX_VCPUS	(~(u16)0)

struct td_params {
	u64 attributes;
	u64 xfam;
	u16 max_vcpus;
	u8 reserved0[6];

	u64 eptp_controls;
	u64 exec_controls;
	u16 tsc_frequency;
	u8  reserved1[38];

	u64 mrconfigid[6];
	u64 mrowner[6];
	u64 mrownerconfig[6];
	u64 reserved2[4];

	union {
		struct tdx_cpuid_value cpuid_values[0];
		u8 reserved3[768];
	};
} __packed __aligned(1024);

/*
 * Guest uses MAX_PA for GPAW when set.
 * 0: GPA.SHARED bit is GPA[47]
 * 1: GPA.SHARED bit is GPA[51]
 */
#define TDX_EXEC_CONTROL_MAX_GPAW      BIT_ULL(0)

/*
 * TDH.VP.ENTER, TDG.VP.VMCALL preserves RBP
 * 0: RBP can be used for TDG.VP.VMCALL input. RBP is clobbered.
 * 1: RBP can't be used for TDG.VP.VMCALL input. RBP is preserved.
 */
#define TDX_CONTROL_FLAG_NO_BRP_MOD	BIT_ULL(1)


/*
 * TDX requires the frequency to be defined in units of 25MHz, which is the
 * frequency of the core crystal clock on TDX-capable platforms, i.e. the TDX
 * module can only program frequencies that are multiples of 25MHz.  The
 * frequency must be between 100mhz and 10ghz (inclusive).
 */
#define TDX_TSC_KHZ_TO_25MHZ(tsc_in_khz)	((tsc_in_khz) / (25 * 1000))
#define TDX_TSC_25MHZ_TO_KHZ(tsc_in_25mhz)	((tsc_in_25mhz) * (25 * 1000))
#define TDX_MIN_TSC_FREQUENCY_KHZ		(100 * 1000)
#define TDX_MAX_TSC_FREQUENCY_KHZ		(10 * 1000 * 1000)

/* The private memory reading/writing must be 8 bytes chunk  */
#define TDX_MEMORY_RW_CHUNK 8
#define TDX_MEMORY_RW_CHUNK_OFFSET_MASK (TDX_MEMORY_RW_CHUNK - 1)
#define TDX_MEMORY_RW_CHUNK_MASK (~TDX_MEMORY_RW_CHUNK_OFFSET_MASK)

union tdx_ext_exit_qualification {
	struct {
		u64 type		:  4;
		u64 reserved0		: 28;
		u64 req_sept_level	:  3;
		u64 err_sept_level	:  3;
		u64 err_sept_state	:  8;
		u64 err_sept_is_leaf	:  1;
		u64 reserved1		: 17;
	};
	u64 full;
};

enum tdx_ext_exit_qualification_type {
	EXT_EXIT_QUAL_NONE = 0,
	EXT_EXIT_QUAL_ACCEPT,
	NUM_EXT_EXIT_QUAL,
};

union tdx_sept_entry {
	struct {
		u64 r		:  1;
		u64 w		:  1;
		u64 x		:  1;
		u64 mt		:  3;
		u64 ipat	:  1;
		u64 leaf	:  1;
		u64 reserved0	:  3;
		u64 ignored0	:  1;
		u64 pfn		: 40;
		u64 reserved1	:  2;
		u64 ignored1	:  1;
		u64 reserved2	:  2;
		u64 ignored2	:  1;
		u64 sve		:  1;
	};
	u64 raw;
};

enum tdx_sept_entry_state {
	TDX_SEPT_FREE = 0,
	TDX_SEPT_BLOCKED = 1,
	TDX_SEPT_PENDING = 2,
	TDX_SEPT_PENDING_BLOCKED = 3,
	TDX_SEPT_PRESENT = 4,
};

union tdx_sept_level_state {
	struct {
		u64 level	: 3;
		u64 reserved0	: 5;
		u64 state	: 8;
		u64 reserved1	: 48;
	};
	u64 raw;
};

union tdx_md_field_id {
	struct {
		u64 field			: 24;
		u64 reserved0			: 8;
		u64 element_size		: 2;
		u64 last_element_in_field	: 4;
		u64 reserved1			: 3;
		u64 inc_size			: 1;
		u64 write_mask_valid		: 1;
		u64 context			: 3;
		u64 reserved2			: 1;
		u64 class			: 6;
		u64 reserved3			: 1;
		u64 non_arch			: 1;
	};
	u64 raw;
};

#define TDX_MD_CLASS_GLOBAL_VERSION		8

#define TDX_MD_FID_GLOBAL_FEATURES0		0x0A00000300000008

/* FIXME: Once the bit is determined, replace this with the correct value. */
#define TDX_MD_FID_GLBOAL_FEATURES0_NO_BRP_MOD	BIT_ULL(63)

/* The field id used to access the metadata (defined in the TDX ABI spec) */
#define TDX_MD_FID_SERVTD_MAX_SERVTDS		0xA100000100000000
#define TDX_MD_FID_MAX_MIGS			0xA000000100000010
#define TDX_MD_FID_IMMUTABLE_STATE_PAGES	0xA000000000000020
#define TDX_MD_FID_TD_STATE_PAGES		0xA000000000000021
#define TDX_MD_FID_VP_STATE_PAGES		0xA000000000000022

#endif /* __KVM_X86_TDX_ARCH_H */
