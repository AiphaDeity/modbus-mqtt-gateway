#ifndef UPGRADE_BASELINE_H
#define UPGRADE_BASELINE_H

// Boot、App和工具统一使用持久版本2，不转换其他版本的记录。
#ifndef GATEWAY_UPGRADE_PERSISTENCE_VERSION
#define GATEWAY_UPGRADE_PERSISTENCE_VERSION 2
#endif

#if GATEWAY_UPGRADE_PERSISTENCE_VERSION != 2
#error "Only upgrade persistence version 2 is supported"
#endif

#define UPGRADE_PERSISTENCE_VERSION GATEWAY_UPGRADE_PERSISTENCE_VERSION
// 最大试运行放行次数，包含安装后的首次；每次放行前持久计数，不是首次之外再重试两次。
#define UPGRADE_TRIAL_LIMIT 2U

#endif
