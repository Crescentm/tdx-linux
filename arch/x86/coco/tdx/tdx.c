// SPDX-License-Identifier: GPL-2.0
/* Copyright (C) 2021-2022 Intel Corporation */

#undef pr_fmt
#define pr_fmt(fmt)     "tdx: " fmt

#include <linux/cpufeature.h>
#include <linux/export.h>
#include <linux/io.h>
#include <linux/string.h>
#include <linux/uaccess.h>
#include <linux/interrupt.h>
#include <linux/irq.h>
#include <linux/numa.h>
#include <linux/platform_device.h>
#include <linux/pci.h>
#include <linux/random.h>
#include <linux/virtio_anchor.h>
#include <asm/coco.h>
#include <asm/tdx.h>
#include <asm/i8259.h>
#include <asm/vmx.h>
#include <asm/insn.h>
#include <asm/insn-eval.h>
#include <asm/pgtable.h>
#include <asm/irqdomain.h>

#define CREATE_TRACE_POINTS
#include <asm/trace/tdx.h>

/* MMIO direction */
#define EPT_READ	0
#define EPT_WRITE	1

/* Port I/O direction */
#define PORT_READ	0
#define PORT_WRITE	1

/* See Exit Qualification for I/O Instructions in VMX documentation */
#define VE_IS_IO_IN(e)		((e) & BIT(3))
#define VE_GET_IO_SIZE(e)	(((e) & GENMASK(2, 0)) + 1)
#define VE_GET_PORT_NUM(e)	((e) >> 16)
#define VE_IS_IO_STRING(e)	((e) & BIT(4))

#define ATTR_DEBUG		BIT(0)
#define ATTR_SEPT_VE_DISABLE	BIT(28)

/* TDX Module call error codes */
#define TDCALL_RETURN_CODE(a)	((a) >> 32)
#define TDCALL_INVALID_OPERAND	0xc0000100
#define TDCALL_OPERAND_BUSY	0x80000200

#define TDREPORT_SUBTYPE_0	0

/* Caches TD Attributes from TDG.VP.INFO TDCALL */
static u64 td_attr;

int tdx_notify_irq = -1;
EXPORT_SYMBOL_GPL(tdx_notify_irq);

/* Traced version of __tdx_hypercall */
static u64 __trace_tdx_hypercall(struct tdx_module_args *args)
{
	u64 err;

	trace_tdx_hypercall_enter_rcuidle(args->r11, args->r12, args->r13,
			args->r14, args->r15);
	err = __tdx_hypercall(args);
	trace_tdx_hypercall_exit_rcuidle(err, args->r11, args->r12,
			args->r13, args->r14, args->r15);

	return err;
}

/* Traced version of __tdx_module_call */
static u64 __trace_tdcall_ret(u64 fn, struct tdx_module_args *args)
{
	struct tdx_module_args dummy_out;
	u64 err;

	if (!args)
		args = &dummy_out;

	trace_tdx_module_call_enter_rcuidle(fn, args->rcx, args->rdx, args->r8, args->r9);
	err = tdcall_ret(fn, args);
	trace_tdx_module_call_exit_rcuidle(err, args->rcx, args->rdx,
			args->r8, args->r9, args->r10, args->r11);

	return err;
}

/* Called from __tdx_hypercall() for unrecoverable failure */
noinstr void __noreturn __tdx_hypercall_failed(void)
{
	instrumentation_begin();
	panic("TDVMCALL failed. TDX module bug?");
}

#ifdef CONFIG_KVM_GUEST
long tdx_kvm_hypercall(unsigned int nr, unsigned long p1, unsigned long p2,
		       unsigned long p3, unsigned long p4)
{
	struct tdx_module_args args = {
		.r10 = nr,
		.r11 = p1,
		.r12 = p2,
		.r13 = p3,
		.r14 = p4,
	};

	return __trace_tdx_hypercall(&args);
}
EXPORT_SYMBOL_GPL(tdx_kvm_hypercall);
#endif

/*
 * Used for TDX guests to make calls directly to the TD module.  This
 * should only be used for calls that have no legitimate reason to fail
 * or where the kernel can not survive the call failing.
 */
static inline void tdcall_ret_with_trace(u64 fn, struct tdx_module_args *args)
{
	if (__trace_tdcall_ret(fn, args))
		panic("TDCALL %lld failed (Buggy TDX module!)\n", fn);
}

/**
 * tdx_mcall_get_report0() - Wrapper to get TDREPORT0 (a.k.a. TDREPORT
 *                           subtype 0) using TDG.MR.REPORT TDCALL.
 * @reportdata: Address of the input buffer which contains user-defined
 *              REPORTDATA to be included into TDREPORT.
 * @tdreport: Address of the output buffer to store TDREPORT.
 *
 * Refer to section titled "TDG.MR.REPORT leaf" in the TDX Module
 * v1.0 specification for more information on TDG.MR.REPORT TDCALL.
 * It is used in the TDX guest driver module to get the TDREPORT0.
 *
 * Return 0 on success, -EINVAL for invalid operands, or -EIO on
 * other TDCALL failures.
 */
int tdx_mcall_get_report0(u8 *reportdata, u8 *tdreport)
{
	struct tdx_module_args args = {
		.rcx = virt_to_phys(tdreport),
		.rdx = virt_to_phys(reportdata),
		.r8 = TDREPORT_SUBTYPE_0,
	};
	u64 ret;

	ret = tdcall(TDG_MR_REPORT, &args);
	if (ret) {
		if (TDCALL_RETURN_CODE(ret) == TDCALL_INVALID_OPERAND)
			return -EINVAL;
		return -EIO;
	}

	return 0;
}
EXPORT_SYMBOL_GPL(tdx_mcall_get_report0);

static void __noreturn tdx_panic(const char *msg)
{
	struct tdx_module_args args = {
		.r10 = TDX_HYPERCALL_STANDARD,
		.r11 = TDVMCALL_REPORT_FATAL_ERROR,
		.r12 = 0, /* Error code: 0 is Panic */
	};
	union {
		/* Define register order according to the GHCI */
		struct { u64 r14, r15, rbx, rdi, rsi, r8, r9, rdx; };

		char str[64];
	} message;

	/* VMM assumes '\0' in byte 65, if the message took all 64 bytes */
	strncpy(message.str, msg, 64);

	args.r8  = message.r8;
	args.r9  = message.r9;
	args.r14 = message.r14;
	args.r15 = message.r15;
	args.rdi = message.rdi;
	args.rsi = message.rsi;
	args.rbx = message.rbx;
	args.rdx = message.rdx;

	/*
	 * This hypercall should never return and it is not safe
	 * to keep the guest running. Call it forever if it
	 * happens to return.
	 */
	while (1)
		__tdx_hypercall(&args);
}

/**
 * tdx_mcall_verify_report() - Wrapper for TDG.MR.VERIFYREPORT TDCALL.
 * @reportmac: Address of the input buffer which contains REPORTMACSTRUCT.
 *
 * Refer to section titled "TDG.MR.VERIFYREPORT leaf" in the TDX
 * Module v1.0 specification for more information on TDG.MR.VERIFYREPORT
 * TDCALL. It is used in the TDX guest driver module to verify the
 * REPORTMACSTRUCT (part of TDREPORT struct which was generated via
 * TDG.MR.TDREPORT TDCALL).
 *
 * Return 0 on success, or error code on other TDCALL failures.
 */
u64 tdx_mcall_verify_report(u8 *reportmac)
{
	struct tdx_module_args args = {
		.rcx = virt_to_phys(reportmac),
	};
	return tdcall(TDG_VERIFYREPORT, &args);
}
EXPORT_SYMBOL_GPL(tdx_mcall_verify_report);

/**
 * tdx_mcall_extend_rtmr() - Wrapper to extend RTMR registers using
 *                           TDG.MR.RTMR.EXTEND TDCALL.
 * @data: Address of the input buffer with RTMR register extend data.
 * @index: Index of RTMR register to be extended.
 *
 * Refer to section titled "TDG.MR.RTMR.EXTEND leaf" in the TDX Module
 * v1.0 specification for more information on TDG.MR.RTMR.EXTEND TDCALL.
 * It is used in the TDX guest driver module to allow user extend the
 * RTMR registers (index > 1).
 *
 * Return 0 on success, -EINVAL for invalid operands, -EBUSY for busy
 * operation or -EIO on other TDCALL failures.
 */
int tdx_mcall_extend_rtmr(u8 *data, u8 index)
{
	struct tdx_module_args args = {
		.rcx = virt_to_phys(data),
		.rdx = index,
	};
	u64 ret;

	ret = tdcall(TDG_EXTEND_RTMR, &args);
	if (ret) {
		if (TDCALL_RETURN_CODE(ret) == TDCALL_INVALID_OPERAND)
			return -EINVAL;
		if (TDCALL_RETURN_CODE(ret) == TDCALL_OPERAND_BUSY)
			return -EBUSY;
		return -EIO;
	}

	return 0;
}
EXPORT_SYMBOL_GPL(tdx_mcall_extend_rtmr);

/**
 * tdx_hcall_get_quote() - Wrapper to get TD Quote using GetQuote
 *                         hypercall.
 * @tdquote: Address of the input buffer which contains TDREPORT
 *           data. The same buffer will be used by VMM to store
 *           the generated TD Quote output.
 * @size: size of the tdquote buffer.
 *
 * Refer to section titled "TDG.VP.VMCALL<GetQuote>" in the TDX GHCI
 * v1.0 specification for more information on GetQuote hypercall.
 * It is used in the TDX guest driver module to get the TD Quote.
 *
 * Return 0 on success, -EINVAL for invalid operands, or -EIO on
 * other TDCALL failures.
 */
int tdx_hcall_get_quote(void *tdquote, int size)
{
        struct tdx_module_args args = {0};

        args.r10 = TDX_HYPERCALL_STANDARD;
        args.r11 = TDVMCALL_GET_QUOTE;
        args.r12 = cc_mkdec(virt_to_phys(tdquote));
        args.r13 = size;

	/*
	 * Pass the physical address of TDREPORT to the VMM and
	 * trigger the Quote generation. It is not a blocking
	 * call, hence completion of this request will be notified to
	 * the TD guest via a callback interrupt.
	 */
	return __tdx_hypercall(&args);
}
EXPORT_SYMBOL_GPL(tdx_hcall_get_quote);

static void tdx_parse_tdinfo(u64 *cc_mask)
{
	struct tdx_module_args args = {};
	unsigned int gpa_width;

	/*
	 * TDINFO TDX module call is used to get the TD execution environment
	 * information like GPA width, number of available vcpus, debug mode
	 * information, etc. More details about the ABI can be found in TDX
	 * Guest-Host-Communication Interface (GHCI), section 2.4.2 TDCALL
	 * [TDG.VP.INFO].
	 */
	tdcall_ret_with_trace(TDG_VP_INFO, &args);

	/*
	 * The highest bit of a guest physical address is the "sharing" bit.
	 * Set it for shared pages and clear it for private pages.
	 *
	 * The GPA width that comes out of this call is critical. TDX guests
	 * can not meaningfully run without it.
	 */
	gpa_width = args.rcx & GENMASK(5, 0);
	*cc_mask = BIT_ULL(gpa_width - 1);

	/*
	 * The kernel can not handle #VE's when accessing normal kernel
	 * memory.  Ensure that no #VE will be delivered for accesses to
	 * TD-private memory.  Only VMM-shared memory (MMIO) will #VE.
	 */
	td_attr = args.rdx;
	if (!(td_attr & ATTR_SEPT_VE_DISABLE)) {
		const char *msg = "TD misconfiguration: SEPT_VE_DISABLE attribute must be set.";

		/* Relax SEPT_VE_DISABLE check for debug TD. */
		if (td_attr & ATTR_DEBUG)
			pr_warn("%s\n", msg);
		else
			tdx_panic(msg);
	}
}

/*
 * The TDX module spec states that #VE may be injected for a limited set of
 * reasons:
 *
 *  - Emulation of the architectural #VE injection on EPT violation;
 *
 *  - As a result of guest TD execution of a disallowed instruction,
 *    a disallowed MSR access, or CPUID virtualization;
 *
 *  - A notification to the guest TD about anomalous behavior;
 *
 * The last one is opt-in and is not used by the kernel.
 *
 * The Intel Software Developer's Manual describes cases when instruction
 * length field can be used in section "Information for VM Exits Due to
 * Instruction Execution".
 *
 * For TDX, it ultimately means GET_VEINFO provides reliable instruction length
 * information if #VE occurred due to instruction execution, but not for EPT
 * violations.
 */
static int ve_instr_len(struct ve_info *ve)
{
	switch (ve->exit_reason) {
	case EXIT_REASON_HLT:
	case EXIT_REASON_MSR_READ:
	case EXIT_REASON_MSR_WRITE:
	case EXIT_REASON_CPUID:
	case EXIT_REASON_IO_INSTRUCTION:
		/* It is safe to use ve->instr_len for #VE due instructions */
		return ve->instr_len;
	case EXIT_REASON_EPT_VIOLATION:
		/*
		 * For EPT violations, ve->insn_len is not defined. For those,
		 * the kernel must decode instructions manually and should not
		 * be using this function.
		 */
		WARN_ONCE(1, "ve->instr_len is not defined for EPT violations");
		return 0;
	default:
		WARN_ONCE(1, "Unexpected #VE-type: %lld\n", ve->exit_reason);
		return ve->instr_len;
	}
}

bool tdx_debug_enabled(void)
{
	return !!(td_attr & ATTR_DEBUG);
}

static u64 __cpuidle __halt(const bool irq_disabled)
{
	struct tdx_module_args args = {
		.r10 = TDX_HYPERCALL_STANDARD,
		.r11 = hcall_func(EXIT_REASON_HLT),
		.r12 = irq_disabled,
	};

	/*
	 * Emulate HLT operation via hypercall. More info about ABI
	 * can be found in TDX Guest-Host-Communication Interface
	 * (GHCI), section 3.8 TDG.VP.VMCALL<Instruction.HLT>.
	 *
	 * The VMM uses the "IRQ disabled" param to understand IRQ
	 * enabled status (RFLAGS.IF) of the TD guest and to determine
	 * whether or not it should schedule the halted vCPU if an
	 * IRQ becomes pending. E.g. if IRQs are disabled, the VMM
	 * can keep the vCPU in virtual HLT, even if an IRQ is
	 * pending, without hanging/breaking the guest.
	 */
	return __trace_tdx_hypercall(&args);
}

static int handle_halt(struct ve_info *ve)
{
	const bool irq_disabled = irqs_disabled();

	if (__halt(irq_disabled))
		return -EIO;

	return ve_instr_len(ve);
}

void __cpuidle tdx_safe_halt(void)
{
	const bool irq_disabled = false;

	/*
	 * Use WARN_ONCE() to report the failure.
	 */
	if (__halt(irq_disabled))
		WARN_ONCE(1, "HLT instruction emulation failed\n");
}

static int read_msr(struct pt_regs *regs, struct ve_info *ve)
{
	struct tdx_module_args args = {
		.r10 = TDX_HYPERCALL_STANDARD,
		.r11 = hcall_func(EXIT_REASON_MSR_READ),
		.r12 = regs->cx,
	};

	/*
	 * Emulate the MSR read via hypercall. More info about ABI
	 * can be found in TDX Guest-Host-Communication Interface
	 * (GHCI), section titled "TDG.VP.VMCALL<Instruction.RDMSR>".
	 */
	if (__trace_tdx_hypercall(&args))
		return -EIO;

	regs->ax = lower_32_bits(args.r11);
	regs->dx = upper_32_bits(args.r11);
	return ve_instr_len(ve);
}

static int write_msr(struct pt_regs *regs, struct ve_info *ve)
{
	struct tdx_module_args args = {
		.r10 = TDX_HYPERCALL_STANDARD,
		.r11 = hcall_func(EXIT_REASON_MSR_WRITE),
		.r12 = regs->cx,
		.r13 = (u64)regs->dx << 32 | regs->ax,
	};

	/*
	 * Emulate the MSR write via hypercall. More info about ABI
	 * can be found in TDX Guest-Host-Communication Interface
	 * (GHCI) section titled "TDG.VP.VMCALL<Instruction.WRMSR>".
	 */
	if (__trace_tdx_hypercall(&args))
		return -EIO;

	return ve_instr_len(ve);
}

/*
 * TDX has context switched MSRs and emulated MSRs. The emulated MSRs
 * normally trigger a #VE, but that is expensive, which can be avoided
 * by doing a direct TDCALL. Unfortunately, this cannot be done for all
 * because some MSRs are "context switched" and need WRMSR.
 *
 * The list for this is unfortunately quite long. To avoid maintaining
 * very long switch statements just do a fast path for the few critical
 * MSRs that need TDCALL, currently only TSC_DEADLINE.
 *
 * More can be added as needed.
 *
 * The others will be handled by the #VE handler as needed.
 * See 18.1 "MSR virtualization" in the TDX Module EAS
 */
static bool tdx_fast_tdcall_path_msr(unsigned int msr)
{
	switch (msr) {
	case MSR_IA32_TSC_DEADLINE:
		return true;
	default:
		return false;

	}
}

static void notrace tdx_write_msr(unsigned int msr, u32 low, u32 high)
{
	struct tdx_module_args args = {
		.r10 = TDX_HYPERCALL_STANDARD,
		.r11 = hcall_func(EXIT_REASON_MSR_WRITE),
		.r12 = msr,
		.r13 = (u64)high << 32 | low,
	};

	if (tdx_fast_tdcall_path_msr(msr))
		__tdx_hypercall(&args);
	else
		native_write_msr(msr, low, high);
}

static int handle_cpuid(struct pt_regs *regs, struct ve_info *ve)
{
	struct tdx_module_args args = {
		.r10 = TDX_HYPERCALL_STANDARD,
		.r11 = hcall_func(EXIT_REASON_CPUID),
		.r12 = regs->ax,
		.r13 = regs->cx,
	};

	/*
	 * CPUID leaf 0x2 provides cache and TLB information.
	 *
	 * The leaf is obsolete. There are leafs that provides the same
	 * information in a structured form. See leaf 0x4 on cache info and
	 * leaf 0x18 on TLB info.
	 */
	if (regs->ax == 2) {
		/*
		 * Each byte in EAX/EBX/ECX/EDX is an informational descriptor.
		 *
		 * The least-significant byte in register EAX always returns
		 * 0x01. Software should ignore this value and not interpret
		 * it as an informational descriptor.
		 *
		 * Descriptors used here:
		 *
		 *  - 0xff: use CPUID leaf 0x4 to query cache parameters;
		 *
		 *  - 0xfe: use CPUID leaf 0x18 to query TLB and other address
		 *          translation parameters.
		 *
		 * XXX: provide prefetch information?
		 */
		regs->ax = 0xf1ff01;
		regs->bx = regs->cx = regs->dx = 0;
		return ve_instr_len(ve);
	}

	/*
	 * Only allow VMM to control range reserved for hypervisor
	 * communication.
	 *
	 * Return all-zeros for any CPUID outside the range. It matches CPU
	 * behaviour for non-supported leaf.
	 */
	if (regs->ax < 0x40000000 || regs->ax > 0x4FFFFFFF) {
		regs->ax = regs->bx = regs->cx = regs->dx = 0;
		return ve_instr_len(ve);
	}

	/*
	 * Emulate the CPUID instruction via a hypercall. More info about
	 * ABI can be found in TDX Guest-Host-Communication Interface
	 * (GHCI), section titled "VP.VMCALL<Instruction.CPUID>".
	 */
	if (__trace_tdx_hypercall(&args))
		return -EIO;

	/*
	 * As per TDX GHCI CPUID ABI, r12-r15 registers contain contents of
	 * EAX, EBX, ECX, EDX registers after the CPUID instruction execution.
	 * So copy the register contents back to pt_regs.
	 */
	regs->ax = args.r12;
	regs->bx = args.r13;
	regs->cx = args.r14;
	regs->dx = args.r15;

	return ve_instr_len(ve);
}

static bool mmio_read(int size, unsigned long addr, unsigned long *val)
{
	struct tdx_module_args args = {
		.r10 = TDX_HYPERCALL_STANDARD,
		.r11 = hcall_func(EXIT_REASON_EPT_VIOLATION),
		.r12 = size,
		.r13 = EPT_READ,
		.r14 = addr,
		.r15 = *val,
	};

	if (__trace_tdx_hypercall(&args))
		return false;

	*val = args.r11;
	return true;
}

static bool mmio_write(int size, unsigned long addr, unsigned long val)
{
	return !_tdx_hypercall(hcall_func(EXIT_REASON_EPT_VIOLATION), size,
			       EPT_WRITE, addr, val);
}

static int handle_mmio(struct pt_regs *regs, struct ve_info *ve)
{
	unsigned long *reg, val, vaddr;
	char buffer[MAX_INSN_SIZE];
	enum insn_mmio_type mmio;
	struct insn insn = {};
	int size, extend_size;
	u8 extend_val = 0;

	/* Only in-kernel MMIO is supported */
	if (WARN_ON_ONCE(user_mode(regs)))
		return -EFAULT;

	if (copy_from_kernel_nofault(buffer, (void *)regs->ip, MAX_INSN_SIZE))
		return -EFAULT;

	if (insn_decode(&insn, buffer, MAX_INSN_SIZE, INSN_MODE_64))
		return -EINVAL;

	mmio = insn_decode_mmio(&insn, &size);
	if (WARN_ON_ONCE(mmio == INSN_MMIO_DECODE_FAILED))
		return -EINVAL;

	if (mmio != INSN_MMIO_WRITE_IMM && mmio != INSN_MMIO_MOVS) {
		reg = insn_get_modrm_reg_ptr(&insn, regs);
		if (!reg)
			return -EINVAL;
	}

	/*
	 * Reject EPT violation #VEs that split pages.
	 *
	 * MMIO accesses are supposed to be naturally aligned and therefore
	 * never cross page boundaries. Seeing split page accesses indicates
	 * a bug or a load_unaligned_zeropad() that stepped into an MMIO page.
	 *
	 * load_unaligned_zeropad() will recover using exception fixups.
	 */
	vaddr = (unsigned long)insn_get_addr_ref(&insn, regs);
	if (vaddr / PAGE_SIZE != (vaddr + size - 1) / PAGE_SIZE)
		return -EFAULT;

	/* Handle writes first */
	switch (mmio) {
	case INSN_MMIO_WRITE:
		memcpy(&val, reg, size);
		if (!mmio_write(size, ve->gpa, val))
			return -EIO;
		return insn.length;
	case INSN_MMIO_WRITE_IMM:
		val = insn.immediate.value;
		if (!mmio_write(size, ve->gpa, val))
			return -EIO;
		return insn.length;
	case INSN_MMIO_READ:
	case INSN_MMIO_READ_ZERO_EXTEND:
	case INSN_MMIO_READ_SIGN_EXTEND:
		/* Reads are handled below */
		break;
	case INSN_MMIO_MOVS:
	case INSN_MMIO_DECODE_FAILED:
		/*
		 * MMIO was accessed with an instruction that could not be
		 * decoded or handled properly. It was likely not using io.h
		 * helpers or accessed MMIO accidentally.
		 */
		return -EINVAL;
	default:
		WARN_ONCE(1, "Unknown insn_decode_mmio() decode value?");
		return -EINVAL;
	}

	/* Handle reads */
	if (!mmio_read(size, ve->gpa, &val))
		return -EIO;

	switch (mmio) {
	case INSN_MMIO_READ:
		/* Zero-extend for 32-bit operation */
		extend_size = size == 4 ? sizeof(*reg) : 0;
		break;
	case INSN_MMIO_READ_ZERO_EXTEND:
		/* Zero extend based on operand size */
		extend_size = insn.opnd_bytes;
		break;
	case INSN_MMIO_READ_SIGN_EXTEND:
		/* Sign extend based on operand size */
		extend_size = insn.opnd_bytes;
		if (size == 1 && val & BIT(7))
			extend_val = 0xFF;
		else if (size > 1 && val & BIT(15))
			extend_val = 0xFF;
		break;
	default:
		/* All other cases has to be covered with the first switch() */
		WARN_ON_ONCE(1);
		return -EINVAL;
	}

	if (extend_size)
		memset(reg, extend_val, extend_size);
	memcpy(reg, &val, size);
	return insn.length;
}

static unsigned long tdx_virt_mmio(int size, bool write, unsigned long vaddr,
	unsigned long* val)
{
	pte_t* pte;
	int level;

	pte = lookup_address(vaddr, &level);
	if (!pte)
		return -EIO;

	return write ? 
		mmio_write(size,
			(pte_pfn(*pte) << PAGE_SHIFT) +
			(vaddr & ~page_level_mask(level)),
			*val) :
		mmio_read(size,
			(pte_pfn(*pte) << PAGE_SHIFT) +
			(vaddr & ~page_level_mask(level)),
			val);
}

static unsigned char tdx_mmio_readb(void __iomem* addr)
{
	unsigned long val;

	if (tdx_virt_mmio(1, false, (unsigned long)addr, &val))
		return 0xff;
	return val;
}

static unsigned short tdx_mmio_readw(void __iomem* addr)
{
	unsigned long val;

	if (tdx_virt_mmio(2, false, (unsigned long)addr, &val))
		return 0xffff;
	return val;
}

static unsigned int tdx_mmio_readl(void __iomem* addr)
{
	unsigned long val;

	if (tdx_virt_mmio(4, false, (unsigned long)addr, &val))
		return 0xffffffff;
	return val;
}

static unsigned long tdx_mmio_readq(void __iomem* addr)
{
	unsigned long val;

	if (tdx_virt_mmio(8, false, (unsigned long)addr, &val))
		return 0xffffffffffffffff;
	return val;
}

static void tdx_mmio_writeb(unsigned char v, void __iomem* addr)
{
	unsigned long val = v;

	tdx_virt_mmio(1, true, (unsigned long)addr, &val);
}

static void tdx_mmio_writew(unsigned short v, void __iomem* addr)
{
	unsigned long val = v;

	tdx_virt_mmio(2, true, (unsigned long)addr, &val);
}

static void tdx_mmio_writel(unsigned int v, void __iomem* addr)
{
	unsigned long val = v;

	tdx_virt_mmio(4, true, (unsigned long)addr, &val);
}

static void tdx_mmio_writeq(unsigned long v, void __iomem* addr)
{
	unsigned long val = v;

	tdx_virt_mmio(8, true, (unsigned long)addr, &val);
}

static const struct iomap_mmio tdx_iomap_mmio = {
	.ireadb = tdx_mmio_readb,
	.ireadw = tdx_mmio_readw,
	.ireadl = tdx_mmio_readl,
	.ireadq = tdx_mmio_readq,
	.iwriteb = tdx_mmio_writeb,
	.iwritew = tdx_mmio_writew,
	.iwritel = tdx_mmio_writel,
	.iwriteq = tdx_mmio_writeq,
};

static bool handle_in(struct pt_regs *regs, int size, int port)
{
	struct tdx_module_args args = {
		.r10 = TDX_HYPERCALL_STANDARD,
		.r11 = hcall_func(EXIT_REASON_IO_INSTRUCTION),
		.r12 = size,
		.r13 = PORT_READ,
		.r14 = port,
	};
	u64 mask = GENMASK(BITS_PER_BYTE * size, 0);
	bool success;

	if (!tdx_allowed_port(port)) {
		regs->ax &= ~mask;
		regs->ax |= (UINT_MAX & mask);
		return true;
	}

	/*
	 * Emulate the I/O read via hypercall. More info about ABI can be found
	 * in TDX Guest-Host-Communication Interface (GHCI) section titled
	 * "TDG.VP.VMCALL<Instruction.IO>".
	 */
	success = !__trace_tdx_hypercall(&args);

	/* Update part of the register affected by the emulated instruction */
	regs->ax &= ~mask;
	if (success)
		regs->ax |= args.r11 & mask;

	return success;
}

static bool handle_out(struct pt_regs *regs, int size, int port)
{
	u64 mask = GENMASK(BITS_PER_BYTE * size, 0);

	if (!tdx_allowed_port(port))
		return true;

	/*
	 * Emulate the I/O write via hypercall. More info about ABI can be found
	 * in TDX Guest-Host-Communication Interface (GHCI) section titled
	 * "TDG.VP.VMCALL<Instruction.IO>".
	 */
	return !_tdx_hypercall(hcall_func(EXIT_REASON_IO_INSTRUCTION), size,
			       PORT_WRITE, port, regs->ax & mask);
}

/*
 * Emulate I/O using hypercall.
 *
 * Assumes the IO instruction was using ax, which is enforced
 * by the standard io.h macros.
 *
 * Return True on success or False on failure.
 */
static int handle_io(struct pt_regs *regs, struct ve_info *ve)
{
	u32 exit_qual = ve->exit_qual;
	int size, port;
	bool in, ret;

	if (VE_IS_IO_STRING(exit_qual))
		return -EIO;

	in   = VE_IS_IO_IN(exit_qual);
	size = VE_GET_IO_SIZE(exit_qual);
	port = VE_GET_PORT_NUM(exit_qual);

	if (in)
		ret = handle_in(regs, size, port);
	else
		ret = handle_out(regs, size, port);
	if (!ret)
		return -EIO;

	return ve_instr_len(ve);
}

/*
 * Early #VE exception handler. Only handles a subset of port I/O.
 * Intended only for earlyprintk. If failed, return false.
 */
__init bool tdx_early_handle_ve(struct pt_regs *regs)
{
	struct ve_info ve;
	int insn_len;

	tdx_get_ve_info(&ve);

	if (ve.exit_reason != EXIT_REASON_IO_INSTRUCTION)
		return false;

	insn_len = handle_io(regs, &ve);
	if (insn_len < 0)
		return false;

	regs->ip += insn_len;
	return true;
}

void tdx_get_ve_info(struct ve_info *ve)
{
	struct tdx_module_args args = {};

	/*
	 * Called during #VE handling to retrieve the #VE info from the
	 * TDX module.
	 *
	 * This has to be called early in #VE handling.  A "nested" #VE which
	 * occurs before this will raise a #DF and is not recoverable.
	 *
	 * The call retrieves the #VE info from the TDX module, which also
	 * clears the "#VE valid" flag. This must be done before anything else
	 * because any #VE that occurs while the valid flag is set will lead to
	 * #DF.
	 *
	 * Note, the TDX module treats virtual NMIs as inhibited if the #VE
	 * valid flag is set. It means that NMI=>#VE will not result in a #DF.
	 */
	tdcall_ret_with_trace(TDG_VP_VEINFO_GET, &args);

	/* Transfer the output parameters */
	ve->exit_reason = args.rcx;
	ve->exit_qual   = args.rdx;
	ve->gla         = args.r8;
	ve->gpa         = args.r9;
	ve->instr_len   = lower_32_bits(args.r10);
	ve->instr_info  = upper_32_bits(args.r10);
}

/*
 * Handle the user initiated #VE.
 *
 * On success, returns the number of bytes RIP should be incremented (>=0)
 * or -errno on error.
 */
static int virt_exception_user(struct pt_regs *regs, struct ve_info *ve)
{
	switch (ve->exit_reason) {
	case EXIT_REASON_CPUID:
		return handle_cpuid(regs, ve);
	default:
		pr_warn("Unexpected #VE: %lld\n", ve->exit_reason);
		return -EIO;
	}
}

static inline bool is_private_gpa(u64 gpa)
{
	return gpa == cc_mkenc(gpa);
}

/*
 * Handle the kernel #VE.
 *
 * On success, returns the number of bytes RIP should be incremented (>=0)
 * or -errno on error.
 */
static int virt_exception_kernel(struct pt_regs *regs, struct ve_info *ve)
{

	trace_tdx_virtualization_exception_rcuidle(regs->ip, ve->exit_reason,
			ve->exit_qual, ve->gpa, ve->instr_len, ve->instr_info,
			regs->cx, regs->ax, regs->dx);

	switch (ve->exit_reason) {
	case EXIT_REASON_HLT:
		return handle_halt(ve);
	case EXIT_REASON_MSR_READ:
		return read_msr(regs, ve);
	case EXIT_REASON_MSR_WRITE:
		return write_msr(regs, ve);
	case EXIT_REASON_CPUID:
		return handle_cpuid(regs, ve);
	case EXIT_REASON_EPT_VIOLATION:
		if (is_private_gpa(ve->gpa))
			panic("Unexpected EPT-violation on private memory.");
		return handle_mmio(regs, ve);
	case EXIT_REASON_IO_INSTRUCTION:
		return handle_io(regs, ve);
	default:
		pr_warn("Unexpected #VE: %lld\n", ve->exit_reason);
		return -EIO;
	}
}

bool tdx_handle_virt_exception(struct pt_regs *regs, struct ve_info *ve)
{
	int insn_len;

	if (user_mode(regs))
		insn_len = virt_exception_user(regs, ve);
	else
		insn_len = virt_exception_kernel(regs, ve);
	if (insn_len < 0)
		return false;

	/* After successful #VE handling, move the IP */
	regs->ip += insn_len;

	/*
	 * Single-stepping through an emulated instruction is
	 * two-fold: handling the #VE and raising a #DB. The
	 * former is taken care of above; this tells the #VE
	 * trap handler to do the latter. #DB is raised after
	 * the instruction has been executed; the IP also needs
	 * to be advanced in this case.
	 */
	if (regs->flags & X86_EFLAGS_TF)
		return false;

	return true;
}

static bool tdx_tlb_flush_required(bool private)
{
	/*
	 * TDX guest is responsible for flushing TLB on private->shared
	 * transition. VMM is responsible for flushing on shared->private.
	 *
	 * The VMM _can't_ flush private addresses as it can't generate PAs
	 * with the guest's HKID.  Shared memory isn't subject to integrity
	 * checking, i.e. the VMM doesn't need to flush for its own protection.
	 *
	 * There's no need to flush when converting from shared to private,
	 * as flushing is the VMM's responsibility in this case, e.g. it must
	 * flush to avoid integrity failures in the face of a buggy or
	 * malicious guest.
	 */
	return !private;
}

static bool tdx_cache_flush_required(void)
{
	/*
	 * AMD SME/SEV can avoid cache flushing if HW enforces cache coherence.
	 * TDX doesn't have such capability.
	 *
	 * Flush cache unconditionally.
	 */
	return true;
}

/*
 * Inform the VMM of the guest's intent for this physical page: shared with
 * the VMM or private to the guest.  The VMM is expected to change its mapping
 * of the page in response.
 */
static bool tdx_enc_status_changed(unsigned long vaddr, int numpages, bool enc)
{
	phys_addr_t start = __pa(vaddr);
	phys_addr_t end   = __pa(vaddr + numpages * PAGE_SIZE);

	if (!enc) {
		/* Set the shared (decrypted) bits: */
		start |= cc_mkdec(0);
		end   |= cc_mkdec(0);
	}

	/*
	 * Notify the VMM about page mapping conversion. More info about ABI
	 * can be found in TDX Guest-Host-Communication Interface (GHCI),
	 * section "TDG.VP.VMCALL<MapGPA>"
	 */
	if (_tdx_hypercall(TDVMCALL_MAP_GPA, start, end - start, 0, 0))
		return false;

	/* shared->private conversion requires memory to be accepted before use */
	if (enc)
		return tdx_accept_memory(start, end);

	return true;
}

static bool tdx_enc_status_change_prepare(unsigned long vaddr, int numpages,
					  bool enc)
{
	/*
	 * Only handle shared->private conversion here.
	 * See the comment in tdx_early_init().
	 */
	if (enc)
		return tdx_enc_status_changed(vaddr, numpages, enc);
	return true;
}

static bool tdx_enc_status_change_finish(unsigned long vaddr, int numpages,
					 bool enc)
{
	/*
	 * Only handle private->shared conversion here.
	 * See the comment in tdx_early_init().
	 */
	if (!enc)
		return tdx_enc_status_changed(vaddr, numpages, enc);
	return true;
}

void __init tdx_early_init(void)
{
	struct tdx_module_args args = {
		.rdx = TDCS_NOTIFY_ENABLES,
		.r9 = -1ULL,
	};
	u64 cc_mask;
	u32 eax, sig[3];

	cpuid_count(TDX_CPUID_LEAF_ID, 0, &eax, &sig[0], &sig[2],  &sig[1]);

	if (memcmp(TDX_IDENT, sig, sizeof(sig)))
		return;

	setup_force_cpu_cap(X86_FEATURE_TDX_GUEST);
	setup_clear_cpu_cap(X86_FEATURE_MCE);
	setup_clear_cpu_cap(X86_FEATURE_MTRR);
	setup_clear_cpu_cap(X86_FEATURE_TME);

	cc_vendor = CC_VENDOR_INTEL;

	/*
	 * The only secure (monotonous) timer inside a TD guest
	 * is the TSC. The TDX module does various checks on the TSC.
	 * There are no other reliable fall back options. Also checking
	 * against jiffies is very unreliable. So force the TSC reliable.
	 */
	setup_force_cpu_cap(X86_FEATURE_TSC_RELIABLE);

	/*
	 * In TDX relying on environmental noise like interrupt
	 * timing alone is dubious, because it can be directly
	 * controlled by a untrusted hypervisor. Make sure to
	 * mix in the CPU hardware random number generator too.
	 */
	random_enable_trust_cpu();

	iomap_mmio = &tdx_iomap_mmio;

	/*
	 * Make sure there is a panic if something goes wrong,
	 * just in case it's some kind of host attack.
	 */
	panic_on_oops = 1;

	/* Set restricted memory access for virtio. */
	virtio_set_mem_acc_cb(virtio_require_restricted_mem_acc);

	pv_ops.cpu.write_msr = tdx_write_msr;

	tdx_parse_tdinfo(&cc_mask);
	cc_set_mask(cc_mask);

	/* Kernel does not use NOTIFY_ENABLES and does not need random #VEs */
	tdcall_ret_with_trace(TDG_VM_WR, &args);

	/*
	 * All bits above GPA width are reserved and kernel treats shared bit
	 * as flag, not as part of physical address.
	 *
	 * Adjust physical mask to only cover valid GPA bits.
	 */
	physical_mask &= cc_mask - 1;

	/*
	 * The kernel mapping should match the TDX metadata for the page.
	 * load_unaligned_zeropad() can touch memory *adjacent* to that which is
	 * owned by the caller and can catch even _momentary_ mismatches.  Bad
	 * things happen on mismatch:
	 *
	 *   - Private mapping => Shared Page  == Guest shutdown
         *   - Shared mapping  => Private Page == Recoverable #VE
	 *
	 * guest.enc_status_change_prepare() converts the page from
	 * shared=>private before the mapping becomes private.
	 *
	 * guest.enc_status_change_finish() converts the page from
	 * private=>shared after the mapping becomes private.
	 *
	 * In both cases there is a temporary shared mapping to a private page,
	 * which can result in a #VE.  But, there is never a private mapping to
	 * a shared page.
	 */
	x86_platform.guest.enc_status_change_prepare = tdx_enc_status_change_prepare;
	x86_platform.guest.enc_status_change_finish  = tdx_enc_status_change_finish;

	x86_platform.guest.enc_cache_flush_required  = tdx_cache_flush_required;
	x86_platform.guest.enc_tlb_flush_required    = tdx_tlb_flush_required;

	/*
	 * TDX intercepts the RDMSR to read the X2APIC ID in the parallel
	 * bringup low level code. That raises #VE which cannot be handled
	 * there.
	 *
	 * Intel-TDX has a secure RDMSR hypercall, but that needs to be
	 * implemented seperately in the low level startup ASM code.
	 * Until that is in place, disable parallel bringup for TDX.
	 */
	x86_cpuinit.parallel_bringup = false;

	legacy_pic = &null_legacy_pic;

	pci_disable_early();
	pci_disable_mmconf();

	pr_info("Guest detected\n");
}

/* Reserve an IRQ from x86_vector_domain for TD event notification */
static int __init tdx_arch_init(void)
{
	struct irq_alloc_info info;
	cpumask_t saved_cpus;
	struct irq_cfg *cfg;
	int cpu;

	if (!cpu_feature_enabled(X86_FEATURE_TDX_GUEST))
		return 0;

	init_irq_alloc_info(&info, NULL);

	/*
	 * Event notification vector will be delivered to the CPU
	 * in which TDVMCALL_SETUP_NOTIFY_INTR hypercall is requested.
	 * So set the IRQ affinity to the current CPU.
	 */
	cpu = get_cpu();

	saved_cpus = *current->cpus_ptr;

	info.mask = cpumask_of(cpu);

	put_cpu();

	set_cpus_allowed_ptr(current, cpumask_of(cpu));

	tdx_notify_irq = irq_domain_alloc_irqs(x86_vector_domain, 1,
				NUMA_NO_NODE, &info);

	if (tdx_notify_irq < 0) {
		pr_err("Event notification IRQ allocation failed %d\n",
				tdx_notify_irq);
		goto init_failed;
	}

	irq_set_handler(tdx_notify_irq, handle_edge_irq);

	cfg = irq_cfg(tdx_notify_irq);
	if (!cfg) {
		pr_err("Event notification IRQ config not found\n");
		goto init_failed;
	}

	/*
	 * Register callback vector address with VMM. More details
	 * about the ABI can be found in TDX Guest-Host-Communication
	 * Interface (GHCI), sec titled
	 * "TDG.VP.VMCALL<SetupEventNotifyInterrupt>".
	 */
	if (_tdx_hypercall(TDVMCALL_SETUP_NOTIFY_INTR, cfg->vector, 0, 0, 0)) {
		pr_err("Setting event notification interrupt failed\n");
		goto init_failed;
	}

init_failed:
	set_cpus_allowed_ptr(current, &saved_cpus);
	return 0;
}
arch_initcall(tdx_arch_init);

/**
 * tdx_alloc_event_irq() - Allocate an IRQ for event notification from
 * 			   the VMM to the TDX Guest.
 *
 * Return IRQ on success or errno on failure.
 *
 */
int tdx_alloc_event_irq(void)
{
	struct irq_alloc_info info;
	int irq;

	if (!cpu_feature_enabled(X86_FEATURE_TDX_GUEST))
		return -ENODEV;

	init_irq_alloc_info(&info, NULL);

	irq = irq_domain_alloc_irqs(x86_vector_domain, 1, NUMA_NO_NODE, &info);
	if (irq <= 0) {
		pr_err("Event notification IRQ allocation failed %d\n", irq);
		return -EIO;
	}

	irq_set_handler(irq, handle_edge_irq);

	return irq;
}
EXPORT_SYMBOL_GPL(tdx_alloc_event_irq);

/**
 * tdx_free_event_irq() - Free the event IRQ.
 *
 */
void tdx_free_event_irq(int irq)
{
	irq_domain_free_irqs(irq, 1);
}
EXPORT_SYMBOL_GPL(tdx_free_event_irq);

static struct platform_device tpm_device = {
	.name = "tpm",
	.id = -1,
};

static int __init tdx_device_init(void)
{
	if (!cpu_feature_enabled(X86_FEATURE_TDX_GUEST))
		return 0;

	if (platform_device_register(&tpm_device))
		pr_warn("TPM device register failed\n");

	return 0;
}
device_initcall(tdx_device_init)
