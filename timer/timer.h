#ifndef TIMER_H
#define TIMER_H

#include "../include/types.h"

/*
 * 定时器模块
 * ==========
 * 基于 PIT 产生周期性时钟中断，并额外维护一个“时间片耗尽”事件计数器，
 * 供进程调度器实现抢占式轮转调度。
 */

/* 以指定频率初始化 PIT 和 IRQ0 处理函数。 */
void timer_init(uint32_t frequency_hz);
/* 返回自启动以来累计的时钟 tick 数。 */
uint32_t timer_get_ticks(void);
/* 返回当前 PIT 配置频率。 */
uint32_t timer_get_frequency(void);
/* 修改轮转调度时间片，单位是 tick。 */
void timer_set_timeslice(uint32_t ticks);
/* 查询当前时间片长度。 */
uint32_t timer_get_timeslice(void);
/* 取走一个待处理的调度事件，若没有则返回 0。 */
int timer_take_schedule_event(void);
/* 仅检查是否存在待处理调度事件，不消费它。 */
int timer_has_schedule_event(void);

#endif
