/**
 * @file mini_json.c
 * @brief 精简 JSON 解析器实现 —— 专为配置变更消息解析设计
 *
 *  解析流程：
 *    1. MiniJson_Parse 入口：跳过前导空白，根据首字符分派到 parse_value
 *    2. parse_value 根据 '{' '[' '"' 数字 true/false/null 分派到对应子解析器
 *    3. parse_object 解析 {"key":value, ...}，每个字段创建一个 child 节点
 *    4. parse_array 解析 [value, value, ...]，每个元素创建一个 child 节点
 *    5. parse_string 解析 "..." 并反转义 \\ \" \n \t \r
 *    6. parse_number 解析整数或浮点（含负号、小数点、指数）
 *
 *  内存管理：
 *    - 每个节点用 pvPortMalloc 分配（FreeRTOS heap）
 *    - 字符串单独分配拷贝
 *    - MiniJson_Free 递归释放整棵树
 *
 *  容错：
 *    - 任何一步解析失败，已分配的节点会被释放，返回 NULL
 *    - 不做 schema 校验，字段类型由调用方用 GetInt/GetFloat/GetString 自行判断
 */
#include "mini_json.h"             // 本文件对应头文件
#include <string.h>                // memcpy, strlen, strncmp
#include <stdlib.h>                // strtod
#include "FreeRTOS.h"              // FreeRTOS 总头文件，pvPortMalloc/vPortFree 在这里
#include <stdio.h>                 // printf 调试用

/* ===== 内部：创建新节点 ===== */
// 分配一个空的 JSON 节点，类型初始化为 INVALID
// 返回节点指针；内存不足返回 NULL
static mini_json_t *mj_new_node(void)
{
    mini_json_t *node = (mini_json_t *)pvPortMalloc(sizeof(mini_json_t));  // 从 FreeRTOS heap 分配
    if (node)                               // 分配成功才初始化
    {
        memset(node, 0, sizeof(mini_json_t));   // 全部清零，所有指针 NULL，数值 0
        node->type = MJ_TYPE_INVALID;           // 类型初始为无效，解析成功后才改
    }
    return node;
}

/* ===== 内部：跳过空白字符 ===== */
// 跳过空格、制表符、换行、回车，返回指向第一个非空白字符的指针
// p 输入字符串指针；返回跳过空白后的指针
static const char *mj_skip_ws(const char *p)
{
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')  // 循环跳过空白
        p++;
    return p;
}

/* ===== 内部：解析字符串 ===== */
// 解析双引号包裹的字符串，处理常见转义
// p 指向开头的双引号
// out_len 输出解析后的字符串长度（不含结尾\0）
// 返回指向字符串结束后下一个字符的指针；失败返回 NULL
static const char *mj_parse_string(const char *p, char **out_str)
{
    *out_str = NULL;                        // 先置空，失败时调用方能拿到 NULL
    if (*p != '"') return NULL;            // 必须以双引号开头
    p++;                                    // 跳过开头的双引号

    /* 第一遍：计算去转义后的字符串长度 */
    int len = 0;                            // 解析后的字符计数
    const char *q = p;                      // 临时指针遍历字符串内容
    while (*q != '"' && *q != '\0')         // 遇到结尾双引号或字符串结束就停
    {
        if (*q == '\\')                     // 反斜杠转义：跳过下一个字符
        {
            q++;                            // 跳过反斜杠本身
            if (*q == '\0') return NULL;   // 反斜杠后没有字符，非法
        }
        q++;                                // 移到下一个字符
        len++;                              // 计一个字符（转义序列算一个字符）
    }
    if (*q != '"') return NULL;            // 没找到结尾双引号，非法
    q++;                                    // 跳过结尾双引号，q 现在指向字符串后面的字符

    /* 第二遍：分配内存并拷贝字符串，处理转义 */
    char *str = (char *)pvPortMalloc((size_t)len + 1);   // 长度+1 给结尾\0
    if (!str) return NULL;                 // 内存不足
    int i = 0;                             // 写入位置
    while (*p != '"' && *p != '\0')        // 再次遍历
    {
        if (*p == '\\')                    // 遇到转义
        {
            p++;                           // 跳过反斜杠
            switch (*p)                    // 根据转义字符填入对应字符
            {
                case '"':  str[i++] = '"';  break;   // 双引号
                case '\\': str[i++] = '\\'; break;   // 反斜杠
                case '/':  str[i++] = '/';  break;   // 斜杠
                case 'n':  str[i++] = '\n'; break;   // 换行
                case 't':  str[i++] = '\t'; break;   // 制表符
                case 'r':  str[i++] = '\r'; break;   // 回车
                case 'b':  str[i++] = '\b'; break;   // 退格
                case 'f':  str[i++] = '\f'; break;   // 换页
                default:   str[i++] = *p;   break;   // 未知转义，保留原字符
            }
            p++;                           // 跳过转义字符
        }
        else                               // 普通字符直接拷贝
        {
            str[i++] = *p++;
        }
    }
    str[i] = '\0';                         // 结尾\0
    *out_str = str;                         // 输出字符串指针
    return q;                               // 返回指向字符串结束后的指针
}

/* ===== 内部：解析数值 ===== */
// 解析整数或浮点数（含负号、小数点、指数）
// p 指向数值的第一个字符
// out_node 已分配好的节点，解析成功后填充 type 和 num_val
// 返回指向数值结束后下一个字符的指针；失败返回 NULL
static const char *mj_parse_number(const char *p, mini_json_t *out_node)
{
    char *endptr;                           // strtod 的输出参数，指向数值后第一个未解析的字符
    double val = strtod(p, &endptr);        // 用标准库 strtod 解析，支持负号、小数、指数
    if (endptr == p) return NULL;          // 没解析到任何数字，失败
    out_node->type = MJ_TYPE_NUMBER;        // 类型设为数值
    out_node->num_val = val;                // 存数值
    return endptr;                          // 返回数值后的指针
}

/* ===== 内部：前向声明 parse_value（对象和数组里会递归调用） ===== */
static const char *mj_parse_value(const char *p, mini_json_t *out_node);

/* ===== 内部：解析对象 { "key": value, ... } ===== */
// p 指向开头的 '{'
// out_node 已分配好的节点，解析成功后填充 type=OBJECT 和子节点链表
// 返回指向 '}' 后下一个字符的指针；失败返回 NULL
static const char *mj_parse_object(const char *p, mini_json_t *out_node)
{
    if (*p != '{') return NULL;            // 必须以 { 开头
    p++;                                    // 跳过 {
    p = mj_skip_ws(p);                      // 跳过空白
    if (*p == '}')                         // 空对象 {}
    {
        p++;                                // 跳过 }
        out_node->type = MJ_TYPE_OBJECT;    // 类型设为对象，无子节点
        return p;                           // 返回 } 后的指针
    }

    /* 循环解析每个 "key": value 对 */
    while (1)
    {
        p = mj_skip_ws(p);                  // 跳过空白
        if (*p != '"') return NULL;        // 字段名必须是字符串

        /* 解析字段名 */
        char *key = NULL;                   // 字段名字符串
        p = mj_parse_string(p, &key);       // 解析字符串
        if (!p) { vPortFree(key); return NULL; }   // 解析失败，释放已分配的字段名

        p = mj_skip_ws(p);                  // 跳过空白
        if (*p != ':')                     // 字段名后必须是冒号
        {
            vPortFree(key);                 // 释放字段名
            return NULL;                    // 没有冒号，非法
        }
        p++;                                // 跳过冒号
        p = mj_skip_ws(p);                  // 跳过冒号后的空白

        /* 创建子节点并解析值 */
        mini_json_t *child = mj_new_node(); // 为这个字段创建子节点
        if (!child) { vPortFree(key); return NULL; }   // 内存不足
        child->name = key;                  // 字段名存到子节点的 name 字段

        p = mj_parse_value(p, child);       // 递归解析字段值
        if (!p)                             // 值解析失败
        {
            MiniJson_Free(child);           // 释放子节点（包括 key）
            return NULL;
        }

        /* 把子节点追加到链表尾部 */
        if (!out_node->first_child)         // 第一个子节点
            out_node->first_child = child;
        else                                // 非第一个，追加到尾部
            out_node->last_child->next_sibling = child;
        out_node->last_child = child;       // 更新尾指针

        p = mj_skip_ws(p);                  // 跳过空白
        if (*p == ',')                     // 逗号，后面还有字段
        {
            p++;                            // 跳过逗号
            continue;                       // 继续解析下一个字段
        }
        else if (*p == '}')                // 右花括号，对象结束
        {
            p++;                            // 跳过 }
            out_node->type = MJ_TYPE_OBJECT;    // 类型设为对象
            return p;                       // 返回 } 后的指针
        }
        else                                // 既不是逗号也不是右花括号，非法
        {
            return NULL;
        }
    }
}

/* ===== 内部：解析数组 [ value, value, ... ] ===== */
// p 指向开头的 '['
// out_node 已分配好的节点，解析成功后填充 type=ARRAY 和子节点链表
// 返回指向 ']' 后下一个字符的指针；失败返回 NULL
static const char *mj_parse_array(const char *p, mini_json_t *out_node)
{
    if (*p != '[') return NULL;            // 必须以 [ 开头
    p++;                                    // 跳过 [
    p = mj_skip_ws(p);                      // 跳过空白
    if (*p == ']')                         // 空数组 []
    {
        p++;                                // 跳过 ]
        out_node->type = MJ_TYPE_ARRAY;     // 类型设为数组，无子节点
        return p;                           // 返回 ] 后的指针
    }

    /* 循环解析每个元素 */
    while (1)
    {
        p = mj_skip_ws(p);                  // 跳过空白

        /* 创建子节点并解析值 */
        mini_json_t *child = mj_new_node(); // 为这个元素创建子节点
        if (!child) return NULL;           // 内存不足

        p = mj_parse_value(p, child);       // 递归解析元素值
        if (!p)                             // 值解析失败
        {
            MiniJson_Free(child);           // 释放子节点
            return NULL;
        }

        /* 把子节点追加到链表尾部（数组元素没有 name） */
        if (!out_node->first_child)         // 第一个元素
            out_node->first_child = child;
        else                                // 非第一个，追加到尾部
            out_node->last_child->next_sibling = child;
        out_node->last_child = child;       // 更新尾指针

        p = mj_skip_ws(p);                  // 跳过空白
        if (*p == ',')                     // 逗号，后面还有元素
        {
            p++;                            // 跳过逗号
            continue;                       // 继续解析下一个元素
        }
        else if (*p == ']')                // 右方括号，数组结束
        {
            p++;                            // 跳过 ]
            out_node->type = MJ_TYPE_ARRAY;     // 类型设为数组
            return p;                       // 返回 ] 后的指针
        }
        else                                // 既不是逗号也不是右方括号，非法
        {
            return NULL;
        }
    }
}

/* ===== 内部：解析一个 JSON 值（根据首字符分派） ===== */
// p 指向值的第一个非空白字符
// out_node 已分配好的节点，解析成功后填充类型和值
// 返回指向值结束后下一个字符的指针；失败返回 NULL
static const char *mj_parse_value(const char *p, mini_json_t *out_node)
{
    p = mj_skip_ws(p);                      // 跳过前导空白

    switch (*p)                             // 根据首字符判断类型
    {
        case '{':                           // 对象
            return mj_parse_object(p, out_node);

        case '[':                           // 数组
            return mj_parse_array(p, out_node);

        case '"':                           // 字符串
        {
            char *str = NULL;               // 字符串内容
            const char *next = mj_parse_string(p, &str);    // 解析字符串
            if (!next) return NULL;        // 失败
            out_node->type = MJ_TYPE_STRING;    // 类型设为字符串
            out_node->str_val = str;           // 存字符串指针
            return next;                       // 返回字符串后的指针
        }

        case 't':                           // 可能是 true
            if (strncmp(p, "true", 4) == 0)     // 匹配 true
            {
                out_node->type = MJ_TYPE_BOOL; // 类型设为布尔
                out_node->bool_val = true;     // 值为 true
                return p + 4;                  // 跳过 4 个字符
            }
            return NULL;                    // 不是 true，非法

        case 'f':                           // 可能是 false
            if (strncmp(p, "false", 5) == 0)    // 匹配 false
            {
                out_node->type = MJ_TYPE_BOOL; // 类型设为布尔
                out_node->bool_val = false;    // 值为 false
                return p + 5;                  // 跳过 5 个字符
            }
            return NULL;                    // 不是 false，非法

        case 'n':                           // 可能是 null
            if (strncmp(p, "null", 4) == 0)     // 匹配 null
            {
                out_node->type = MJ_TYPE_NULL; // 类型设为 null
                return p + 4;                  // 跳过 4 个字符
            }
            return NULL;                    // 不是 null，非法

        default:                            // 其他：尝试解析数值
            /* 数值以 - 或 0~9 开头 */
            if (*p == '-' || (*p >= '0' && *p <= '9'))
                return mj_parse_number(p, out_node);   // 解析数值
            return NULL;                    // 不是任何合法的 JSON 值，失败
    }
}

/* ===== 对外：解析 JSON 字符串 ===== */
mini_json_t *MiniJson_Parse(const char *json_str)
{
    if (!json_str || !*json_str) return NULL;   // 空指针或空字符串，直接返回 NULL

    mini_json_t *root = mj_new_node();      // 创建根节点
    if (!root) return NULL;                // 内存不足

    const char *p = mj_parse_value(json_str, root);   // 从头开始解析
    if (!p)                                 // 解析失败
    {
        MiniJson_Free(root);               // 释放已分配的根节点
        return NULL;                        // 返回 NULL 表示失败
    }

    /* 解析成功后检查是否还有非空白尾随内容（宽松模式：允许尾部空白） */
    p = mj_skip_ws(p);                      // 跳过尾部空白
    if (*p != '\0')                        // 还有非空白内容，JSON 格式有问题
    {
        /* 宽松处理：只要第一个值解析成功就返回，不强制要求整个字符串都解析完 */
        /* 这样可以处理 JSON 后面有 \r\n 等情况 */
    }

    return root;                            // 返回解析成功的根节点
}

/* ===== 对外：递归释放节点树 ===== */
void MiniJson_Free(mini_json_t *node)
{
    if (!node) return;                      // 空指针安全返回

    /* 先递归释放所有子节点 */
    mini_json_t *child = node->first_child; // 从第一个子节点开始
    while (child)                           // 遍历子节点链表
    {
        mini_json_t *next = child->next_sibling;    // 先保存下一个兄弟的指针
        MiniJson_Free(child);              // 递归释放这个子节点（会释放它的子节点和字符串）
        child = next;                      // 移到下一个兄弟
    }

    /* 再释放本节点自己的字符串和字段名 */
    if (node->str_val) vPortFree(node->str_val);   // 释放字符串值
    if (node->name) vPortFree(node->name);          // 释放字段名

    /* 最后释放节点本身 */
    vPortFree(node);
}

/* ===== 对外：从对象中按字段名取子节点 ===== */
mini_json_t *MiniJson_ObjectGet(const mini_json_t *obj, const char *key)
{
    if (!obj || !key) return NULL;         // 参数校验
    if (obj->type != MJ_TYPE_OBJECT) return NULL;   // 不是对象类型，无法取字段

    /* 遍历子节点链表，找 name 匹配的字段 */
    mini_json_t *child = obj->first_child;  // 从第一个子节点开始
    while (child)                           // 遍历
    {
        if (child->name && strcmp(child->name, key) == 0)   // 字段名匹配
            return child;                   // 找到了，返回该子节点
        child = child->next_sibling;        // 没找到，继续下一个
    }
    return NULL;                            // 没找到，返回 NULL
}

/* ===== 对外：从数组中按下标取子节点 ===== */
mini_json_t *MiniJson_ArrayGet(const mini_json_t *arr, int index)
{
    if (!arr) return NULL;                 // 参数校验
    if (arr->type != MJ_TYPE_ARRAY) return NULL;   // 不是数组类型，无法取元素

    /* 遍历子节点链表，数到第 index 个 */
    int i = 0;                              // 当前下标
    mini_json_t *child = arr->first_child;  // 从第一个元素开始
    while (child)                           // 遍历
    {
        if (i == index) return child;      // 下标匹配，返回该元素
        i++;                                // 下标加1
        child = child->next_sibling;        // 继续下一个
    }
    return NULL;                            // 越界，返回 NULL
}

/* ===== 对外：取数组元素个数 ===== */
int MiniJson_ArraySize(const mini_json_t *arr)
{
    if (!arr || arr->type != MJ_TYPE_ARRAY) return 0;  // 非数组返回 0

    int count = 0;                          // 计数器
    mini_json_t *child = arr->first_child;  // 从第一个元素开始
    while (child)                           // 遍历链表
    {
        count++;                            // 计数
        child = child->next_sibling;        // 下一个
    }
    return count;                           // 返回总数
}

/* ===== 对外：从对象子节点取整数值 ===== */
int MiniJson_GetInt(const mini_json_t *obj, const char *key, int default_val)
{
    mini_json_t *child = MiniJson_ObjectGet(obj, key);   // 按字段名取子节点
    if (!child || child->type != MJ_TYPE_NUMBER) return default_val;   // 不存在或非数值，返回默认值
    return (int)child->num_val;            // 返回整数值（截断小数部分）
}

/* ===== 对外：从对象子节点取浮点值 ===== */
double MiniJson_GetFloat(const mini_json_t *obj, const char *key, double default_val)
{
    mini_json_t *child = MiniJson_ObjectGet(obj, key);   // 按字段名取子节点
    if (!child || child->type != MJ_TYPE_NUMBER) return default_val;   // 不存在或非数值，返回默认值
    return child->num_val;                 // 返回浮点值
}

/* ===== 对外：从对象子节点取字符串值 ===== */
const char *MiniJson_GetString(const mini_json_t *obj, const char *key, const char *default_val)
{
    mini_json_t *child = MiniJson_ObjectGet(obj, key);   // 按字段名取子节点
    if (!child || child->type != MJ_TYPE_STRING) return default_val;   // 不存在或非字符串，返回默认值
    return child->str_val;                 // 返回字符串指针（指向节点内部内存）
}

/* ===== 对外：从对象子节点取布尔值 ===== */
bool MiniJson_GetBool(const mini_json_t *obj, const char *key, bool default_val)
{
    mini_json_t *child = MiniJson_ObjectGet(obj, key);   // 按字段名取子节点
    if (!child || child->type != MJ_TYPE_BOOL) return default_val;   // 不存在或非布尔，返回默认值
    return child->bool_val;                // 返回布尔值
}
