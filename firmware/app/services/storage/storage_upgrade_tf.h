#ifndef STORAGE_UPGRADE_TF_H
#define STORAGE_UPGRADE_TF_H

#include <stdbool.h>

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
