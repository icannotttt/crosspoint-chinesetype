/**
 * Copyright (c) 2025 Xinkerr
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Disclaimer / 免责声明
 *
 * This software is provided "as is", without warranty of any kind, express or implied, 
 * including but not limited to the warranties of merchantability, fitness for a 
 * particular purpose, or non-infringement. In no event shall the authors or copyright 
 * holders be liable for any claim, damages, or other liability, whether in an action 
 * of contract, tort, or otherwise, arising from, out of, or in connection with the 
 * software or the use or other dealings in the software.
 *
 * 本软件按“原样”提供，不附带任何明示或暗示的担保，包括但不限于对适销性、特定用途适用性
 * 或非侵权的保证。在任何情况下，作者或版权持有人均不对因本软件或使用本软件而产生的任何
 * 索赔、损害或其他责任负责，无论是合同诉讼、侵权行为还是其他情况。
 */

#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <stdlib.h>
#include "crc.h"
#include "ymodem.h"

#define ymd_log(format, ...)

//SOH NUM ~NUM PAYLOAD CRC16H CRC16L
#define SOH_PKG_SZ (1+2+128+2)
//STX NUM ~NUM PAYLOAD CRC16H CRC16L
#define STX_PKG_SZ (1+2+1024+2)

enum ymodem_code
{
    YMD_NONE = 0x00,
    YMD_SOH  = 0x01,
    YMD_STX  = 0x02,
    YMD_EOT  = 0x04,
    YMD_ACK  = 0x06,
    YMD_NAK  = 0x15,
    YMD_CODE_C = 0x43,
};

typedef enum
{
    YMD_STOP = 0,
    YMD_INITIATE,
    YMD_BEGIN,
    YMD_RECEIVE,
    YMD_END,
    YMD_ERROR
}ymodem_step_t;

//发送单字节数据
static putc_func_t data_putc = NULL;
//读取数据
static read_func_t data_read = NULL;
//文件信息帧接收回调
static ymodem_file_handler_t rec_file_cb;
//数据接收帧回调
static ymodem_data_handler_t rec_data_cb;
//结束帧回调
static ymodem_end_handler_t end_cb;
//错误回调
static ymodem_error_handler_t error_cb; 
//获取系统时间函数指针
static ym_uptime_get_t ym_uptime_get = NULL;


static uint8_t buffer[YMODEM_BUF_SIZE];
static ymodem_step_t recv_step;
//当前数据接收的偏移量
static int buf_offset;
//当前接收包的大小
static int pkg_size;
//当前有效包的包序号
static uint8_t pack_num;

//ymodem接收协议开始的时间
static uint32_t ymd_start_time;
//ymodem当前开始接收包的时间
static uint32_t ymd_rec_time;

/**
 * @brief  参数初始化
 *
 *
 * @return  void
 */
static void param_init(void)
{
    buf_offset = 0;
    pkg_size = 0;
    pack_num = 0;
    recv_step = YMD_STOP;
}

/**
 * @brief  校验帧数据
 *
 * @param[in] pack_num：包序号
 * @param[in] buf：数据地址
 *
 * @return  0：OK
 *         -2：包序号错误
 *         -3: 包序号反码校验出错
 *         -4: CRC校验出错
 */
static int complete_check(uint8_t pack_num, uint8_t* buf)
{
    uint16_t recv_crc;
    int data_size = pkg_size - 5;
//    if(buf[0] != head)
//        return -1;
    if(buf[1] != pack_num)
        return -2;
    if((buf[1] & buf[2]) != 0x00)
        return -3;
    recv_crc = (uint16_t)(*(buf + pkg_size - 2) << 8) | *(buf + pkg_size - 1);
    if(recv_crc != CRC16(buf+3, data_size))
        return -4;
    return 0;
}

/**
 * @brief  获取文件的大小
 *
 * @details 通过接收到的字符串转换数值
 *
 * @param[in] src_str：接收到的文件信息帧数据的地址
 *
 * @return 字节大小
 */
static int get_size(char* src_str)
{
    int size;
    char* ptr;
    ptr = src_str + strlen(src_str) + 1;
    size = atoi(ptr);
    return size;
}

static bool timeout_check(uint32_t start_time, uint32_t timeout)
{
    uint32_t tmp;
	
    uint32_t current_uptime = ym_uptime_get();
    if(current_uptime >= start_time)
    {
        if(current_uptime - start_time >= timeout)
        {
            return true;
        }
    }
    else
    {
        tmp = timeout - (0xffffffff-start_time);
        if(current_uptime >= tmp)
        {
            return true;
        }
    }
    return false;
}

/**
 * @brief  Ymodem协议头帧(文件信息帧)的首字节检测
 *
 * @return 0: 没有检测到
 *         1: 检测到首字节SOH
 */
static int receive_head(void)
{
    int ret = 0;
    if(data_read == NULL)
    {
        ymd_log("data_read NULL\n"); 
        return 0;
    }
    if(data_read(buffer, 1))
    {         
        if(buffer[0] == YMD_SOH)
        {
            pkg_size = SOH_PKG_SZ;
            buf_offset = 1;
            ret = 1;
        }
    }
    return ret;
}

/**
 * @brief  Ymodem协议头帧(文件信息帧)数据处理
 *
 * @return 0：未接收到正确完整的头帧数据
 *         1: 接收到头帧数据
 */
static int receive_begin(void)
{   
    int err;
    int len;
    int ret = 0;
    len = data_read(&buffer[buf_offset], SOH_PKG_SZ);
    // if(len == 0)
    //     data_putc('0');
    // else if(len < 10)
    //     data_putc('0'+len);
    // else if(len < 100)
    // {
    //     data_putc('A');
    //     data_putc('0'+ len/10);
    //     data_putc('0'+ len%10);
    // }
    // else if(len < 200)
    // {
    //     data_putc('B');
    //     data_putc('0'+ len/100);
    //     data_putc('0'+ len%100/10);
    //     data_putc('0'+ len%10);
    // }
    // else if(len < 300)
    // {
    //     data_putc('E');
    //     data_putc('2');
    // }
    // else if(len < 400)
    // {
    //     data_putc('E');
    //     data_putc('3');
    // }
    buf_offset += len;
    // data_putc('F');
    // data_putc('0'+ buf_offset/100);
    // data_putc('0'+ buf_offset%100/10);
    // data_putc('0'+ buf_offset%10);
    if(buf_offset >= SOH_PKG_SZ)
    {
        err = complete_check(0, buffer);
        if(err == 0)
        {
            int file_size = get_size((char*)(buffer+3));
            pack_num = 0;
            rec_file_cb((char*)(buffer+3), file_size); 
            ret = 1;
        }
//        else
//        {
//            ymd_log("err:%d\n", err);
//        }
        buf_offset = 0;
    }
    return ret;
}

#if YMODEM_1K_ENABLE
/**
 * @brief  Ymodem协议数据帧接收处理
 *
 * @return 0：未接收到正确完整的数据帧
 *         1: 接收到正确完整的数据帧
 *         2: 接收到EOT结束字符
 */
static int receive_block(void)
{
    int err;
    int len;
    int ret = 0;
    
    if(buf_offset == 0)
    {
        if(data_read(&buffer[0], 1) == 1)
        {
            switch(buffer[0])
            {
                case YMD_SOH:
                    pkg_size = SOH_PKG_SZ;
                    buf_offset = 1;
                    break;
                case YMD_STX:
                    pkg_size = STX_PKG_SZ; 
                    buf_offset = 1;
                    break;
                case YMD_EOT:
                    pkg_size = 0;
                    buf_offset = 0;
                    ret = 2;
                    break;
            }            
        }
    }
    else
    {
        len = data_read(&buffer[buf_offset], pkg_size-1);
        buf_offset += len;
        if(buf_offset >= pkg_size)
        {
            err = complete_check(pack_num+1, buffer);
            if(err == 0)
            {
                pack_num++;
                switch(buffer[0])
                {
                    case YMD_SOH:
                        rec_data_cb(pack_num, buffer+3, 128);
                        ret = 1;
                        break;
                    
                    case YMD_STX:
                        rec_data_cb(pack_num, buffer+3, 1024);
                        ret = 1;
                        break;
                }
            }
            buf_offset = 0;
        }
    }
    return ret;
}
#else
/**
 * @brief  Ymodem协议数据帧接收处理
 *
 * @return 0：未接收到正确完整的数据帧
 *         1: 接收到正确完整的数据帧
 *         2: 接收到EOT结束字符
 */
static int receive_block(void)
{
    int err;
    int len;
    int ret = 0;
    len = data_read(&buffer[buf_offset], SOH_PKG_SZ);
    buf_offset += len;
    
    if(buf_offset > 0)
    {
        switch(buffer[0])
        {
            case YMD_SOH:
                pkg_size = SOH_PKG_SZ;
                if(buf_offset >= SOH_PKG_SZ)
                {
                    err = complete_check(pack_num+1, buffer);
                    if(err == 0)
                    {
                        pack_num++;
                        rec_data_cb(pack_num, buffer+3, 128);
                        ret = 1;
                    }
                    buf_offset = 0;
                }
                break;
            
            case YMD_EOT:
                pkg_size = 0;
                buf_offset = 0;
                ret = 2;
                break;
        }
    }
    return ret;
}
#endif


/**
 * @brief  Ymodem协议结束帧处理
 *
 * @return 0：未接收到正确完整的结束帧
 *         1: 接收到EOT结束字符
 *         2: 接收到正确完整的结束帧
 */
static int receive_end(void)
{
    int err;
    int len;
    int ret = 0;
    if(buf_offset == 0)
    {
        if(data_read(&buffer[0], 1) == 1)
        {
            //通过首字节判断帧类型
            switch(buffer[0])
            {
                case YMD_SOH:
                    pkg_size = SOH_PKG_SZ;
                    buf_offset = 1;
                    break;

                case YMD_EOT:
                    pkg_size = 0;
                    ret = 1;
                    break;
            }
        }
    }
    else
    {
        len = data_read(&buffer[buf_offset], pkg_size-1);
        buf_offset += len;
        if(buf_offset >= SOH_PKG_SZ)
        {
            err = complete_check(0x00, buffer);
            if(err == 0)
            {
                ret = 2;
            }
            buf_offset = 0;
        }
    }
    return ret;
}

/**
 * @brief  Ymodem错误处理
 *
 * @param[in] err: 错误代码
 *
 * @return void
 */
static void ymodem_error(int err)
{
    param_init();
    //执行错误回调
    error_cb(err);
}

/**
 * @brief  Ymodem初始化
 *
 * @param[in] putc: 注册数据发送函数
 * @param[in] read: 注册读取数据函数
 * @param[in] rec_file_func: 注册文件信息帧接收回调函数
 * @param[in] rec_data_func: 注册文件数据帧接收回调函数
 * @param[in] end_func: 注册文件接收结束帧回调函数
 * @param[in] err_func: 注册ymodem接收出错回调函数
 * @param[in] uptime_get_func: 注册系统运行时间获取函数 
 *
 * @return void
 */
void ymodem_init(putc_func_t putc, read_func_t read,
                 ymodem_file_handler_t rec_file_func,
                 ymodem_data_handler_t rec_data_func,
                 ymodem_end_handler_t end_func,
                 ymodem_error_handler_t err_func,
                 ym_uptime_get_t uptime_get_func)
{
    //参数初始化
    param_init();
    
    data_putc = putc;
    data_read = read;
    rec_file_cb = rec_file_func;
    rec_data_cb = rec_data_func;
    end_cb = end_func;
    error_cb = err_func;
    ym_uptime_get = uptime_get_func;
}

/**
 * @brief  Ymodem接收端协议处理
 *
 * @return void
 */
int ymodem_recv_process(void)
{
    int result;
    if(ym_uptime_get == NULL)
        return -1;
    if(data_putc == NULL || data_read == NULL)
        return -2;
    
    switch(recv_step)
    {
        case YMD_STOP:
//            param_init();
            break;
        
        case YMD_INITIATE:
            //一定时间内没有收到SOH，则执行error退出回到STOP步骤
            if(timeout_check(ymd_start_time, YMODEM_INITIATE_TIMEOUT))
            {
                ymodem_error(-YMD_INITIATE);
                break;
            }
            result = receive_head();
            if(result)
            {
//                ymd_log("receive SOH\n");
                //接收到SOH头，进入接收头包数据步骤
                recv_step = YMD_BEGIN;
                //获取当前uptime值
                ymd_rec_time = ym_uptime_get();
            }
            else
            {
                //发送字符'C'
                data_putc(YMD_CODE_C);
            }
            break;
            
        case YMD_BEGIN:
            //一定时间内没有收到完整首包数据，则执行error退出回到STOP步骤
            if(timeout_check(ymd_rec_time, YMODEM_REC_TIMEOUT))
            {
                ymodem_error(-YMD_BEGIN);
                break;
            }
            result = receive_begin();
            if(result)
            {
                //接收到完整头包文件信息数据后，发送ACK和'C'.
                //并进入有数数据接收步骤
                data_putc(YMD_ACK);
                data_putc(YMD_CODE_C);
                recv_step = YMD_RECEIVE;
                //获取当前uptime值
                ymd_rec_time = ym_uptime_get();
            }
            break;
        
        case YMD_RECEIVE:
            //一定时间内没有收到完整的数据，则执行error退出回到STOP步骤
            if(timeout_check(ymd_rec_time, YMODEM_REC_TIMEOUT))
            {
                ymodem_error(-YMD_RECEIVE);
                break;
            }
            result = receive_block();
            if(result == 1)
            {
                //接收到完整数据后,发送ACK
                data_putc(YMD_ACK);
                //获取当前uptime值
                ymd_rec_time = ym_uptime_get();
            }
            else if(result == 2)
            {
                //接收到首个EOT后,发送NACK,并进入结束步骤
                data_putc(YMD_NAK);
                recv_step = YMD_END;
                //获取当前uptime值
                ymd_rec_time = ym_uptime_get();
            }
            break;
        
        case YMD_END:
            //一定时间内没有收到结束包数据，则执行error退出回到STOP步骤
            if(timeout_check(ymd_rec_time, YMODEM_REC_TIMEOUT))
            {
                ymodem_error(-YMD_END);
                break;
            }
            result = receive_end();
            if(result == 1)
            {
                //接收到第二个EOT,发送ACK和'C'
                data_putc(YMD_ACK);
                data_putc(YMD_CODE_C);
            }
            else if(result == 2)
            {
                //接收到结束包后，发送ACK
                data_putc(YMD_ACK);
//                recv_step = YMD_STOP;
//                ymd_log("Finish\n");
                end_cb();
                //参数初始化，回到STOP步骤
                param_init();
            }
            break;
            
        case YMD_ERROR:
            param_init();
            break;
    }
	return 0;
}

/**
 * @brief  Ymodem开始接收端
 *
 * @return void
 */
int ymodem_start(void)
{
    //进入发送'C'的请求步骤
    recv_step = YMD_INITIATE;
    if(ym_uptime_get == NULL)
        return -1;
    //获取当前uptime值
    ymd_start_time = ym_uptime_get();
    return 0;
}
