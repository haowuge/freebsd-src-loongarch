/*-
 * Copyright (C) 1994, David Greenman
 * Copyright (c) 1990, 1993
 *	The Regents of the University of California.  All rights reserved.
 *
 * This code is derived from software contributed to Berkeley by
 * the University of Utah, and William Jolitz.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. All advertising materials mentioning features or use of this software
 *    must display the following acknowledgement:
 *	This product includes software developed by the University of
 *	California, Berkeley and its contributors.
 * 4. Neither the name of the University nor the names of its contributors
 *    may be used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE REGENTS AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE REGENTS OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 *
 *	from: @(#)trap.c	7.4 (Berkeley) 5/13/91
 *	$Id: trap.c,v 1.42 1994/12/24 07:22:58 bde Exp $
 */

/*
 * 386 Trap and System call handling
 */

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/proc.h>
#include <sys/user.h>
#include <sys/acct.h>
#include <sys/kernel.h>
#include <sys/syscall.h>
#include <sys/sysent.h>
#ifdef KTRACE
#include <sys/ktrace.h>
#endif

#include <vm/vm_param.h>
#include <vm/pmap.h>
#include <vm/vm_map.h>
#include <vm/vm_page.h>

#include <machine/cpu.h>
#include <machine/psl.h>
#include <machine/reg.h>
#include <machine/trap.h>
#include <machine/../isa/isa_device.h>

#include "isa.h"
#include "npx.h"

int	trap_pfault	__P((struct trapframe *, int));
void	trap_fatal	__P((struct trapframe *));

#define MAX_TRAP_MSG		27
char *trap_msg[] = {
	"reserved addressing fault",		/*  0 T_RESADFLT */
	"privileged instruction fault",		/*  1 T_PRIVINFLT */
	"reserved operand fault",		/*  2 T_RESOPFLT */
	"breakpoint instruction fault",		/*  3 T_BPTFLT */
	"",					/*  4 unused */
	"system call trap",			/*  5 T_SYSCALL */
	"arithmetic trap",			/*  6 T_ARITHTRAP */
	"system forced exception",		/*  7 T_ASTFLT */
	"segmentation (limit) fault",		/*  8 T_SEGFLT */
	"general protection fault",		/*  9 T_PROTFLT */
	"trace trap",				/* 10 T_TRCTRAP */
	"",					/* 11 unused */
	"page fault",				/* 12 T_PAGEFLT */
	"page table fault",			/* 13 T_TABLEFLT */
	"alignment fault",			/* 14 T_ALIGNFLT */
	"kernel stack pointer not valid",	/* 15 T_KSPNOTVAL */
	"bus error",				/* 16 T_BUSERR */
	"kernel debugger fault",		/* 17 T_KDBTRAP */
	"integer divide fault",			/* 18 T_DIVIDE */
	"non-maskable interrupt trap",		/* 19 T_NMI */
	"overflow trap",			/* 20 T_OFLOW */
	"FPU bounds check fault",		/* 21 T_BOUND */
	"FPU device not available",		/* 22 T_DNA */
	"double fault",				/* 23 T_DOUBLEFLT */
	"FPU operand fetch fault",		/* 24 T_FPOPFLT */
	"invalid TSS fault",			/* 25 T_TSSFLT */
	"segment not present fault",		/* 26 T_SEGNPFLT */
	"stack fault",				/* 27 T_STKFLT */
};

static inline void
userret(p, frame, oticks)
	struct proc *p;
	struct trapframe *frame;
	u_quad_t oticks;
{
	int sig, s;

	while ((sig = CURSIG(p)) != 0)
		postsig(sig);
	p->p_priority = p->p_usrpri;
	if (want_resched) {
		/*
		 * Since we are curproc, clock will normally just change
		 * our priority without moving us from one queue to another
		 * (since the running process is not on a queue.)
		 * If that happened after we setrunqueue ourselves but before we
		 * mi_switch()'ed, we might not be on the queue indicated by
		 * our priority.
		 */
		s = splclock();
		setrunqueue(p);
		p->p_stats->p_ru.ru_nivcsw++;
		mi_switch();
		splx(s);
		while ((sig = CURSIG(p)) != 0)
			postsig(sig);
	}
	if (p->p_stats->p_prof.pr_scale) {
		u_quad_t ticks = p->p_sticks - oticks;

		if (ticks) {
#ifdef PROFTIMER
			extern int profscale;
			addupc(frame->tf_eip, &p->p_stats->p_prof,
			    ticks * profscale);
#else
			addupc(frame->tf_eip, &p->p_stats->p_prof, ticks);
#endif
		}
	}
	curpriority = p->p_priority;
}

/*
 * trap(frame):
 *	Exception, fault, and trap interface to the FreeBSD kernel.
 * This common code is called from assembly language IDT gate entry
 * routines that prepare a suitable stack frame, and restore this
 * frame after the exception has been processed.
 */

/*ARGSUSED*/
void
trap(frame)
	struct trapframe frame;
{
	struct proc *p = curproc;
	u_quad_t sticks = 0;
	int i = 0, ucode = 0, type, code;
#ifdef DIAGNOSTIC
	u_long eva;
#endif

	frame.tf_eflags &= ~PSL_NT;	/* clear nested trap XXX */
	type = frame.tf_trapno;
	code = frame.tf_err;
	
	if (ISPL(frame.tf_cs) == SEL_UPL) {
		/* user trap */

		sticks = p->p_sticks;
		p->p_md.md_regs = (int *)&frame;

		switch (type) {
		case T_RESADFLT:	/* reserved addressing fault */
		case T_PRIVINFLT:	/* privileged instruction fault */
		case T_RESOPFLT:	/* reserved operand fault */
			ucode = type;
			i = SIGILL;
			break;

		case T_BPTFLT:		/* bpt instruction fault */
		case T_TRCTRAP:		/* trace trap */
			frame.tf_eflags &= ~PSL_T;
			i = SIGTRAP;
			break;

		case T_ARITHTRAP:	/* arithmetic trap */
			ucode = code;
			i = SIGFPE;
			break;

		case T_ASTFLT:		/* Allow process switch */
			astoff();
			cnt.v_soft++;
			if ((p->p_flag & P_OWEUPC) && p->p_stats->p_prof.pr_scale) {
				addupc(frame.tf_eip, &p->p_stats->p_prof, 1);
				p->p_flag &= ~P_OWEUPC;
			}
			goto out;

		case T_PROTFLT:		/* general protection fault */
		case T_SEGNPFLT:	/* segment not present fault */
		case T_STKFLT:		/* stack fault */
			ucode = code + BUS_SEGM_FAULT ;
			i = SIGBUS;
			break;

		case T_PAGEFLT:		/* page fault */
			i = trap_pfault(&frame, TRUE);
			if (i == -1)
				return;
			if (i == 0)
				goto out;

			ucode = T_PAGEFLT;
			break;

		case T_DIVIDE:		/* integer divide fault */
			ucode = FPE_INTDIV_TRAP;
			i = SIGFPE;
			break;

#if NISA > 0
		case T_NMI:
#ifdef DDB
			/* NMI can be hooked up to a pushbutton for debugging */
			printf ("NMI ... going to debugger\n");
			if (kdb_trap (type, 0, &frame))
				return;
#endif
			/* machine/parity/power fail/"kitchen sink" faults */
			if (isa_nmi(code) == 0) return;
			panic("NMI indicates hardware failure");
#endif

		case T_OFLOW:		/* integer overflow fault */
			ucode = FPE_INTOVF_TRAP;
			i = SIGFPE;
			break;

		case T_BOUND:		/* bounds check fault */
			ucode = FPE_SUBRNG_TRAP;
			i = SIGFPE;
			break;

		case T_DNA:
#if NNPX > 0
			/* if a transparent fault (due to context switch "late") */
			if (npxdna())
				return;
#endif	/* NNPX > 0 */

#if defined(MATH_EMULATE) || defined(GPL_MATH_EMULATE) 
			i = math_emulate(&frame);
			if (i == 0) {
				if (!(frame.tf_eflags & PSL_T))
					return;
				frame.tf_eflags &= ~PSL_T;
				i = SIGTRAP;
			}
			/* else ucode = emulator_only_knows() XXX */
#else	/* MATH_EMULATE || GPL_MATH_EMULATE */
			i = SIGFPE;
			ucode = FPE_FPU_NP_TRAP;
#endif	/* MATH_EMULATE || GPL_MATH_EMULATE */
			break;

		case T_FPOPFLT:		/* FPU operand fetch fault */
			ucode = T_FPOPFLT;
			i = SIGILL;
			break;

		default:
			trap_fatal(&frame);
			return;
		}
	} else {
		/* kernel trap */

		switch (type) {
		case T_PAGEFLT:			/* page fault */
			(void) trap_pfault(&frame, FALSE);
			return;

		case T_PROTFLT:		/* general protection fault */
		case T_SEGNPFLT:	/* segment not present fault */
			if (curpcb && curpcb->pcb_onfault) {
				frame.tf_eip = (int)curpcb->pcb_onfault;
				return;
			}
			break;

#ifdef DDB
		case T_BPTFLT:
		case T_TRCTRAP:
			if (kdb_trap (type, 0, &frame))
				return;
			break;
#else
		case T_TRCTRAP:	 /* trace trap -- someone single stepping lcall's */
			/* Q: how do we turn it on again? */
			frame.tf_eflags &= ~PSL_T;
			return;
#endif
	
#if NISA > 0
		case T_NMI:
#ifdef DDB
			/* NMI can be hooked up to a pushbutton for debugging */
			printf ("NMI ... going to debugger\n");
			if (kdb_trap (type, 0, &frame))
				return;
#endif
			/* machine/parity/power fail/"kitchen sink" faults */
			if (isa_nmi(code) == 0) return;
			/* FALL THROUGH */
#endif
		}

		trap_fatal(&frame);
		return;
	}

	trapsignal(p, i, ucode);

#ifdef DIAGNOSTIC
	eva = rcr2();
	if (type <= MAX_TRAP_MSG) {
		uprintf("fatal process exception: %s", 
			trap_msg[type]);
		if ((type == T_PAGEFLT) || (type == T_PROTFLT))
			uprintf(", fault VA = 0x%x", eva);
		uprintf("\n");
	}
#endif

out:
	userret(p, &frame, sticks);
}

int
trap_pfault(frame, usermode)
	struct trapframe *frame;
	int usermode;
{
	vm_offset_t va;
	struct vmspace *vm = NULL;
	vm_map_t map = 0;
	int rv = 0;
	vm_prot_t ftype;
	extern vm_map_t kernel_map;
	int eva;
	struct proc *p = curproc;

	eva = rcr2();
	va = trunc_page((vm_offset_t)eva);

	if (va >= KERNBASE) {
		/*
		 * Don't allow user-mode faults in kernel address space.
		 */
		if (usermode)
			goto nogo;

		map = kernel_map;
	} else {
		/*
		 * This is a fault on non-kernel virtual memory.
		 * vm is initialized above to NULL. If curproc is NULL
		 * or curproc->p_vmspace is NULL the fault is fatal.
		 */
		if (p != NULL)
			vm = p->p_vmspace;

		if (vm == NULL)
			goto nogo;

		map = &vm->vm_map;
	}

	if (frame->tf_err & PGEX_W)
		ftype = VM_PROT_READ | VM_PROT_WRITE;
	else
		ftype = VM_PROT_READ;

	if (map != kernel_map) {
		vm_offset_t v = (vm_offset_t) vtopte(va);
		vm_page_t ptepg;

		/*
		 * Keep swapout from messing with us during this
		 *	critical time.
		 */
		++p->p_lock;

		/*
		 * Grow the stack if necessary
		 */
		if ((caddr_t)va > vm->vm_maxsaddr
		    && (caddr_t)va < (caddr_t)USRSTACK) {
			if (!grow(p, va)) {
				rv = KERN_FAILURE;
				--p->p_lock;
				goto nogo;
			}
		}

		/*
		 * Check if page table is mapped, if not,
		 *	fault it first
		 */

		/* Fault the pte only if needed: */
		*(volatile char *)v += 0;	

		ptepg = (vm_page_t) pmap_pte_vm_page(vm_map_pmap(map), v);
		vm_page_hold(ptepg);

		/* Fault in the user page: */
		rv = vm_fault(map, va, ftype, FALSE);

		vm_page_unhold(ptepg);

		/*
		 * page table pages don't need to be kept if they
		 * are not held
		 */
		if( ptepg->hold_count == 0 && ptepg->wire_count == 0) {
			pmap_page_protect( VM_PAGE_TO_PHYS(ptepg),
				VM_PROT_NONE);
			vm_page_free(ptepg);
		}

		--p->p_lock;
	} else {
		/*
		 * Since we know that kernel virtual address addresses
		 * always have pte pages mapped, we just have to fault
		 * the page.
		 */
		rv = vm_fault(map, va, ftype, FALSE);
	}

	if (rv == KERN_SUCCESS)
		return (0);
nogo:
	if (!usermode) {
		if (curpcb && curpcb->pcb_onfault) {
			frame->tf_eip = (int)curpcb->pcb_onfault;
			return (0);
		}
		trap_fatal(frame);
		return (-1);
	}

	/* kludge to pass faulting virtual address to sendsig */
	frame->tf_err = eva;

	return((rv == KERN_PROTECTION_FAILURE) ? SIGBUS : SIGSEGV);
}

void
trap_fatal(frame)
	struct trapframe *frame;
{
	int code, type, eva;
	struct soft_segment_descriptor softseg;

	code = frame->tf_err;
	type = frame->tf_trapno;
	eva = rcr2();
	sdtossd(&gdt[IDXSEL(frame->tf_cs & 0xffff)].sd, &softseg);

	if (type <= MAX_TRAP_MSG)
		printf("\n\nFatal trap %d: %s while in %s mode\n",
			type, trap_msg[type],
			ISPL(frame->tf_cs) == SEL_UPL ? "user" : "kernel");
	if (type == T_PAGEFLT) {
		printf("fault virtual address	= 0x%x\n", eva);
		printf("fault code		= %s %s, %s\n",
			code & PGEX_U ? "user" : "supervisor",
			code & PGEX_W ? "write" : "read",
			code & PGEX_P ? "protection violation" : "page not present");
	}
	printf("instruction pointer	= 0x%x:0x%x\n", frame->tf_cs & 0xffff, frame->tf_eip);
	printf("code segment		= base 0x%x, limit 0x%x, type 0x%x\n",
	    softseg.ssd_base, softseg.ssd_limit, softseg.ssd_type);
	printf("			= DPL %d, pres %d, def32 %d, gran %d\n",
	    softseg.ssd_dpl, softseg.ssd_p, softseg.ssd_def32, softseg.ssd_gran);
	printf("processor eflags	= ");
	if (frame->tf_eflags & PSL_T)
		printf("trace/trap, ");
	if (frame->tf_eflags & PSL_I)
		printf("interrupt enabled, ");
	if (frame->tf_eflags & PSL_NT)
		printf("nested task, ");
	if (frame->tf_eflags & PSL_RF)
		printf("resume, ");
	if (frame->tf_eflags & PSL_VM)
		printf("vm86, ");
	printf("IOPL = %d\n", (frame->tf_eflags & PSL_IOPL) >> 12);
	printf("current process		= ");
	if (curproc) {
		printf("%lu (%s)\n",
		    (u_long)curproc->p_pid, curproc->p_comm ?
		    curproc->p_comm : "");
	} else {
		printf("Idle\n");
	}
	printf("interrupt mask		= ");
	if ((cpl & net_imask) == net_imask)
		printf("net ");
	if ((cpl & tty_imask) == tty_imask)
		printf("tty ");
	if ((cpl & bio_imask) == bio_imask)
		printf("bio ");
	if (cpl == 0)
		printf("none");
	printf("\n");

#ifdef KDB
	if (kdb_trap(&psl))
		return;
#endif
#ifdef DDB
	if (kdb_trap (type, 0, frame))
		return;
#endif
	if (type <= MAX_TRAP_MSG)
		panic(trap_msg[type]);
	else
		panic("unknown/reserved trap");
}

/*
 * Compensate for 386 brain damage (missing URKR).
 * This is a little simpler than the pagefault handler in trap() because
 * it the page tables have already been faulted in and high addresses
 * are thrown out early for other reasons.
 */
int trapwrite(addr)
	unsigned addr;
{
	struct proc *p;
	vm_offset_t va, v;
	struct vmspace *vm;
	int rv;

	va = trunc_page((vm_offset_t)addr);
	/*
	 * XXX - MAX is END.  Changed > to >= for temp. fix.
	 */
	if (va >= VM_MAXUSER_ADDRESS)
		return (1);

	p = curproc;
	vm = p->p_vmspace;

	++p->p_lock;

	if ((caddr_t)va >= vm->vm_maxsaddr
	    && (caddr_t)va < (caddr_t)USRSTACK) {
		if (!grow(p, va)) {
			--p->p_lock;
			return (1);
		}
	}

	v = trunc_page(vtopte(va));

	/*
	 * wire the pte page
	 */
	if (va < USRSTACK) {
		vm_map_pageable(&vm->vm_map, v, round_page(v+1), FALSE);
	}

	/*
	 * fault the data page
	 */
	rv = vm_fault(&vm->vm_map, va, VM_PROT_READ|VM_PROT_WRITE, FALSE);

	/*
	 * unwire the pte page
	 */
	if (va < USRSTACK) {
		vm_map_pageable(&vm->vm_map, v, round_page(v+1), TRUE);
	}

	--p->p_lock;

	if (rv != KERN_SUCCESS)
		return 1;

	return (0);
}

/*
 * syscall(frame):
 *	System call request from POSIX system call gate interface to kernel.
 * Like trap(), argument is call by reference.
 */
/*ARGSUSED*/
void
syscall(frame)
	struct trapframe frame;
{
	caddr_t params;
	int i;
	struct sysent *callp;
	struct proc *p = curproc;
	u_quad_t sticks;
	int error, opc;
	int args[8], rval[2];
	u_int code;

	sticks = p->p_sticks;
	if (ISPL(frame.tf_cs) != SEL_UPL)
		panic("syscall");

	code = frame.tf_eax;
	p->p_md.md_regs = (int *)&frame;
	params = (caddr_t)frame.tf_esp + sizeof (int) ;

	/*
	 * Reconstruct pc, assuming lcall $X,y is 7 bytes, as it is always.
	 */
	opc = frame.tf_eip - 7;
	/*
	 * Need to check if this is a 32 bit or 64 bit syscall.
	 */
	if (code == SYS_syscall) {
		/*
		 * Code is first argument, followed by actual args.
		 */
		code = fuword(params);
		params += sizeof (int);
	} else if (code == SYS___syscall) {
		/*
		 * Like syscall, but code is a quad, so as to maintain
		 * quad alignment for the rest of the arguments.
		 */
		code = fuword(params + _QUAD_LOWWORD * sizeof(int));
		params += sizeof(quad_t);
	}

 	if (p->p_sysent->sv_mask)
 		code = code & p->p_sysent->sv_mask;
 
 	if (code >= p->p_sysent->sv_size)
 		callp = &p->p_sysent->sv_table[0];
  	else
 		callp = &p->p_sysent->sv_table[code];

	if ((i = callp->sy_narg * sizeof (int)) &&
	    (error = copyin(params, (caddr_t)args, (u_int)i))) {
#ifdef KTRACE
		if (KTRPOINT(p, KTR_SYSCALL))
			ktrsyscall(p->p_tracep, code, callp->sy_narg, args);
#endif
		goto bad;
	}
#ifdef KTRACE
	if (KTRPOINT(p, KTR_SYSCALL))
		ktrsyscall(p->p_tracep, code, callp->sy_narg, args);
#endif
	rval[0] = 0;
	rval[1] = frame.tf_edx;

	error = (*callp->sy_call)(p, args, rval);

	switch (error) {

	case 0:
		/*
		 * Reinitialize proc pointer `p' as it may be different
		 * if this is a child returning from fork syscall.
		 */
		p = curproc;
		frame.tf_eax = rval[0];
		frame.tf_edx = rval[1];
		frame.tf_eflags &= ~PSL_C;	/* carry bit */
		break;

	case ERESTART:
		frame.tf_eip = opc;
		break;

	case EJUSTRETURN:
		break;

	default:
	bad:
 		if (p->p_sysent->sv_errsize)
 			if (error >= p->p_sysent->sv_errsize)
  				error = -1;	/* XXX */
   			else 
  				error = p->p_sysent->sv_errtbl[error];
		frame.tf_eax = error;
		frame.tf_eflags |= PSL_C;	/* carry bit */
		break;
	}

	userret(p, &frame, sticks);

#ifdef KTRACE
	if (KTRPOINT(p, KTR_SYSRET))
		ktrsysret(p->p_tracep, code, error, rval[0]);
#endif
}
