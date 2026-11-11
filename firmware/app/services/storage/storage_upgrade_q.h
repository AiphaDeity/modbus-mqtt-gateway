#ifndef STORAGE_UPGRADE_Q_H
#define STORAGE_UPGRADE_Q_H

#include "cmsis_os2.h"
#include <stdbool.h>
#include <stdint.h>

/* Storage_Init创建静态值拷贝队列；重复调用不重置在途消息。 */
bool storage_upgrade_q_init(void);
osMessageQueueId_t storage_upgrade_req_queue(void);
osMessageQueueId_t storage_upgrade_rsp_queue(void);

/* Storage_Task每轮处理至多一个请求；wait_ticks单位为RTOS tick。 */
void storage_upgrade_q_process(uint32_t wait_ticks);

#endif /* STORAGE_UPGRADE_Q_H */
