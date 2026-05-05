/**
 * Copyright (c) 2025 Xinkerr
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef __YMODEM_H__
#define __YMODEM_H__

#ifdef __cplusplus
extern "C" {
#endif

/******************************************CONFIG***************************************************/
#define YMODEM_BUF_SIZE                 150
#define YMODEM_INITIATE_TIMEOUT         30000
#define YMODEM_REC_TIMEOUT              1200
#define YMODEM_1K_ENABLE                0
/***************************************************************************************************/

#include <stdint.h>

typedef void (*putc_func_t)(uint8_t put_data);
typedef int (*read_func_t)(uint8_t* pdata, int len);
typedef void (*ymodem_file_handler_t)(char* file, int size);
typedef void (*ymodem_data_handler_t)(uint8_t num, uint8_t* pdata, int len);
typedef void (*ymodem_end_handler_t)(void);
typedef void (*ymodem_error_handler_t)(int err);
typedef uint32_t (*ym_uptime_get_t)(void);

void ymodem_init(putc_func_t putc, read_func_t read,
                 ymodem_file_handler_t rec_file_func,
                 ymodem_data_handler_t rec_data_func,
                 ymodem_end_handler_t end_func,
                 ymodem_error_handler_t err_func,
                 ym_uptime_get_t uptime_get_func);

int ymodem_recv_process(void);
int ymodem_start(void);

#ifdef __cplusplus
}
#endif

#endif