#include <kernel/trap.h>
#include <kernel/panic.h>
#include <kernel/printf.h>
#include <kernel/serial.h>
#include <arch/csr.h>
#include <arch/plic.h>
#include <arch/timer.h>

/* defined in src/trap_entry.S */
extern void trap_entry();

/* the kernel currently always runs on hart 0 (no SMP support yet) */
#define BOOT_HART	0

/*
 * handle_exception(): handle a synchronous trap (exception).
 *
 * We don't expect any exceptions during normal operation of the shell, so we
 * pretty-print whatever information we can gather (this is extremely useful
 * when debugging page faults) and then panic.
 */
static void handle_exception(u64 scause, u64 stval, u64 sepc)
{
	switch (scause) {
	case EXCEPTION_INST_ACCESS_FAULT:
		error("instruction access fault at 0x%x, sepc = 0x%x\n", stval, sepc);
		break;
	case EXCEPTION_LOAD_ACCESS_FAULT:
		error("load access fault at 0x%x, sepc = 0x%x\n", stval, sepc);
		break;
	case EXCEPTION_STORE_ACCESS_FAULT:
		error("store access fault at 0x%x, sepc = 0x%x\n", stval, sepc);
		break;
	case EXCEPTION_INST_PAGE_FAULT:
		error("instruction page fault at 0x%x, sepc = 0x%x\n", stval, sepc);
		break;
	case EXCEPTION_LOAD_PAGE_FAULT:
		error("load page fault at 0x%x, sepc = 0x%x\n", stval, sepc);
		break;
	case EXCEPTION_STORE_PAGE_FAULT:
		error("store page fault at 0x%x, sepc = 0x%x\n", stval, sepc);
		break;
	default:
		error("uncaught exception! scause = 0x%x, stval = 0x%x, sepc = 0x%x\n",
		      scause, stval, sepc);
	}

	panic("unexpected exception!\n");
}

/*
 * handle_irq(): handle an asynchronous trap (interrupt).
 *
 * We forward timer interrupts to the timer subsystem, and external interrupts
 * (originating from devices, routed through the PLIC) to the matching device
 * driver after performing the PLIC claim/complete handshake.
 */
static void handle_irq(u64 scause)
{
	u32 irq;

	switch (scause) {
	case TRAP_TIMER_IRQ:
		timer_irq();
		break;
	case TRAP_EXTERNAL_IRQ:
		/* claim the interrupt from the PLIC; a 0 means we lost the
		 * race to another hart (impossible with a single hart) */
		irq = plic_hart_claim_irq(BOOT_HART);
		if (irq == IRQ_SERIAL)
			serial_irq();
		/* notify the PLIC that we're done servicing this IRQ */
		if (irq != 0)
			plic_hart_complete_irq(BOOT_HART, irq);
		break;
	default:
		error("uncaught interrupt! scause = 0x%x\n", scause);
		break;
	}
}

/*
 * handle_trap(): top-level C trap handler, called from trap_entry.S.
 *
 * Bit 63 in scause tells us whether this was an interrupt or an exception.
 */
void handle_trap()
{
	u64 scause = csr_read(CSR_SCAUSE);

	if (scause & TRAP_IRQ_BIT)
		handle_irq(scause);
	else
		handle_exception(scause, csr_read(CSR_STVAL), csr_read(CSR_SEPC));
}

void trap_setup()
{
	/* point the hart at our trap handler */
	csr_write(CSR_STVEC, (u64)trap_entry);

	/* start with all interrupt sources masked; the timer/serial setup
	 * routines will enable the bits they care about in `sie` */
	csr_write(CSR_SIE, 0);

	/* globally enable interrupts for this hart */
	hart_irq_enable();
}

void hart_irq_enable()
{
	csr_set(CSR_SSTATUS, CSR_SSTATUS_SIE);
}

void hart_irq_disable()
{
	csr_clear(CSR_SSTATUS, CSR_SSTATUS_SIE);
}

u64 hart_irq_save()
{
	/* atomically read sstatus and clear the SIE bit, returning the
	 * previous value of SIE so it can be restored later */
	u64 old = csr_read_clear(CSR_SSTATUS, CSR_SSTATUS_SIE);
	return old & CSR_SSTATUS_SIE;
}

void hart_irq_restore(u64 flags)
{
	/* only re-enable interrupts if they were enabled before */
	if (flags & CSR_SSTATUS_SIE)
		csr_set(CSR_SSTATUS, CSR_SSTATUS_SIE);
}
