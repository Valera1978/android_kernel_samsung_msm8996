/*
 * Based on arch/arm/include/asm/mmu_context.h
 *
 * Copyright (C) 1996 Russell King.
 * Copyright (C) 2012 ARM Ltd.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */
#ifndef __ASM_MMU_CONTEXT_H
#define __ASM_MMU_CONTEXT_H

#include <linux/compiler.h>
#include <linux/sched.h>

#include <asm/cacheflush.h>
#include <asm/cpufeature.h>
#include <asm/proc-fns.h>
#include <asm-generic/mm_hooks.h>
#include <asm/cputype.h>
#include <asm/pgtable.h>
#include <linux/msm_rtb.h>

#ifdef CONFIG_PID_IN_CONTEXTIDR
static inline void contextidr_thread_switch(struct task_struct *next)
{
	pid_t pid = task_pid_nr(next);
	asm(
	"	msr	contextidr_el1, %0\n"
	"	isb"
	:
	: "r" (pid));
	uncached_logk(LOGK_CTXID, (void *)(u64)pid);

}
#else
static inline void contextidr_thread_switch(struct task_struct *next)
{
	uncached_logk(LOGK_CTXID, (void *)(u64)task_pid_nr(next));
}
#endif

/*
 * Set TTBR0 to empty_zero_page. No translations will be possible via TTBR0.
 */
static inline void cpu_set_reserved_ttbr0(void)
{
	unsigned long ttbr = virt_to_phys(empty_zero_page);
#ifndef CONFIG_TIMA_RKP
	asm(
	"	msr	ttbr0_el1, %0			// set TTBR0\n"
	"	isb"
	:
	: "r" (ttbr));
#else
	rkp_call(RKP_EMULT_TTBR0, (unsigned long)ttbr, 0, 0, 0, 0);
	asm(
	"	isb"
	::);
#endif
}

/*
 * TCR.T0SZ value to use when the ID map is active. Usually equals
 * TCR_T0SZ(VA_BITS), unless system RAM is positioned very high in
 * physical memory, in which case it will be smaller.
 */
extern u64 idmap_t0sz;

static inline bool __cpu_uses_extended_idmap(void)
{
	return (!IS_ENABLED(CONFIG_ARM64_VA_BITS_48) &&
		unlikely(idmap_t0sz != TCR_T0SZ(VA_BITS)));
}

/*
 * Set TCR.T0SZ to its default value (based on VA_BITS)
 */
static inline void cpu_set_default_tcr_t0sz(void)
{
	unsigned long tcr;

	if (!__cpu_uses_extended_idmap())
		return;

	asm volatile (
	"	mrs	%0, tcr_el1	;"
	"	bfi	%0, %1, %2, %3	;"
	"	msr	tcr_el1, %0	;"
	"	isb"
	: "=&r" (tcr)
	: "r"(TCR_T0SZ(VA_BITS)), "I"(TCR_T0SZ_OFFSET), "I"(TCR_TxSZ_WIDTH));
}

/*
 * It would be nice to return ASIDs back to the allocator, but unfortunately
 * that introduces a race with a generation rollover where we could erroneously
 * free an ASID allocated in a future generation. We could workaround this by
 * freeing the ASID from the context of the dying mm (e.g. in arch_exit_mmap),
 * but we'd then need to make sure that we didn't dirty any TLBs afterwards.
 * Setting a reserved TTBR0 or EPD0 would work, but it all gets ugly when you
 * take CPU migration into account.
 */
#define destroy_context(mm)		do { } while(0)
void check_and_switch_context(struct mm_struct *mm, unsigned int cpu);

#define init_new_context(tsk,mm)	({ atomic64_set(&(mm)->context.id, 0); 0; })

#ifdef CONFIG_ARM64_SW_TTBR0_PAN
static inline void update_saved_ttbr0(struct task_struct *tsk,
				      struct mm_struct *mm)
{
	u64 ttbr; 

	if (!system_uses_ttbr0_pan()) 
		return; 
 
	if (mm == &init_mm) 
		ttbr = __pa_symbol(empty_zero_page); 
	else 
		ttbr = virt_to_phys(mm->pgd) | ASID(mm) << 48; 

	task_thread_info(tsk)->ttbr0 = ttbr;
}
#else
static inline void update_saved_ttbr0(struct task_struct *tsk,
				      struct mm_struct *mm)
{
}
#endif

static inline void 
enter_lazy_tlb(struct mm_struct *mm, struct task_struct *tsk) 
{
	/* 
	* We don't actually care about the ttbr0 mapping, so point it at the 
	* zero page. 
	*/ 
	update_saved_ttbr0(tsk, &init_mm); 
}

static inline void __switch_mm(struct mm_struct *next)
{
	unsigned int cpu = smp_processor_id();

	/*
	 * init_mm.pgd does not contain any user mappings and it is always
	 * active for kernel addresses in TTBR1. Just set the reserved TTBR0.
	 */
	if (next == &init_mm) {
		cpu_set_reserved_ttbr0();
		return;
	}

	check_and_switch_context(next, cpu);
}

static inline void
switch_mm(struct mm_struct *prev, struct mm_struct *next,
	  struct task_struct *tsk)
{
	if (prev != next)
		__switch_mm(next);

	/*
	 * Update the saved TTBR0_EL1 of the scheduled-in task as the previous
	 * value may have not been initialised yet (activate_mm caller) or the
	 * ASID has changed since the last run (following the context switch
	 * of another thread of the same process).
	 */
	update_saved_ttbr0(tsk, next);
}

#define deactivate_mm(tsk,mm)	do { } while (0)
#define activate_mm(prev,next)	switch_mm(prev, next, current)

#endif
