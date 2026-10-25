#include "Self_aim.h"
#include "main.h"
#include "usart.h"
#include "CRC8_CRC16.h"
#include "string.h"
#include "cmsis_os.h"
#include "gimbal_task.h"
#include "INS_task.h"
#include "usbd_cdc_if.h"
#include "CAN_receive.h"
#include "video_forward.h"
uint8_t buffer[sizeof(RECEIVE_DATA)];//用于存储完整的数据包
uint8_t Usart_Receive[40]; //用于接收单个字节的数据
uint16_t received = 0;//当前接收到的数据长度
uint8_t Count=0;//接收状态标签
InputData inputdata;
SEND_DATA Send_Data;
extern __IO uint32_t uwTick;
uint32_t sendTick =0;
gimbal_control_t *gimbal_data;

/* ===== 10-24 计数器：看清 USB 流里到底有什么 =====
 * g_parse_sof_seen : 找帧头状态下遇到 0xA5 的次数 -> 视频帧真的来了几帧
 * g_parse_ff_seen  : 找帧头状态下遇到 0xFF 的次数 -> 控制帧发了多少
 * g_parse_ctrl_ok  : 收满 32 字节且帧尾 0x0D 对的控制帧
 * 三个都是 volatile，Watch 窗口直接加名字就能看，不用函数调用。 */
volatile uint32_t g_parse_sof_seen = 0u;
volatile uint32_t g_parse_ff_seen  = 0u;
volatile uint32_t g_parse_ctrl_ok  = 0u;

// uint32_t usb_rx_length = 0;


/* ================== 单一状态机：一段字节只解析一次 ==================
 * 帧类型由帧头决定，同一字节只会进入一个解析器，不会互相污染：
 *   0xA5 -> 视频帧，收满 309 字节 -> 校验 CRC -> 入队转发 VT03
 *   0xFF -> 控制帧，收满 32 字节 -> 校验帧尾 0x0D -> 写入 inputdata
 *
 * 旧实现让两个独立解析器同时扫描同一份数据，视频载荷里随机的 0xFF
 * 会被控制解析器当成帧头，实测 18.6% 的控制帧是垃圾（云台疯转根因之一）。
 * ================================================================= */
#define CONTROL_FRAME_BYTES   ((uint16_t)sizeof(RECEIVE_DATA))
#define VIDEO_FRAME_BYTES     309u
#define VIDEO_SOF             0xA5u
#define CONTROL_TAIL          0x0D

void process_usb_data_packet(uint8_t* data, uint16_t length)
{
    static uint8_t  buf[VIDEO_FRAME_BYTES];   /* 最大帧 309，两路共用 */
    static uint16_t idx = 0;
    static uint8_t  kind = 0;                 /* 0=找帧头 1=视频 2=控制 */

    if (data == 0) return;

    for (uint16_t i = 0; i < length; i++) {
        uint8_t b = data[i];

        if (kind == 0) {
            if (b == VIDEO_SOF) {
                g_parse_sof_seen++;
                buf[0] = b; idx = 1u; kind = 1u;
            } else if (b == FRAME_HEADER_FF) {
                g_parse_ff_seen++;
                buf[0] = b; idx = 1u; kind = 2u;
            }
            continue;
        }

        if (idx < VIDEO_FRAME_BYTES) {
            buf[idx++] = b;
        } else {
            idx = VIDEO_FRAME_BYTES;          /* 防御：不应发生 */
        }

        if (kind == 1u) {
            if (idx >= VIDEO_FRAME_BYTES) {
                kind = 0; idx = 0;
                (void)VideoForward_PushFrame(buf);
            }
        } else {
            if (idx >= CONTROL_FRAME_BYTES) {
                kind = 0; idx = 0;
                /* 帧尾校验：挡掉错位帧，宁可不更新也不要用垃圾角度 */
                if (buf[CONTROL_FRAME_BYTES - 1u] == CONTROL_TAIL) {
                    g_parse_ctrl_ok++;
                    memcpy(&inputdata, buf + sizeof(FrameHeader), sizeof(InputData));
                }
            }
        }
    }
}

/* USB CDC 回调入口：所有字节交给单一状态机分流 */
void usbd_cdc_receive_handle(uint8_t *data, uint16_t length)
{
    VideoForward_NoteUsbBytes(length);
    process_usb_data_packet(data, length);
}

void data_transition(void)
{
    // uint8_t enemy_color;
    // get_enemy_color(&enemy_color);
    // Send_Data.frame_header.s = FRAME_HEADER_S; 
    // Send_Data.frame_header.p = FRAME_HEADER_P;
    // gimbal_data = get_gimbal_data();
    // Send_Data.output_data.mode = aimflag;
    // Send_Data.output_data.q[0] = INS.q[0];
    // Send_Data.output_data.q[1] = INS.q[1];
    // Send_Data.output_data.q[2] = INS.q[2];
    // Send_Data.output_data.q[3] = INS.q[3];
    // Send_Data.output_data.yaw = gimbal_data->gimbal_yaw_motor.absolute_angle / 180 * 3.1415926f;
    // Send_Data.output_data.yaw_vel = 0;
    // Send_Data.output_data.pitch = gimbal_data->gimbal_pitch_motor.absolute_angle;
    // Send_Data.output_data.pitch_vel = 0;
    // if(shoot_bullet_speed != 0)
    // {
    //     Send_Data.output_data.bullet_speed = shoot_bullet_speed;
    // }
    // else {Send_Data.output_data.bullet_speed = 19.9;}


    // uint8_t *data_ptr = (uint8_t *)&Send_Data;                                    
    // uint32_t data_length = sizeof(SEND_DATA) - sizeof(Send_Data.frame_tailer.crc16); 
    // Send_Data.frame_tailer.crc16 = get_CRC16_check_sum(data_ptr, data_length, FRAME_TAILER_CRC16_init);



    uint8_t enemy_color;
    get_enemy_color(&enemy_color);
    Send_Data.frame_header.ff = FRAME_HEADER_FF; 
    gimbal_data = get_gimbal_data();
    Send_Data.output_data.mode = 4;
    Send_Data.output_data.roll = 0;
    Send_Data.output_data.yaw = gimbal_data->gimbal_yaw_motor.absolute_angle;
    Send_Data.output_data.pitch = gimbal_data->gimbal_pitch_motor.absolute_angle;
    Send_Data.frame_tailer.crc8 = 0;
    Send_Data.frame_tailer.end = 0x0D;
}
void USART1_SEND(void)
{
    if (uwTick - sendTick < 5)
        return;
    data_transition();
    sendTick = uwTick;
    uint8_t *data_ptr = (uint8_t *)&Send_Data;
    uint16_t data_length = sizeof(SEND_DATA);
    CDC_Transmit_FS(data_ptr, data_length);     //使用虚拟串口发送数据
}
void Self_aim_task(void const *pvParameters)
{
    /* 使用 micro usb 通信，接收中断在 usbd_cdc_if.c 中。
     * 本任务 200Hz 循环：
     *   - USART1_SEND        : 每 5ms 向上位机回发云台状态
     *   - VideoForward_Task  : 把已收到的 0x0310 视频帧转发给 VT03 */
    VideoForward_Init();
    while(1)
    {
        USART1_SEND();
        VideoForward_Task();
        osDelay(5);
    }
}
InputData *get_selfaim_data(void)
{
	return &inputdata;
}
