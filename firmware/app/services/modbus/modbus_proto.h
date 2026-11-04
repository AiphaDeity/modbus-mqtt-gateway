#ifndef MODBUS_PROTO_H
#define MODBUS_PROTO_H
#include <stdint.h>
#include <stdbool.h>
#include "modbus_app.h"



typedef enum
{
    MB_FUNC_CODE_READ_COILS = 0x01,
    MB_FUNC_CODE_READ_DISCRETE_INPUTS = 0x02,
    MB_FUNC_CODE_READ_HOLDING_REGISTERS = 0x03,
    MB_FUNC_CODE_READ_INPUT_REGISTERS = 0x04,
    MB_FUNC_CODE_WRITE_SINGLE_COIL = 0x05,
    MB_FUNC_CODE_WRITE_SINGLE_REG = 0x06,
    MB_FUNC_CODE_WRITE_MULTIPLE_COILS = 0x0F,
    MB_FUNC_CODE_WRITE_MULTIPLE_REGISTERS = 0x10,
}FuncCode_t;

bool mb_check_crc16(uint8_t *data, uint16_t len);
uint16_t mb_crc16(uint8_t *data, uint16_t len);
uint8_t mb_find_slave_index(uint8_t slave_id);

int mb_verify_ack_length(uint8_t rx_func_code, mb_slave_runtime_t *slave, uint16_t frame_len, uint8_t *is_exception);

mb_status_e mb_build_slave_request(mb_slave_runtime_t *slave);


#endif
