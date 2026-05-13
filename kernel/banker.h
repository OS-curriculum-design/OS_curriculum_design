#ifndef BANKER_H
#define BANKER_H

#include "../include/types.h"

#define BANKER_RESOURCE_COUNT 3
#define BANKER_MAX_PROCESSES 8

#define BANKER_OK 1
#define BANKER_ERR_NOT_READY 0
#define BANKER_ERR_NO_SLOT -1
#define BANKER_ERR_NO_PROCESS -2
#define BANKER_ERR_OVER_NEED -3
#define BANKER_ERR_OVER_AVAILABLE -4
#define BANKER_ERR_UNSAFE -5
#define BANKER_ERR_OVER_ALLOCATED -6

/* 初始化银行家算法的可用资源向量 Available。 */
void banker_init(const uint32_t* available);
/* 登记某个进程的最大需求 Max，Allocation 初始为 0，Need 初始等于 Max。 */
int banker_register_process(int pid, const uint32_t* max);
/* 进程结束或被撤销时注销登记，并把已分配资源归还到 Available。 */
void banker_unregister_process(int pid);
/* 按银行家算法尝试资源请求；只有安全状态才真正提交分配。 */
int banker_request(int pid, const uint32_t* request);
/* 释放某个进程已经持有的一部分资源。 */
int banker_release(int pid, const uint32_t* release);
/* 打印当前 Available / Max / Allocation / Need 表。 */
void banker_print_state(void);

#endif
