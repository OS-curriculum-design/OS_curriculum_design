#ifndef TIMER_H
#define TIMER_H

#include "../include/types.h"

void timer_init(uint32_t frequency_hz);
uint32_t timer_get_ticks(void);
uint32_t timer_get_frequency(void);
void timer_set_timeslice(uint32_t ticks);
uint32_t timer_get_timeslice(void);
int timer_take_schedule_event(void);
int timer_has_schedule_event(void);

#endif
