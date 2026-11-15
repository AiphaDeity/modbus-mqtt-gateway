#ifndef STORAGE_UPGRADE_TF_H
#define STORAGE_UPGRADE_TF_H

#include <stdbool.h>
#include "storage_config.h"

#define STORAGE_TF_CONFIG_PATH "/config.json"
#define STORAGE_TF_CONFIG_FILE 1U
#define STORAGE_TF_FIRMWARE_FILE 2U

/* 仅Storage_Task调用，复用原文件对象和磁盘回调，不增文件队列或任务。 */
HAL_StatusTypeDef storage_config_tf(storage_config_message_t *message, uint32_t deadline_ms);

/* Storage_Init创建静态文件服务队列；Storage_Task就绪后绑定UpgradeTfSource。 */
bool storage_upgrade_tf_init(void);
void storage_upgrade_tf_bind(void);

/* Storage_Task每轮处理至多一个文件请求，所有FatFs调用留在存储任务。 */
void storage_upgrade_tf_process(void);
/* Storage_Task结算查询，租约未关闭时不允许安装。 */
bool storage_upgrade_tf_is_open(void);

/* 任一插拔事件均使租约失败；unmounted=true表示卷已卸载/重挂，可丢弃旧FIL。 */
void storage_upgrade_tf_invalidate(bool unmounted);

#endif /* STORAGE_UPGRADE_TF_H */
