/*
 * fiwix/include/fiwix/timer.h
 *
 * Copyright 2018, Jordi Sanfeliu. All rights reserved.
 * Distributed under the terms of the Fiwix License.
 */

#ifndef _FIWIX_TIMER_H
#define _FIWIX_TIMER_H

#include <fiwix/types.h>
#include <fiwix/sigcontext.h>

#define TIMER_IRQ	0
#define HZ		100	/* kernel's Hertz rate (100 = 10ms) */
#define TICK		(1000000 / HZ)

#define UNIX_EPOCH	1970

#define LEAP_YEAR(y)	((y % 4) == 0 && ((y % 100) != 0 || (y % 400) == 0))
#define DAYS_PER_YEAR(y)	((LEAP_YEAR(y)) ? 366 : 365)

#define SECS_PER_MIN	60
#define SECS_PER_HOUR	(SECS_PER_MIN * 60)
#define SECS_PER_DAY	(SECS_PER_HOUR * 24)

#define INFINITE_WAIT	0xFFFFFFFF

struct callout {
	int expires;
	void (*fn)(unsigned int);
	unsigned int arg;
	struct callout *next;
};

struct callout_req {
	void (*fn)(unsigned int);
	unsigned int arg;
};

void add_callout(struct callout_req *, unsigned int);
void del_callout(struct callout_req *);
void irq_timer(int, struct sigcontext *);
void irq_timer_bh(void);
void do_callouts_bh(void);
void get_system_time(void);
void set_system_time(__time_t);
int gettimeoffset(void);
void timer_init(void);

#endif /* _FIWIX_TIMER_H */
