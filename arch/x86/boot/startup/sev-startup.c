// SPDX-License-Identifier: GPL-2.0-only
/*
 * AMD Memory Encryption Support
 *
 * Copyright (C) 2019 SUSE
 *
 * Author: Joerg Roedel <jroedel@suse.de>
 */

#define pr_fmt(fmt)	"SEV: " fmt

#include <linux/sched/debug.h>	/* For show_regs() */
#include <linux/percpu-defs.h>
#include <linux/cc_platform.h>
#include <linux/printk.h>
#include <linux/mm_types.h>
#include <linux/set_memory.h>
#include <linux/memblock.h>
#include <linux/kernel.h>
#include <linux/mm.h>
#include <linux/cpumask.h>
#include <linux/efi.h>
#include <linux/io.h>
#include <linux/psp-sev.h>
#include <uapi/linux/sev-guest.h>

#include <asm/init.h>
#include <asm/cpu_entry_area.h>
#include <asm/stacktrace.h>
#include <asm/sev.h>
#include <asm/sev-internal.h>
#include <asm/insn-eval.h>
#include <asm/fpu/xcr.h>
#include <asm/processor.h>
#include <asm/realmode.h>
#include <asm/setup.h>
#include <asm/traps.h>
#include <asm/svm.h>
#include <asm/smp.h>
#include <asm/cpu.h>
#include <asm/apic.h>
#include <asm/cpuid.h>
#include <asm/cmdline.h>

/* For early boot hypervisor communication in SEV-ES enabled guests */
struct ghcb boot_ghcb_page __bss_decrypted __aligned(PAGE_SIZE);

/*
 * Needs to be in the .data section because we need it NULL before bss is
 * cleared
 */
struct ghcb *boot_ghcb __section(".data");

/* Bitmap of SEV features supported by the hypervisor */
u64 sev_hv_features __ro_after_init;

/* Secrets page physical address from the CC blob */
static u64 secrets_pa __ro_after_init;

/* For early boot SVSM communication */
struct svsm_ca boot_svsm_ca_page __aligned(PAGE_SIZE);

DEFINE_PER_CPU(struct svsm_ca *, svsm_caa);
DEFINE_PER_CPU(u64, svsm_caa_pa);

/*
 * Nothing shall interrupt this code path while holding the per-CPU
 * GHCB. The backup GHCB is only for NMIs interrupting this path.
 *
 * Callers must disable local interrupts around it.
 */
noinstr struct ghcb *__sev_get_ghcb(struct ghcb_state *state)
{
	struct sev_es_runtime_data *data;
	struct ghcb *ghcb;

	WARN_ON(!irqs_disabled());

	data = this_cpu_read(runtime_data);
	ghcb = &data->ghcb_page;

	if (unlikely(data->ghcb_active)) {
		/* GHCB is already in use - save its contents */

		if (unlikely(data->backup_ghcb_active)) {
			/*
			 * Backup-GHCB is also already in use. There is no way
			 * to continue here so just kill the machine. To make
			 * panic() work, mark GHCBs inactive so that messages
			 * can be printed out.
			 */
			data->ghcb_active        = false;
			data->backup_ghcb_active = false;

			instrumentation_begin();
			panic("Unable to handle #VC exception! GHCB and Backup GHCB are already in use");
			instrumentation_end();
		}

		/* Mark backup_ghcb active before writing to it */
		data->backup_ghcb_active = true;

		state->ghcb = &data->backup_ghcb;

		/* Backup GHCB content */
		*state->ghcb = *ghcb;
	} else {
		state->ghcb = NULL;
		data->ghcb_active = true;
	}

	return ghcb;
}

static int vc_fetch_insn_kernel(struct es_em_ctxt *ctxt,
				unsigned char *buffer)
{
	return copy_from_kernel_nofault(buffer, (unsigned char *)ctxt->regs->ip, MAX_INSN_SIZE);
}

static enum es_result __vc_decode_user_insn(struct es_em_ctxt *ctxt)
{
	char buffer[MAX_INSN_SIZE];
	int insn_bytes;

	insn_bytes = insn_fetch_from_user_inatomic(ctxt->regs, buffer);
	if (insn_bytes == 0) {
		/* Nothing could be copied */
		ctxt->fi.vector     = X86_TRAP_PF;
		ctxt->fi.error_code = X86_PF_INSTR | X86_PF_USER;
		ctxt->fi.cr2        = ctxt->regs->ip;
		return ES_EXCEPTION;
	} else if (insn_bytes == -EINVAL) {
		/* Effective RIP could not be calculated */
		ctxt->fi.vector     = X86_TRAP_GP;
		ctxt->fi.error_code = 0;
		ctxt->fi.cr2        = 0;
		return ES_EXCEPTION;
	}

	if (!insn_decode_from_regs(&ctxt->insn, ctxt->regs, buffer, insn_bytes))
		return ES_DECODE_FAILED;

	if (ctxt->insn.immediate.got)
		return ES_OK;
	else
		return ES_DECODE_FAILED;
}

static enum es_result __vc_decode_kern_insn(struct es_em_ctxt *ctxt)
{
	char buffer[MAX_INSN_SIZE];
	int res, ret;

	res = vc_fetch_insn_kernel(ctxt, buffer);
	if (res) {
		ctxt->fi.vector     = X86_TRAP_PF;
		ctxt->fi.error_code = X86_PF_INSTR;
		ctxt->fi.cr2        = ctxt->regs->ip;
		return ES_EXCEPTION;
	}

	ret = insn_decode(&ctxt->insn, buffer, MAX_INSN_SIZE, INSN_MODE_64);
	if (ret < 0)
		return ES_DECODE_FAILED;
	else
		return ES_OK;
}

static enum es_result vc_decode_insn(struct es_em_ctxt *ctxt)
{
	if (user_mode(ctxt->regs))
		return __vc_decode_user_insn(ctxt);
	else
		return __vc_decode_kern_insn(ctxt);
}

static enum es_result vc_write_mem(struct es_em_ctxt *ctxt,
				   char *dst, char *buf, size_t size)
{
	unsigned long error_code = X86_PF_PROT | X86_PF_WRITE;

	/*
	 * This function uses __put_user() independent of whether kernel or user
	 * memory is accessed. This works fine because __put_user() does no
	 * sanity checks of the pointer being accessed. All that it does is
	 * to report when the access failed.
	 *
	 * Also, this function runs in atomic context, so __put_user() is not
	 * allowed to sleep. The page-fault handler detects that it is running
	 * in atomic context and will not try to take mmap_sem and handle the
	 * fault, so additional pagefault_enable()/disable() calls are not
	 * needed.
	 *
	 * The access can't be done via copy_to_user() here because
	 * vc_write_mem() must not use string instructions to access unsafe
	 * memory. The reason is that MOVS is emulated by the #VC handler by
	 * splitting the move up into a read and a write and taking a nested #VC
	 * exception on whatever of them is the MMIO access. Using string
	 * instructions here would cause infinite nesting.
	 */
	switch (size) {
	case 1: {
		u8 d1;
		u8 __user *target = (u8 __user *)dst;

		memcpy(&d1, buf, 1);
		if (__put_user(d1, target))
			goto fault;
		break;
	}
	case 2: {
		u16 d2;
		u16 __user *target = (u16 __user *)dst;

		memcpy(&d2, buf, 2);
		if (__put_user(d2, target))
			goto fault;
		break;
	}
	case 4: {
		u32 d4;
		u32 __user *target = (u32 __user *)dst;

		memcpy(&d4, buf, 4);
		if (__put_user(d4, target))
			goto fault;
		break;
	}
	case 8: {
		u64 d8;
		u64 __user *target = (u64 __user *)dst;

		memcpy(&d8, buf, 8);
		if (__put_user(d8, target))
			goto fault;
		break;
	}
	default:
		WARN_ONCE(1, "%s: Invalid size: %zu\n", __func__, size);
		return ES_UNSUPPORTED;
	}

	return ES_OK;

fault:
	if (user_mode(ctxt->regs))
		error_code |= X86_PF_USER;

	ctxt->fi.vector = X86_TRAP_PF;
	ctxt->fi.error_code = error_code;
	ctxt->fi.cr2 = (unsigned long)dst;

	return ES_EXCEPTION;
}

static enum es_result vc_read_mem(struct es_em_ctxt *ctxt,
				  char *src, char *buf, size_t size)
{
	unsigned long error_code = X86_PF_PROT;

	/*
	 * This function uses __get_user() independent of whether kernel or user
	 * memory is accessed. This works fine because __get_user() does no
	 * sanity checks of the pointer being accessed. All that it does is
	 * to report when the access failed.
	 *
	 * Also, this function runs in atomic context, so __get_user() is not
	 * allowed to sleep. The page-fault handler detects that it is running
	 * in atomic context and will not try to take mmap_sem and handle the
	 * fault, so additional pagefault_enable()/disable() calls are not
	 * needed.
	 *
	 * The access can't be done via copy_from_user() here because
	 * vc_read_mem() must not use string instructions to access unsafe
	 * memory. The reason is that MOVS is emulated by the #VC handler by
	 * splitting the move up into a read and a write and taking a nested #VC
	 * exception on whatever of them is the MMIO access. Using string
	 * instructions here would cause infinite nesting.
	 */
	switch (size) {
	case 1: {
		u8 d1;
		u8 __user *s = (u8 __user *)src;

		if (__get_user(d1, s))
			goto fault;
		memcpy(buf, &d1, 1);
		break;
	}
	case 2: {
		u16 d2;
		u16 __user *s = (u16 __user *)src;

		if (__get_user(d2, s))
			goto fault;
		memcpy(buf, &d2, 2);
		break;
	}
	case 4: {
		u32 d4;
		u32 __user *s = (u32 __user *)src;

		if (__get_user(d4, s))
			goto fault;
		memcpy(buf, &d4, 4);
		break;
	}
	case 8: {
		u64 d8;
		u64 __user *s = (u64 __user *)src;
		if (__get_user(d8, s))
			goto fault;
		memcpy(buf, &d8, 8);
		break;
	}
	default:
		WARN_ONCE(1, "%s: Invalid size: %zu\n", __func__, size);
		return ES_UNSUPPORTED;
	}

	return ES_OK;

fault:
	if (user_mode(ctxt->regs))
		error_code |= X86_PF_USER;

	ctxt->fi.vector = X86_TRAP_PF;
	ctxt->fi.error_code = error_code;
	ctxt->fi.cr2 = (unsigned long)src;

	return ES_EXCEPTION;
}

static enum es_result vc_slow_virt_to_phys(struct ghcb *ghcb, struct es_em_ctxt *ctxt,
					   unsigned long vaddr, phys_addr_t *paddr)
{
	unsigned long va = (unsigned long)vaddr;
	unsigned int level;
	phys_addr_t pa;
	pgd_t *pgd;
	pte_t *pte;

	pgd = __va(read_cr3_pa());
	pgd = &pgd[pgd_index(va)];
	pte = lookup_address_in_pgd(pgd, va, &level);
	if (!pte) {
		ctxt->fi.vector     = X86_TRAP_PF;
		ctxt->fi.cr2        = vaddr;
		ctxt->fi.error_code = 0;

		if (user_mode(ctxt->regs))
			ctxt->fi.error_code |= X86_PF_USER;

		return ES_EXCEPTION;
	}

	if (WARN_ON_ONCE(pte_val(*pte) & _PAGE_ENC))
		/* Emulated MMIO to/from encrypted memory not supported */
		return ES_UNSUPPORTED;

	pa = (phys_addr_t)pte_pfn(*pte) << PAGE_SHIFT;
	pa |= va & ~page_level_mask(level);

	*paddr = pa;

	return ES_OK;
}

static enum es_result vc_ioio_check(struct es_em_ctxt *ctxt, u16 port, size_t size)
{
	BUG_ON(size > 4);

	if (user_mode(ctxt->regs)) {
		struct thread_struct *t = &current->thread;
		struct io_bitmap *iobm = t->io_bitmap;
		size_t idx;

		if (!iobm)
			goto fault;

		for (idx = port; idx < port + size; ++idx) {
			if (test_bit(idx, iobm->bitmap))
				goto fault;
		}
	}

	return ES_OK;

fault:
	ctxt->fi.vector = X86_TRAP_GP;
	ctxt->fi.error_code = 0;

	return ES_EXCEPTION;
}

static __always_inline void vc_forward_exception(struct es_em_ctxt *ctxt)
{
	long error_code = ctxt->fi.error_code;
	int trapnr = ctxt->fi.vector;

	ctxt->regs->orig_ax = ctxt->fi.error_code;

	switch (trapnr) {
	case X86_TRAP_GP:
		exc_general_protection(ctxt->regs, error_code);
		break;
	case X86_TRAP_UD:
		exc_invalid_op(ctxt->regs);
		break;
	case X86_TRAP_PF:
		write_cr2(ctxt->fi.cr2);
		exc_page_fault(ctxt->regs, error_code);
		break;
	case X86_TRAP_AC:
		exc_alignment_check(ctxt->regs, error_code);
		break;
	default:
		pr_emerg("Unsupported exception in #VC instruction emulation - can't continue\n");
		BUG();
	}
}

/* Include code shared with pre-decompression boot stage */
#include "sev-shared.c"

noinstr void __sev_put_ghcb(struct ghcb_state *state)
{
	struct sev_es_runtime_data *data;
	struct ghcb *ghcb;

	WARN_ON(!irqs_disabled());

	data = this_cpu_read(runtime_data);
	ghcb = &data->ghcb_page;

	if (state->ghcb) {
		/* Restore GHCB from Backup */
		*ghcb = *state->ghcb;
		data->backup_ghcb_active = false;
		state->ghcb = NULL;
	} else {
		/*
		 * Invalidate the GHCB so a VMGEXIT instruction issued
		 * from userspace won't appear to be valid.
		 */
		vc_ghcb_invalidate(ghcb);
		data->ghcb_active = false;
	}
}

int svsm_perform_call_protocol(struct svsm_call *call)
{
	struct ghcb_state state;
	unsigned long flags;
	struct ghcb *ghcb;
	int ret;

	/*
	 * This can be called very early in the boot, use native functions in
	 * order to avoid paravirt issues.
	 */
	flags = native_local_irq_save();

	/*
	 * Use rip-relative references when called early in the boot. If
	 * ghcbs_initialized is set, then it is late in the boot and no need
	 * to worry about rip-relative references in called functions.
	 */
	if (RIP_REL_REF(sev_cfg).ghcbs_initialized)
		ghcb = __sev_get_ghcb(&state);
	else if (RIP_REL_REF(boot_ghcb))
		ghcb = RIP_REL_REF(boot_ghcb);
	else
		ghcb = NULL;

	do {
		ret = ghcb ? svsm_perform_ghcb_protocol(ghcb, call)
			   : svsm_perform_msr_protocol(call);
	} while (ret == -EAGAIN);

	if (RIP_REL_REF(sev_cfg).ghcbs_initialized)
		__sev_put_ghcb(&state);

	native_local_irq_restore(flags);

	return ret;
}

void __head
early_set_pages_state(unsigned long vaddr, unsigned long paddr,
		      unsigned long npages, enum psc_op op)
{
	unsigned long paddr_end;
	u64 val;

	vaddr = vaddr & PAGE_MASK;

	paddr = paddr & PAGE_MASK;
	paddr_end = paddr + (npages << PAGE_SHIFT);

	while (paddr < paddr_end) {
		/* Page validation must be rescinded before changing to shared */
		if (op == SNP_PAGE_STATE_SHARED)
			pvalidate_4k_page(vaddr, paddr, false);

		/*
		 * Use the MSR protocol because this function can be called before
		 * the GHCB is established.
		 */
		sev_es_wr_ghcb_msr(GHCB_MSR_PSC_REQ_GFN(paddr >> PAGE_SHIFT, op));
		VMGEXIT();

		val = sev_es_rd_ghcb_msr();

		if (GHCB_RESP_CODE(val) != GHCB_MSR_PSC_RESP)
			goto e_term;

		if (GHCB_MSR_PSC_RESP_VAL(val))
			goto e_term;

		/* Page validation must be performed after changing to private */
		if (op == SNP_PAGE_STATE_PRIVATE)
			pvalidate_4k_page(vaddr, paddr, true);

		vaddr += PAGE_SIZE;
		paddr += PAGE_SIZE;
	}

	return;

e_term:
	sev_es_terminate(SEV_TERM_SET_LINUX, GHCB_TERM_PSC);
}

void __head early_snp_set_memory_private(unsigned long vaddr, unsigned long paddr,
					 unsigned long npages)
{
	/*
	 * This can be invoked in early boot while running identity mapped, so
	 * use an open coded check for SNP instead of using cc_platform_has().
	 * This eliminates worries about jump tables or checking boot_cpu_data
	 * in the cc_platform_has() function.
	 */
	if (!(RIP_REL_REF(sev_status) & MSR_AMD64_SEV_SNP_ENABLED))
		return;

	 /*
	  * Ask the hypervisor to mark the memory pages as private in the RMP
	  * table.
	  */
	early_set_pages_state(vaddr, paddr, npages, SNP_PAGE_STATE_PRIVATE);
}

void __head early_snp_set_memory_shared(unsigned long vaddr, unsigned long paddr,
					unsigned long npages)
{
	/*
	 * This can be invoked in early boot while running identity mapped, so
	 * use an open coded check for SNP instead of using cc_platform_has().
	 * This eliminates worries about jump tables or checking boot_cpu_data
	 * in the cc_platform_has() function.
	 */
	if (!(RIP_REL_REF(sev_status) & MSR_AMD64_SEV_SNP_ENABLED))
		return;

	 /* Ask hypervisor to mark the memory pages shared in the RMP table. */
	early_set_pages_state(vaddr, paddr, npages, SNP_PAGE_STATE_SHARED);
}

/* Writes to the SVSM CAA MSR are ignored */
static enum es_result __vc_handle_msr_caa(struct pt_regs *regs, bool write)
{
	if (write)
		return ES_OK;

	regs->ax = lower_32_bits(this_cpu_read(svsm_caa_pa));
	regs->dx = upper_32_bits(this_cpu_read(svsm_caa_pa));

	return ES_OK;
}

/*
 * TSC related accesses should not exit to the hypervisor when a guest is
 * executing with Secure TSC enabled, so special handling is required for
 * accesses of MSR_IA32_TSC and MSR_AMD64_GUEST_TSC_FREQ.
 */
static enum es_result __vc_handle_secure_tsc_msrs(struct pt_regs *regs, bool write)
{
	u64 tsc;

	/*
	 * GUEST_TSC_FREQ should not be intercepted when Secure TSC is enabled.
	 * Terminate the SNP guest when the interception is enabled.
	 */
	if (regs->cx == MSR_AMD64_GUEST_TSC_FREQ)
		return ES_VMM_ERROR;

	/*
	 * Writes: Writing to MSR_IA32_TSC can cause subsequent reads of the TSC
	 *         to return undefined values, so ignore all writes.
	 *
	 * Reads: Reads of MSR_IA32_TSC should return the current TSC value, use
	 *        the value returned by rdtsc_ordered().
	 */
	if (write) {
		WARN_ONCE(1, "TSC MSR writes are verboten!\n");
		return ES_OK;
	}

	tsc = rdtsc_ordered();
	regs->ax = lower_32_bits(tsc);
	regs->dx = upper_32_bits(tsc);

	return ES_OK;
}

static enum es_result vc_handle_msr(struct ghcb *ghcb, struct es_em_ctxt *ctxt)
{
	struct pt_regs *regs = ctxt->regs;
	enum es_result ret;
	bool write;

	/* Is it a WRMSR? */
	write = ctxt->insn.opcode.bytes[1] == 0x30;

	switch (regs->cx) {
	case MSR_SVSM_CAA:
		return __vc_handle_msr_caa(regs, write);
	case MSR_IA32_TSC:
	case MSR_AMD64_GUEST_TSC_FREQ:
		if (sev_status & MSR_AMD64_SNP_SECURE_TSC)
			return __vc_handle_secure_tsc_msrs(regs, write);
		break;
	default:
		break;
	}

	ghcb_set_rcx(ghcb, regs->cx);
	if (write) {
		ghcb_set_rax(ghcb, regs->ax);
		ghcb_set_rdx(ghcb, regs->dx);
	}

	ret = sev_es_ghcb_hv_call(ghcb, ctxt, SVM_EXIT_MSR, write, 0);

	if ((ret == ES_OK) && !write) {
		regs->ax = ghcb->save.rax;
		regs->dx = ghcb->save.rdx;
	}

	return ret;
}

static void __init vc_early_forward_exception(struct es_em_ctxt *ctxt)
{
	int trapnr = ctxt->fi.vector;

	if (trapnr == X86_TRAP_PF)
		native_write_cr2(ctxt->fi.cr2);

	ctxt->regs->orig_ax = ctxt->fi.error_code;
	do_early_exception(ctxt->regs, trapnr);
}

static long *vc_insn_get_rm(struct es_em_ctxt *ctxt)
{
	long *reg_array;
	int offset;

	reg_array = (long *)ctxt->regs;
	offset    = insn_get_modrm_rm_off(&ctxt->insn, ctxt->regs);

	if (offset < 0)
		return NULL;

	offset /= sizeof(long);

	return reg_array + offset;
}
static enum es_result vc_do_mmio(struct ghcb *ghcb, struct es_em_ctxt *ctxt,
				 unsigned int bytes, bool read)
{
	u64 exit_code, exit_info_1, exit_info_2;
	unsigned long ghcb_pa = __pa(ghcb);
	enum es_result res;
	phys_addr_t paddr;
	void __user *ref;

	ref = insn_get_addr_ref(&ctxt->insn, ctxt->regs);
	if (ref == (void __user *)-1L)
		return ES_UNSUPPORTED;

	exit_code = read ? SVM_VMGEXIT_MMIO_READ : SVM_VMGEXIT_MMIO_WRITE;

	res = vc_slow_virt_to_phys(ghcb, ctxt, (unsigned long)ref, &paddr);
	if (res != ES_OK) {
		if (res == ES_EXCEPTION && !read)
			ctxt->fi.error_code |= X86_PF_WRITE;

		return res;
	}

	exit_info_1 = paddr;
	/* Can never be greater than 8 */
	exit_info_2 = bytes;

	ghcb_set_sw_scratch(ghcb, ghcb_pa + offsetof(struct ghcb, shared_buffer));

	return sev_es_ghcb_hv_call(ghcb, ctxt, exit_code, exit_info_1, exit_info_2);
}

/*
 * The MOVS instruction has two memory operands, which raises the
 * problem that it is not known whether the access to the source or the
 * destination caused the #VC exception (and hence whether an MMIO read
 * or write operation needs to be emulated).
 *
 * Instead of playing games with walking page-tables and trying to guess
 * whether the source or destination is an MMIO range, split the move
 * into two operations, a read and a write with only one memory operand.
 * This will cause a nested #VC exception on the MMIO address which can
 * then be handled.
 *
 * This implementation has the benefit that it also supports MOVS where
 * source _and_ destination are MMIO regions.
 *
 * It will slow MOVS on MMIO down a lot, but in SEV-ES guests it is a
 * rare operation. If it turns out to be a performance problem the split
 * operations can be moved to memcpy_fromio() and memcpy_toio().
 */
static enum es_result vc_handle_mmio_movs(struct es_em_ctxt *ctxt,
					  unsigned int bytes)
{
	unsigned long ds_base, es_base;
	unsigned char *src, *dst;
	unsigned char buffer[8];
	enum es_result ret;
	bool rep;
	int off;

	ds_base = insn_get_seg_base(ctxt->regs, INAT_SEG_REG_DS);
	es_base = insn_get_seg_base(ctxt->regs, INAT_SEG_REG_ES);

	if (ds_base == -1L || es_base == -1L) {
		ctxt->fi.vector = X86_TRAP_GP;
		ctxt->fi.error_code = 0;
		return ES_EXCEPTION;
	}

	src = ds_base + (unsigned char *)ctxt->regs->si;
	dst = es_base + (unsigned char *)ctxt->regs->di;

	ret = vc_read_mem(ctxt, src, buffer, bytes);
	if (ret != ES_OK)
		return ret;

	ret = vc_write_mem(ctxt, dst, buffer, bytes);
	if (ret != ES_OK)
		return ret;

	if (ctxt->regs->flags & X86_EFLAGS_DF)
		off = -bytes;
	else
		off =  bytes;

	ctxt->regs->si += off;
	ctxt->regs->di += off;

	rep = insn_has_rep_prefix(&ctxt->insn);
	if (rep)
		ctxt->regs->cx -= 1;

	if (!rep || ctxt->regs->cx == 0)
		return ES_OK;
	else
		return ES_RETRY;
}

static enum es_result vc_handle_mmio(struct ghcb *ghcb, struct es_em_ctxt *ctxt)
{
	struct insn *insn = &ctxt->insn;
	enum insn_mmio_type mmio;
	unsigned int bytes = 0;
	enum es_result ret;
	u8 sign_byte;
	long *reg_data;

	mmio = insn_decode_mmio(insn, &bytes);
	if (mmio == INSN_MMIO_DECODE_FAILED)
		return ES_DECODE_FAILED;

	if (mmio != INSN_MMIO_WRITE_IMM && mmio != INSN_MMIO_MOVS) {
		reg_data = insn_get_modrm_reg_ptr(insn, ctxt->regs);
		if (!reg_data)
			return ES_DECODE_FAILED;
	}

	if (user_mode(ctxt->regs))
		return ES_UNSUPPORTED;

	switch (mmio) {
	case INSN_MMIO_WRITE:
		memcpy(ghcb->shared_buffer, reg_data, bytes);
		ret = vc_do_mmio(ghcb, ctxt, bytes, false);
		break;
	case INSN_MMIO_WRITE_IMM:
		memcpy(ghcb->shared_buffer, insn->immediate1.bytes, bytes);
		ret = vc_do_mmio(ghcb, ctxt, bytes, false);
		break;
	case INSN_MMIO_READ:
		ret = vc_do_mmio(ghcb, ctxt, bytes, true);
		if (ret)
			break;

		/* Zero-extend for 32-bit operation */
		if (bytes == 4)
			*reg_data = 0;

		memcpy(reg_data, ghcb->shared_buffer, bytes);
		break;
	case INSN_MMIO_READ_ZERO_EXTEND:
		ret = vc_do_mmio(ghcb, ctxt, bytes, true);
		if (ret)
			break;

		/* Zero extend based on operand size */
		memset(reg_data, 0, insn->opnd_bytes);
		memcpy(reg_data, ghcb->shared_buffer, bytes);
		break;
	case INSN_MMIO_READ_SIGN_EXTEND:
		ret = vc_do_mmio(ghcb, ctxt, bytes, true);
		if (ret)
			break;

		if (bytes == 1) {
			u8 *val = (u8 *)ghcb->shared_buffer;

			sign_byte = (*val & 0x80) ? 0xff : 0x00;
		} else {
			u16 *val = (u16 *)ghcb->shared_buffer;

			sign_byte = (*val & 0x8000) ? 0xff : 0x00;
		}

		/* Sign extend based on operand size */
		memset(reg_data, sign_byte, insn->opnd_bytes);
		memcpy(reg_data, ghcb->shared_buffer, bytes);
		break;
	case INSN_MMIO_MOVS:
		ret = vc_handle_mmio_movs(ctxt, bytes);
		break;
	default:
		ret = ES_UNSUPPORTED;
		break;
	}

	return ret;
}

static enum es_result vc_handle_dr7_write(struct ghcb *ghcb,
					  struct es_em_ctxt *ctxt)
{
	struct sev_es_runtime_data *data = this_cpu_read(runtime_data);
	long val, *reg = vc_insn_get_rm(ctxt);
	enum es_result ret;

	if (sev_status & MSR_AMD64_SNP_DEBUG_SWAP)
		return ES_VMM_ERROR;

	if (!reg)
		return ES_DECODE_FAILED;

	val = *reg;

	/* Upper 32 bits must be written as zeroes */
	if (val >> 32) {
		ctxt->fi.vector = X86_TRAP_GP;
		ctxt->fi.error_code = 0;
		return ES_EXCEPTION;
	}

	/* Clear out other reserved bits and set bit 10 */
	val = (val & 0xffff23ffL) | BIT(10);

	/* Early non-zero writes to DR7 are not supported */
	if (!data && (val & ~DR7_RESET_VALUE))
		return ES_UNSUPPORTED;

	/* Using a value of 0 for ExitInfo1 means RAX holds the value */
	ghcb_set_rax(ghcb, val);
	ret = sev_es_ghcb_hv_call(ghcb, ctxt, SVM_EXIT_WRITE_DR7, 0, 0);
	if (ret != ES_OK)
		return ret;

	if (data)
		data->dr7 = val;

	return ES_OK;
}

static enum es_result vc_handle_dr7_read(struct ghcb *ghcb,
					 struct es_em_ctxt *ctxt)
{
	struct sev_es_runtime_data *data = this_cpu_read(runtime_data);
	long *reg = vc_insn_get_rm(ctxt);

	if (sev_status & MSR_AMD64_SNP_DEBUG_SWAP)
		return ES_VMM_ERROR;

	if (!reg)
		return ES_DECODE_FAILED;

	if (data)
		*reg = data->dr7;
	else
		*reg = DR7_RESET_VALUE;

	return ES_OK;
}

static enum es_result vc_handle_wbinvd(struct ghcb *ghcb,
				       struct es_em_ctxt *ctxt)
{
	return sev_es_ghcb_hv_call(ghcb, ctxt, SVM_EXIT_WBINVD, 0, 0);
}

static enum es_result vc_handle_rdpmc(struct ghcb *ghcb, struct es_em_ctxt *ctxt)
{
	enum es_result ret;

	ghcb_set_rcx(ghcb, ctxt->regs->cx);

	ret = sev_es_ghcb_hv_call(ghcb, ctxt, SVM_EXIT_RDPMC, 0, 0);
	if (ret != ES_OK)
		return ret;

	if (!(ghcb_rax_is_valid(ghcb) && ghcb_rdx_is_valid(ghcb)))
		return ES_VMM_ERROR;

	ctxt->regs->ax = ghcb->save.rax;
	ctxt->regs->dx = ghcb->save.rdx;

	return ES_OK;
}

static enum es_result vc_handle_monitor(struct ghcb *ghcb,
					struct es_em_ctxt *ctxt)
{
	/*
	 * Treat it as a NOP and do not leak a physical address to the
	 * hypervisor.
	 */
	return ES_OK;
}

static enum es_result vc_handle_mwait(struct ghcb *ghcb,
				      struct es_em_ctxt *ctxt)
{
	/* Treat the same as MONITOR/MONITORX */
	return ES_OK;
}

static enum es_result vc_handle_vmmcall(struct ghcb *ghcb,
					struct es_em_ctxt *ctxt)
{
	enum es_result ret;

	ghcb_set_rax(ghcb, ctxt->regs->ax);
	ghcb_set_cpl(ghcb, user_mode(ctxt->regs) ? 3 : 0);

	if (x86_platform.hyper.sev_es_hcall_prepare)
		x86_platform.hyper.sev_es_hcall_prepare(ghcb, ctxt->regs);

	ret = sev_es_ghcb_hv_call(ghcb, ctxt, SVM_EXIT_VMMCALL, 0, 0);
	if (ret != ES_OK)
		return ret;

	if (!ghcb_rax_is_valid(ghcb))
		return ES_VMM_ERROR;

	ctxt->regs->ax = ghcb->save.rax;

	/*
	 * Call sev_es_hcall_finish() after regs->ax is already set.
	 * This allows the hypervisor handler to overwrite it again if
	 * necessary.
	 */
	if (x86_platform.hyper.sev_es_hcall_finish &&
	    !x86_platform.hyper.sev_es_hcall_finish(ghcb, ctxt->regs))
		return ES_VMM_ERROR;

	return ES_OK;
}

static enum es_result vc_handle_trap_ac(struct ghcb *ghcb,
					struct es_em_ctxt *ctxt)
{
	/*
	 * Calling ecx_alignment_check() directly does not work, because it
	 * enables IRQs and the GHCB is active. Forward the exception and call
	 * it later from vc_forward_exception().
	 */
	ctxt->fi.vector = X86_TRAP_AC;
	ctxt->fi.error_code = 0;
	return ES_EXCEPTION;
}

static enum es_result vc_handle_exitcode(struct es_em_ctxt *ctxt,
					 struct ghcb *ghcb,
					 unsigned long exit_code)
{
	enum es_result result = vc_check_opcode_bytes(ctxt, exit_code);

	if (result != ES_OK)
		return result;

	switch (exit_code) {
	case SVM_EXIT_READ_DR7:
		result = vc_handle_dr7_read(ghcb, ctxt);
		break;
	case SVM_EXIT_WRITE_DR7:
		result = vc_handle_dr7_write(ghcb, ctxt);
		break;
	case SVM_EXIT_EXCP_BASE + X86_TRAP_AC:
		result = vc_handle_trap_ac(ghcb, ctxt);
		break;
	case SVM_EXIT_RDTSC:
	case SVM_EXIT_RDTSCP:
		result = vc_handle_rdtsc(ghcb, ctxt, exit_code);
		break;
	case SVM_EXIT_RDPMC:
		result = vc_handle_rdpmc(ghcb, ctxt);
		break;
	case SVM_EXIT_INVD:
		pr_err_ratelimited("#VC exception for INVD??? Seriously???\n");
		result = ES_UNSUPPORTED;
		break;
	case SVM_EXIT_CPUID:
		result = vc_handle_cpuid(ghcb, ctxt);
		break;
	case SVM_EXIT_IOIO:
		result = vc_handle_ioio(ghcb, ctxt);
		break;
	case SVM_EXIT_MSR:
		result = vc_handle_msr(ghcb, ctxt);
		break;
	case SVM_EXIT_VMMCALL:
		result = vc_handle_vmmcall(ghcb, ctxt);
		break;
	case SVM_EXIT_WBINVD:
		result = vc_handle_wbinvd(ghcb, ctxt);
		break;
	case SVM_EXIT_MONITOR:
		result = vc_handle_monitor(ghcb, ctxt);
		break;
	case SVM_EXIT_MWAIT:
		result = vc_handle_mwait(ghcb, ctxt);
		break;
	case SVM_EXIT_NPF:
		result = vc_handle_mmio(ghcb, ctxt);
		break;
	default:
		/*
		 * Unexpected #VC exception
		 */
		result = ES_UNSUPPORTED;
	}

	return result;
}

static __always_inline bool is_vc2_stack(unsigned long sp)
{
	return (sp >= __this_cpu_ist_bottom_va(VC2) && sp < __this_cpu_ist_top_va(VC2));
}

static __always_inline bool vc_from_invalid_context(struct pt_regs *regs)
{
	unsigned long sp, prev_sp;

	sp      = (unsigned long)regs;
	prev_sp = regs->sp;

	/*
	 * If the code was already executing on the VC2 stack when the #VC
	 * happened, let it proceed to the normal handling routine. This way the
	 * code executing on the VC2 stack can cause #VC exceptions to get handled.
	 */
	return is_vc2_stack(sp) && !is_vc2_stack(prev_sp);
}

static bool vc_raw_handle_exception(struct pt_regs *regs, unsigned long error_code)
{
	struct ghcb_state state;
	struct es_em_ctxt ctxt;
	enum es_result result;
	struct ghcb *ghcb;
	bool ret = true;

	ghcb = __sev_get_ghcb(&state);

	vc_ghcb_invalidate(ghcb);
	result = vc_init_em_ctxt(&ctxt, regs, error_code);

	if (result == ES_OK)
		result = vc_handle_exitcode(&ctxt, ghcb, error_code);

	__sev_put_ghcb(&state);

	/* Done - now check the result */
	switch (result) {
	case ES_OK:
		vc_finish_insn(&ctxt);
		break;
	case ES_UNSUPPORTED:
		pr_err_ratelimited("Unsupported exit-code 0x%02lx in #VC exception (IP: 0x%lx)\n",
				   error_code, regs->ip);
		ret = false;
		break;
	case ES_VMM_ERROR:
		pr_err_ratelimited("Failure in communication with VMM (exit-code 0x%02lx IP: 0x%lx)\n",
				   error_code, regs->ip);
		ret = false;
		break;
	case ES_DECODE_FAILED:
		pr_err_ratelimited("Failed to decode instruction (exit-code 0x%02lx IP: 0x%lx)\n",
				   error_code, regs->ip);
		ret = false;
		break;
	case ES_EXCEPTION:
		vc_forward_exception(&ctxt);
		break;
	case ES_RETRY:
		/* Nothing to do */
		break;
	default:
		pr_emerg("Unknown result in %s():%d\n", __func__, result);
		/*
		 * Emulating the instruction which caused the #VC exception
		 * failed - can't continue so print debug information
		 */
		BUG();
	}

	return ret;
}

static __always_inline bool vc_is_db(unsigned long error_code)
{
	return error_code == SVM_EXIT_EXCP_BASE + X86_TRAP_DB;
}

/*
 * Runtime #VC exception handler when raised from kernel mode. Runs in NMI mode
 * and will panic when an error happens.
 */
DEFINE_IDTENTRY_VC_KERNEL(exc_vmm_communication)
{
	irqentry_state_t irq_state;

	/*
	 * With the current implementation it is always possible to switch to a
	 * safe stack because #VC exceptions only happen at known places, like
	 * intercepted instructions or accesses to MMIO areas/IO ports. They can
	 * also happen with code instrumentation when the hypervisor intercepts
	 * #DB, but the critical paths are forbidden to be instrumented, so #DB
	 * exceptions currently also only happen in safe places.
	 *
	 * But keep this here in case the noinstr annotations are violated due
	 * to bug elsewhere.
	 */
	if (unlikely(vc_from_invalid_context(regs))) {
		instrumentation_begin();
		panic("Can't handle #VC exception from unsupported context\n");
		instrumentation_end();
	}

	/*
	 * Handle #DB before calling into !noinstr code to avoid recursive #DB.
	 */
	if (vc_is_db(error_code)) {
		exc_debug(regs);
		return;
	}

	irq_state = irqentry_nmi_enter(regs);

	instrumentation_begin();

	if (!vc_raw_handle_exception(regs, error_code)) {
		/* Show some debug info */
		show_regs(regs);

		/* Ask hypervisor to sev_es_terminate */
		sev_es_terminate(SEV_TERM_SET_GEN, GHCB_SEV_ES_GEN_REQ);

		/* If that fails and we get here - just panic */
		panic("Returned from Terminate-Request to Hypervisor\n");
	}

	instrumentation_end();
	irqentry_nmi_exit(regs, irq_state);
}

/*
 * Runtime #VC exception handler when raised from user mode. Runs in IRQ mode
 * and will kill the current task with SIGBUS when an error happens.
 */
DEFINE_IDTENTRY_VC_USER(exc_vmm_communication)
{
	/*
	 * Handle #DB before calling into !noinstr code to avoid recursive #DB.
	 */
	if (vc_is_db(error_code)) {
		noist_exc_debug(regs);
		return;
	}

	irqentry_enter_from_user_mode(regs);
	instrumentation_begin();

	if (!vc_raw_handle_exception(regs, error_code)) {
		/*
		 * Do not kill the machine if user-space triggered the
		 * exception. Send SIGBUS instead and let user-space deal with
		 * it.
		 */
		force_sig_fault(SIGBUS, BUS_OBJERR, (void __user *)0);
	}

	instrumentation_end();
	irqentry_exit_to_user_mode(regs);
}

bool __init handle_vc_boot_ghcb(struct pt_regs *regs)
{
	unsigned long exit_code = regs->orig_ax;
	struct es_em_ctxt ctxt;
	enum es_result result;

	vc_ghcb_invalidate(boot_ghcb);

	result = vc_init_em_ctxt(&ctxt, regs, exit_code);
	if (result == ES_OK)
		result = vc_handle_exitcode(&ctxt, boot_ghcb, exit_code);

	/* Done - now check the result */
	switch (result) {
	case ES_OK:
		vc_finish_insn(&ctxt);
		break;
	case ES_UNSUPPORTED:
		early_printk("PANIC: Unsupported exit-code 0x%02lx in early #VC exception (IP: 0x%lx)\n",
				exit_code, regs->ip);
		goto fail;
	case ES_VMM_ERROR:
		early_printk("PANIC: Failure in communication with VMM (exit-code 0x%02lx IP: 0x%lx)\n",
				exit_code, regs->ip);
		goto fail;
	case ES_DECODE_FAILED:
		early_printk("PANIC: Failed to decode instruction (exit-code 0x%02lx IP: 0x%lx)\n",
				exit_code, regs->ip);
		goto fail;
	case ES_EXCEPTION:
		vc_early_forward_exception(&ctxt);
		break;
	case ES_RETRY:
		/* Nothing to do */
		break;
	default:
		BUG();
	}

	return true;

fail:
	show_regs(regs);

	sev_es_terminate(SEV_TERM_SET_GEN, GHCB_SEV_ES_GEN_REQ);
}

/*
 * Initial set up of SNP relies on information provided by the
 * Confidential Computing blob, which can be passed to the kernel
 * in the following ways, depending on how it is booted:
 *
 * - when booted via the boot/decompress kernel:
 *   - via boot_params
 *
 * - when booted directly by firmware/bootloader (e.g. CONFIG_PVH):
 *   - via a setup_data entry, as defined by the Linux Boot Protocol
 *
 * Scan for the blob in that order.
 */
static __head struct cc_blob_sev_info *find_cc_blob(struct boot_params *bp)
{
	struct cc_blob_sev_info *cc_info;

	/* Boot kernel would have passed the CC blob via boot_params. */
	if (bp->cc_blob_address) {
		cc_info = (struct cc_blob_sev_info *)(unsigned long)bp->cc_blob_address;
		goto found_cc_info;
	}

	/*
	 * If kernel was booted directly, without the use of the
	 * boot/decompression kernel, the CC blob may have been passed via
	 * setup_data instead.
	 */
	cc_info = find_cc_blob_setup_data(bp);
	if (!cc_info)
		return NULL;

found_cc_info:
	if (cc_info->magic != CC_BLOB_SEV_HDR_MAGIC)
		snp_abort();

	return cc_info;
}

static __head void svsm_setup(struct cc_blob_sev_info *cc_info)
{
	struct svsm_call call = {};
	int ret;
	u64 pa;

	/*
	 * Record the SVSM Calling Area address (CAA) if the guest is not
	 * running at VMPL0. The CA will be used to communicate with the
	 * SVSM to perform the SVSM services.
	 */
	if (!svsm_setup_ca(cc_info))
		return;

	/*
	 * It is very early in the boot and the kernel is running identity
	 * mapped but without having adjusted the pagetables to where the
	 * kernel was loaded (physbase), so the get the CA address using
	 * RIP-relative addressing.
	 */
	pa = (u64)rip_rel_ptr(&boot_svsm_ca_page);

	/*
	 * Switch over to the boot SVSM CA while the current CA is still
	 * addressable. There is no GHCB at this point so use the MSR protocol.
	 *
	 * SVSM_CORE_REMAP_CA call:
	 *   RAX = 0 (Protocol=0, CallID=0)
	 *   RCX = New CA GPA
	 */
	call.caa = svsm_get_caa();
	call.rax = SVSM_CORE_CALL(SVSM_CORE_REMAP_CA);
	call.rcx = pa;
	ret = svsm_perform_call_protocol(&call);
	if (ret)
		sev_es_terminate(SEV_TERM_SET_LINUX, GHCB_TERM_SVSM_CA_REMAP_FAIL);

	RIP_REL_REF(boot_svsm_caa) = (struct svsm_ca *)pa;
	RIP_REL_REF(boot_svsm_caa_pa) = pa;
}

bool __head snp_init(struct boot_params *bp)
{
	struct cc_blob_sev_info *cc_info;

	if (!bp)
		return false;

	cc_info = find_cc_blob(bp);
	if (!cc_info)
		return false;

	if (cc_info->secrets_phys && cc_info->secrets_len == PAGE_SIZE)
		secrets_pa = cc_info->secrets_phys;
	else
		return false;

	setup_cpuid_table(cc_info);

	svsm_setup(cc_info);

	/*
	 * The CC blob will be used later to access the secrets page. Cache
	 * it here like the boot kernel does.
	 */
	bp->cc_blob_address = (u32)(unsigned long)cc_info;

	return true;
}

void __head __noreturn snp_abort(void)
{
	sev_es_terminate(SEV_TERM_SET_GEN, GHCB_SNP_UNSUPPORTED);
}
