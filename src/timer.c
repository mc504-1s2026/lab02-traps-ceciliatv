#include <arch/timer.h>
#include <arch/csr.h>
#include <arch/spinlock.h>

/*
 * A small alarm subsystem built on top of the RISC-V timer interrupt.
 *
 * The hart has a single monotonic counter (`time`) and a single comparator
 * (`stimecmp`): a timer interrupt fires whenever `time >= stimecmp`. To support
 * multiple outstanding alarms we keep an array of deadlines and always program
 * `stimecmp` to the nearest one. When that deadline is reached the timer IRQ
 * marks every expired alarm as fired and reprograms `stimecmp` to the next
 * pending deadline (or "never" if there are none left).
 */

#define MAX_ALARMS	16
/* a comparator value far enough in the future that it never fires in practice */
#define TIMER_NEVER	(~0UL)

struct alarm {
	bool active;
	u64 deadline;	/* value of `time` at which this alarm expires */
};

static struct alarm alarms[MAX_ALARMS];
/* number of alarms that have expired and not yet been consumed by the shell */
static u32 alarms_fired;
/* protects `alarms` and `alarms_fired`, which are shared between the timer
 * interrupt handler and the main application loop */
static struct spinlock timer_lock;

u64 timer_read()
{
	return csr_read(CSR_TIME);
}

/* find the nearest pending deadline; caller must hold timer_lock */
static u64 nearest_deadline()
{
	u64 next = TIMER_NEVER;
	for (size_t i = 0; i < MAX_ALARMS; i++) {
		if (alarms[i].active && alarms[i].deadline < next)
			next = alarms[i].deadline;
	}
	return next;
}

void timer_irq_enable()
{
	spin_init(&timer_lock);

	/* Make sure the comparator is set to "never" before unmasking the
	 * timer interrupt: on boot `stimecmp` reads as 0 while `time` is
	 * already > 0, which would otherwise trigger an immediate (and
	 * perpetual) timer interrupt storm. */
	csr_write(CSR_STIMECMP, TIMER_NEVER);

	/* unmask the supervisor timer interrupt (sie[STIE]) */
	csr_set(CSR_SIE, CSR_SIE_STIE);
}

void timer_irq_disable()
{
	csr_clear(CSR_SIE, CSR_SIE_STIE);
}

void timer_set_alarm(u64 secs)
{
	u64 deadline = timer_read() + secs * TIMER_FREQ;
	u64 flags = spin_lock_irqsave(&timer_lock);

	for (size_t i = 0; i < MAX_ALARMS; i++) {
		if (!alarms[i].active) {
			alarms[i].active = true;
			alarms[i].deadline = deadline;
			break;
		}
	}

	/* reprogram the comparator to the nearest pending deadline */
	csr_write(CSR_STIMECMP, nearest_deadline());
	spin_unlock_irqrestore(&timer_lock, flags);
}

void timer_irq()
{
	u64 now = timer_read();

	/* we run in interrupt context with interrupts already disabled, so a
	 * plain spin_lock is enough (we can't be preempted by ourselves) */
	spin_lock(&timer_lock);
	for (size_t i = 0; i < MAX_ALARMS; i++) {
		if (alarms[i].active && alarms[i].deadline <= now) {
			alarms[i].active = false;
			alarms_fired++;
		}
	}

	/* rearm for the next pending alarm (or disable by setting "never") */
	csr_write(CSR_STIMECMP, nearest_deadline());
	spin_unlock(&timer_lock);
}

u32 timer_poll_alarms()
{
	u64 flags = spin_lock_irqsave(&timer_lock);
	u32 fired = alarms_fired;
	alarms_fired = 0;
	spin_unlock_irqrestore(&timer_lock, flags);
	return fired;
}
