#include "modbus_proto.h"
#include "modbus_app.h"
#include <string.h>
#include "cmsis_os2.h"

extern mb_handle_t *mb_handle;

/*
 * @brief 数据解析
 * @param len 数据长度
 * @return mb_status_e 解析状态
*/
mb_status_e mb_parse_frame(mb_frame_slot_t *slot)
{
    // 先判断槽本身是否有效
    if (slot->valid == 0)
    {
        return MB_PARSE_STATUS_ERR;
    }

    // Modbus RTU最小帧长： addr(1)+func(1)+crc(2) = 4
    if(slot->frame_len < 4)
    {
        // 数据长度不足，直接返回错误
        //printf("Modbus frame length error: %d\n", slot->frame_len);
        // 标记为无效
        slot->valid = 0;
        return MB_PARSE_STATUS_ERR;
    }

    // CRC校验
    if(!mb_check_crc16(slot->data, slot->frame_len))
    {
        return MB_PARSE_STATUS_ERR;
    }

    // 这里只能做一些简单的防御检查，比如功能码范围过滤
    uint8_t slave_id = slot->data[0];
    uint8_t func_code = slot->data[1];

    //判断从站id是否有效,返回从站索引或-1表示未找到
    uint8_t slave_index = mb_find_slave_index(slave_id);
    if(slave_index == -1)   
    {
        return MB_PARSE_STATUS_ERR;
    }

    //根据从站索引获取从站指针
    mb_slave_runtime_t *slave = &mb_handle->slave_list[slave_index];

    
    //判断func_code是否有效(发送时应已校验)
    
    //校验应答长度
    uint8_t is_exception = 0;
    int ret = mb_verify_ack_length(func_code, slave, slot->frame_len, &is_exception);
    if(ret != 0)
    {
        return MB_PARSE_STATUS_ERR;
    }
    
    //将数据解析到寄存器缓存
    memcpy(slave->reg_data_buf, slot->data + 2, slot->frame_len - 2);



    return MB_PARSE_STATUS_ERR;
}


/*
 * @brief 校验CRC
 * @param data 数据指针
 * @param len 数据长度
 * @return bool 校验结果
*/
bool mb_check_crc16(uint8_t *data, uint16_t len)
{
    uint16_t crc = mb_crc16(data, len - 2);
    if(crc != (uint16_t)(data[len - 2] << 8 | data[len - 1]))
    {
        return false;
    }
    return true;
}

/*
CRC校验
*/
uint16_t mb_crc16(uint8_t *data, uint16_t len)
{
    uint16_t crc = 0;
    for(uint16_t i = 0; i < len; i++)
    {
        crc += data[i];
    }
    return crc;
}


/*
 * @brief 查找从站
 * @param slave_id 从站ID
 * @return uint8_t 从站索引或-1表示未找到
*/
uint8_t mb_find_slave_index(uint8_t slave_id)
{
    for(uint8_t i = 0; i < mb_handle->slave_actual_num; i++)
    {
        if(mb_handle->slave_list[i].valid == 1 && mb_handle->slave_list[i].cfg.slave_id == slave_id)
        {
            return i;
        }
    }
    return -1;
}

/**
 * @brief 校验Modbus应答帧长度，只校验帧本身格式合法性，不校验是否匹配本次下发请求
 * @param rx_func_code 收到报文里的功能码（可能带0x80异常位）
 * @param slave        从站 runtime，取 cfg.quantity
 * @param frame_len    当前收到帧总长度
 * @param is_exception [out] 1=异常应答，0=正常应答
 * @return 1 长度合法；0 长度不匹配；-1 不支持该功能码
 */
int mb_verify_ack_length(uint8_t rx_func_code, mb_slave_runtime_t *slave, uint16_t frame_len, uint8_t *is_exception)
{
    *is_exception = 0;

    // 异常应答：最高位为1，固定4字节
    if (rx_func_code & 0x80U)
    {
        *is_exception = 1;
        return (frame_len == 4U) ? 1 : 0;
    }

    uint16_t quantity = slave->cfg.reg_cnt;
    uint16_t pdu_len = 0;

    switch (rx_func_code)
    {
        case MB_FUNC_CODE_READ_COILS:
        case MB_FUNC_CODE_READ_DISCRETE_INPUTS:
        {
            uint16_t byte_cnt = (quantity + 7U) >> 3U;
            pdu_len = 1U + byte_cnt;
            break;
        }
        case MB_FUNC_CODE_READ_HOLDING_REGISTERS:
        case MB_FUNC_CODE_READ_INPUT_REGISTERS:
        {
            uint16_t byte_cnt = quantity * 2U;
            pdu_len = 1U + byte_cnt;
            break;
        }
        case MB_FUNC_CODE_WRITE_SINGLE_COIL:
        case MB_FUNC_CODE_WRITE_SINGLE_REG:
        case MB_FUNC_CODE_WRITE_MULTIPLE_COILS:
        case MB_FUNC_CODE_WRITE_MULTIPLE_REGISTERS:
        {
            pdu_len = 4U;
            break;
        }
        default:
            return -1;
    }

    uint16_t expect_len = pdu_len + 4U;
    return (frame_len == expect_len) ? 1 : 0;
}



/**
 * @brief 根据slave的cfg，预生成读请求帧（01/02/03/04），存入slave->tx_req_buf
 * @param slave 从站运行时对象
 * @retval MB_PARSE_STATUS_OK 成功
 */
mb_status_e mb_build_slave_request(mb_slave_runtime_t *slave)
{
    if (slave == NULL || slave->valid == 0)
    {
        return MB_PARSE_STATUS_ERR;
    }

    uint8_t *tx_buf = slave->tx_req_buf;
    uint16_t len = 0;

    tx_buf[len++] = slave->cfg.slave_id;
    tx_buf[len++] = slave->cfg.func_code;
    tx_buf[len++] = (slave->cfg.reg_start_addr >> 8) & 0xFF;
    tx_buf[len++] = slave->cfg.reg_start_addr & 0xFF;
    tx_buf[len++] = (slave->cfg.reg_cnt >> 8) & 0xFF;
    tx_buf[len++] = slave->cfg.reg_cnt & 0xFF;

    uint16_t crc = mb_crc16(tx_buf, len);
    tx_buf[len++] = crc & 0xFF;
    tx_buf[len++] = (crc >> 8) & 0xFF;

    slave->tx_req_len = len;
    return MB_STATUS_OK;
}



