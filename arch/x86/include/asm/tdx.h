/* SPDX-License-Identifier: GPL-2.0 */
/* Copyright (C) 2021-2022 Intel Corporation */
#ifndef _ASM_X86_TDX_H
#define _ASM_X86_TDX_H

#include <linux/init.h>
#include <linux/bits.h>

#include <asm/errno.h>
#include <asm/ptrace.h>
#include <asm/trapnr.h>
#include <asm/shared/tdx.h>

/*
 * SW-defined error codes.
 *
 * Bits 47:40 == 0xFF indicate Reserved status code class that never used by
 * TDX module.
 */
#define TDX_ERROR			_BITULL(63)
#define TDX_SW_ERROR			(TDX_ERROR | GENMASK_ULL(47, 40))
#define TDX_SEAMCALL_VMFAILINVALID	(TDX_SW_ERROR | _ULL(0xFFFF0000))

#define TDX_SEAMCALL_GP			(TDX_SW_ERROR | X86_TRAP_GP)
#define TDX_SEAMCALL_UD			(TDX_SW_ERROR | X86_TRAP_UD)

#define TDX_NON_RECOVERABLE_BIT		62
/*
 * Error with the non-recoverable bit cleared indicates that the error is
 * likely recoverable (e.g. due to lock busy in TDX module), and the seamcall
 * can be retried.
 */
#define TDX_SEAMCALL_ERR_RECOVERABLE(err) \
	(err >> TDX_NON_RECOVERABLE_BIT == 0x2)

/* The max number of seamcall retries */
#define TDX_SEAMCALL_RETRY_MAX	10000

#ifndef __ASSEMBLY__

/*
 * Used by the #VE exception handler to gather the #VE exception
 * info from the TDX module. This is a software only structure
 * and not part of the TDX module/VMM ABI.
 */
struct ve_info {
	u64 exit_reason;
	u64 exit_qual;
	/* Guest Linear (virtual) Address */
	u64 gla;
	/* Guest Physical Address */
	u64 gpa;
	u32 instr_len;
	u32 instr_info;
};

#ifdef CONFIG_INTEL_TDX_GUEST

extern int tdx_notify_irq;

void __init tdx_early_init(void);
bool tdx_debug_enabled(void);

void tdx_get_ve_info(struct ve_info *ve);

void __init tdx_filter_init(void);

bool tdx_handle_virt_exception(struct pt_regs *regs, struct ve_info *ve);

void tdx_safe_halt(void);

bool tdx_early_handle_ve(struct pt_regs *regs);

int tdx_mcall_get_report0(u8 *reportdata, u8 *tdreport);

bool tdx_allowed_port(int port);

u64 tdx_mcall_verify_report(u8 *reportmac);

int tdx_mcall_extend_rtmr(u8 *data, u8 index);

int tdx_hcall_get_quote(void *tdquote, int size);

int tdx_alloc_event_irq(void);

void tdx_free_event_irq(int);

#else

static inline void tdx_early_init(void) { };
static inline void tdx_safe_halt(void) { };
static inline void tdx_filter_init(void) { };

static inline bool tdx_early_handle_ve(struct pt_regs *regs) { return false; }

#endif /* CONFIG_INTEL_TDX_GUEST */

#if defined(CONFIG_KVM_GUEST) && defined(CONFIG_INTEL_TDX_GUEST)
long tdx_kvm_hypercall(unsigned int nr, unsigned long p1, unsigned long p2,
		       unsigned long p3, unsigned long p4);
#else
static inline long tdx_kvm_hypercall(unsigned int nr, unsigned long p1,
				     unsigned long p2, unsigned long p3,
				     unsigned long p4)
{
	return -ENODEV;
}
#endif /* CONFIG_INTEL_TDX_GUEST && CONFIG_KVM_GUEST */

#ifdef CONFIG_INTEL_TDX_HOST
u64 __seamcall(u64 fn, struct tdx_module_args *args);
u64 __seamcall_ret(u64 fn, struct tdx_module_args *args);
u64 __seamcall_saved_ret(u64 fn, struct tdx_module_args *args);

#define DEBUGCONFIG_TRACE_ALL		0
#define DEBUGCONFIG_TRACE_WARN		1
#define DEBUGCONFIG_TRACE_ERROR		2
#define DEBUGCONFIG_TRACE_CUSTOM	1000
#define DEBUGCONFIG_TRACE_NONE		-1ULL
void tdx_trace_seamcalls(u64 level);

/* -1 indicates CPUID leaf with no sub-leaves. */
#define TDX_CPUID_NO_SUBLEAF	((u32)-1)
struct tdx_cpuid_config {
	__struct_group(tdx_cpuid_config_leaf, leaf_sub_leaf, __packed,
		u32 leaf;
		u32 sub_leaf;
	);
	__struct_group(tdx_cpuid_config_value, value, __packed,
		u32 eax;
		u32 ebx;
		u32 ecx;
		u32 edx;
	);
} __packed;

#define TDSYSINFO_STRUCT_SIZE		1024

/*
 * The size of this structure itself is flexible.  The actual structure
 * passed to TDH.SYS.INFO must be padded to 1024 bytes and be 1204-bytes
 * aligned.
 */
#define TDSYSINFO_ATTRIBUTES_DEBUG	BIT(31)
struct tdsysinfo_struct {
	/* TDX-SEAM Module Info */
	u32	attributes;
	u32	vendor_id;
	u32	build_date;
	u16	build_num;
	u16	minor_version;
	u16	major_version;
	u8	sys_rd;
	u8	reserved0[13];
	/* Memory Info */
	u16	max_tdmrs;
	u16	max_reserved_per_tdmr;
	u16	pamt_entry_size;
	u8	reserved1[10];
	/* Control Struct Info */
	u16	tdcs_base_size;
	u8	reserved2[2];
	u16	tdvps_base_size;
	u8	tdvps_xfam_dependent_size;
	u8	reserved3[9];
	/* TD Capabilities */
	u64	attributes_fixed0;
	u64	attributes_fixed1;
	u64	xfam_fixed0;
	u64	xfam_fixed1;
	u8	reserved4[32];
	u32	num_cpuid_config;
	/*
	 * The actual number of CPUID_CONFIG depends on above
	 * 'num_cpuid_config'.
	 */
	DECLARE_FLEX_ARRAY(struct tdx_cpuid_config, cpuid_configs);
} __packed;

const struct tdsysinfo_struct *tdx_get_sysinfo(void);
bool platform_tdx_enabled(void);
int tdx_enable(void);
void tdx_reset_memory(void);
bool tdx_is_private_mem(unsigned long phys);

/*
 * Key id globally used by TDX module: TDX module maps TDR with this TDX global
 * key id.  TDR includes key id assigned to the TD.  Then TDX module maps other
 * TD-related pages with the assigned key id.  TDR requires this TDX global key
 * id for cache flush unlike other TD-related pages.
 */
extern u32 tdx_global_keyid;
u32 tdx_get_nr_guest_keyids(void);
int tdx_guest_keyid_alloc(void);
void tdx_guest_keyid_free(int keyid);
bool tdx_is_enabled(void);
int __init tdx_init(void);
#else
static inline u64 __seamcall(u64 fn, struct tdx_module_args *args) { return TDX_SEAMCALL_UD; }
static inline u64 __seamcall_ret(u64 fn, struct tdx_module_args *args) { return TDX_SEAMCALL_UD; }
static inline u64 __seamcall_saved_ret(u64 fn, struct tdx_module_args *args) { return TDX_SEAMCALL_UD; }

struct tdsysinfo_struct;
static inline const struct tdsysinfo_struct *tdx_get_sysinfo(void) { return NULL; }
static inline bool platform_tdx_enabled(void) { return false; }
static inline int tdx_enable(void)  { return -ENODEV; }
static inline void tdx_reset_memory(void) { }
static inline bool tdx_is_private_mem(unsigned long phys) { return false; }
static inline u32 tdx_get_nr_guest_keyids(void) { return 0; }
static inline int tdx_guest_keyid_alloc(void) { return -EOPNOTSUPP; }
static inline void tdx_guest_keyid_free(int keyid) { }
static inline bool tdx_is_enabled(void) { return false };
static inline int __init tdx_init(void) { return 0; }
#endif	/* CONFIG_INTEL_TDX_HOST */

#endif /* !__ASSEMBLY__ */
#endif /* _ASM_X86_TDX_H */
