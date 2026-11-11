#ifndef STORAGE_UPGRADE_H
#define STORAGE_UPGRADE_H

#include "upgrade_storage_protocol.h"
#include <stdbool.h>

/* 仅Storage_Task调用。初始化只登记介质状态和所有者，不擦写或重建旧日志。 */
void storage_upgrade_init(bool flash_ready);

/* 同步处理一个值拷贝请求；应答保留req_id/type/session/tx_id和底层detail。
 * 所有I/O使用请求的HAL绝对期限；失败不撤销已发出的介质命令。
 * 函数返回后不再引用req/rsp。 */
void storage_upgrade_handle(const UpgradeStorageRequest *req, UpgradeStorageResponse *rsp);

#endif /* STORAGE_UPGRADE_H */
