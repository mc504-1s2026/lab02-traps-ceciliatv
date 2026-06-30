#ifndef __TICK_H__
#define __TICK_H__

#include <kernel/types.h>

#define TIMER_FREQ 10000000

void timer_irq_enable();
void timer_irq_disable();
void timer_irq();
u64 timer_read();

/*
 * timer_set_alarm(): schedule an alarm to fire `secs` seconds from now.
 *
 * When the alarm expires, a timer interrupt is generated and the alarm is
 * accounted for; callers can find out how many alarms have expired since the
 * last check using timer_poll_alarms().
 */
void timer_set_alarm(u64 secs);

/*
 * timer_poll_alarms(): return (and reset) the number of alarms that have
 * expired since the last call. This is meant to be polled from the main
 * application loop (e.g. the shell).
 */
u32 timer_poll_alarms();

#endif
