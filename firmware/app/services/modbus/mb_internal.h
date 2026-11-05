/**
 * @file mb_internal.h
 * @brief Modbus 内部共享声明（仅 .c 文件 include，不对外暴露）
 *
 *  定义跨文件共享的全局变量 extern 声明和内部辅助函数。
 *  外部模块只 include modbus_app.h，不要碰这个文件。
 */
#ifndef __MB_INTERNAL_H__
#define __MB_INTERNAL_H__

#include "modbus_app.h"
#include "modbus.h"

/* ========= 跨文件共享全局（定义在 mb_poll.c）========= */
extern mb_handle_t *mb_handle;
extern modbusHandler_t hModbusMaster;

#endif /* __MB_INTERNAL_H__ */
