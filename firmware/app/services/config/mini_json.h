/**
 * @file mini_json.h
 * @brief 精简 JSON 解析器 —— 专为配置变更消息解析设计
 *
 *  功能：
 *    - 解析 JSON 字符串为节点树（对象/数组/字符串/数字/布尔/null）
 *    - 按字段名取值、按索引取数组元素
 *    - 支持嵌套对象和数组
 *
 *  不支持：
 *    - JSON 生成（配置回执用 snprintf 手写即可）
 *    - Unicode 转义 \uXXXX（配置里用不到）
 *    - 大整数（配置数值都在 int32/float 范围内）
 *
 *  内存：
 *    - 解析时从 FreeRTOS heap 分配节点，用 MiniJson_Free 递归释放
 *    - 单条配置 JSON 一般 < 1KB，节点数 < 50，内存占用可控
 *
 *  用法示例：
 *    mini_json_t *root = MiniJson_Parse("{\"slaves\":[{\"id\":1}]}");
 *    if (root) {
 *        mini_json_t *slaves = MiniJson_ObjectGet(root, "slaves");
 *        mini_json_t *s0 = MiniJson_ArrayGet(slaves, 0);
 *        int id = MiniJson_GetInt(s0, "id", -1);
 *        MiniJson_Free(root);
 *    }
 */
#ifndef __MINI_JSON_H__
#define __MINI_JSON_H__

#include <stdbool.h>
#include <stdint.h>

/* ===== JSON 节点类型 ===== */
typedef enum
{
    MJ_TYPE_NULL   = 0,   // null 值
    MJ_TYPE_BOOL   = 1,   // true / false
    MJ_TYPE_NUMBER = 2,   // 整数或浮点数
    MJ_TYPE_STRING = 3,   // 字符串（已去除引号，已反转义）
    MJ_TYPE_ARRAY  = 4,   // 数组 [ ... ]
    MJ_TYPE_OBJECT = 5,   // 对象 { ... }
    MJ_TYPE_INVALID= 6,   // 解析失败或未初始化
} mj_type_t;

/* ===== JSON 节点结构体 ===== */
/*
 *  解析后的每个 JSON 值（对象/数组元素/字段值）都用一个节点表示。
 *  对象和数组通过 first_child / next_sibling 组成链表：
 *    - 对象：每个 child 的 name 字段是 key，value 是值
 *    - 数组：child 的 name 为 NULL，按链表顺序就是数组下标
 */
typedef struct mini_json
{
    mj_type_t type;                 // 本节点的类型

    /* 数值类型：整数和浮点共用，解析时先尝试整数，有小数点才用浮点 */
    double num_val;                 // 数值（整数和浮点都存这里）

    /* 布尔类型 */
    bool bool_val;                  // true 或 false

    /* 字符串类型：指向解析时分配的拷贝（已去引号、已反转义） */
    char *str_val;                  // 字符串内容（NULL 结尾）

    /* 对象/数组的子节点链表 */
    struct mini_json *first_child;  // 第一个子节点（对象=第一个字段，数组=第一个元素）
    struct mini_json *last_child;   // 最后一个子节点（追加用，避免每次从头遍历）

    /* 兄弟节点链表：同一个父节点下的下一个兄弟 */
    struct mini_json *next_sibling; // 同级下一个节点

    /* 对象字段的 key 名（仅对象的 child 有，数组的 child 为 NULL） */
    char *name;                     // 对象字段名（NULL 表示是数组元素或根节点）
} mini_json_t;

/* ===== 对外 API ===== */

/**
 * @brief 解析 JSON 字符串为节点树
 * @param json_str 以 NULL 结尾的 JSON 字符串
 * @return 根节点指针；解析失败返回 NULL
 * @note 调用方负责用 MiniJson_Free 释放返回的树
 */
mini_json_t *MiniJson_Parse(const char *json_str);

/**
 * @brief 递归释放节点树（包括所有子节点和字符串）
 * @param node 要释放的根节点；传 NULL 安全
 */
void MiniJson_Free(mini_json_t *node);

/**
 * @brief 从对象中按字段名取子节点
 * @param obj 对象类型节点
 * @param key 字段名
 * @return 子节点指针；不存在或类型不对返回 NULL
 */
mini_json_t *MiniJson_ObjectGet(const mini_json_t *obj, const char *key);

/**
 * @brief 从数组中按下标取子节点
 * @param arr 数组类型节点
 * @param index 数组下标（从 0 开始）
 * @return 子节点指针；越界或类型不对返回 NULL
 */
mini_json_t *MiniJson_ArrayGet(const mini_json_t *arr, int index);

/**
 * @brief 取数组元素个数
 * @param arr 数组类型节点
 * @return 元素数量；非数组返回 0
 */
int MiniJson_ArraySize(const mini_json_t *arr);

/**
 * @brief 从对象子节点取整数值
 * @param obj 对象节点
 * @param key 字段名
 * @param default_val 字段不存在或非数值时的默认返回值
 * @return 整数值
 */
int MiniJson_GetInt(const mini_json_t *obj, const char *key, int default_val);

/**
 * @brief 从对象子节点取浮点值
 * @param obj 对象节点
 * @param key 字段名
 * @param default_val 字段不存在或非数值时的默认返回值
 * @return 浮点值
 */
double MiniJson_GetFloat(const mini_json_t *obj, const char *key, double default_val);

/**
 * @brief 从对象子节点取字符串值
 * @param obj 对象节点
 * @param key 字段名
 * @param default_val 字段不存在或非字符串时的默认返回值
 * @return 字符串指针（指向节点内部内存，不需要调用方释放）；字段不存在返回 default_val
 */
const char *MiniJson_GetString(const mini_json_t *obj, const char *key, const char *default_val);

/**
 * @brief 从对象子节点取布尔值
 * @param obj 对象节点
 * @param key 字段名
 * @param default_val 字段不存在或非布尔时的默认返回值
 * @return 布尔值
 */
bool MiniJson_GetBool(const mini_json_t *obj, const char *key, bool default_val);

#endif /* __MINI_JSON_H__ */
