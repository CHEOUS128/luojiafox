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

// uint32_t usb_rx_length = 0;


void process_usb_data_packet(uint8_t* data, uint16_t length)
{
    /* 控制帧解析：0xFF 帧头，32 字节（一帧三阶指令）。
     * 同一批 USB 数据里还可能含有 309 字节视频帧，视频帧由
     * video_forward.c 单独解析，这里只处理 0xFF 控制帧。
     * 旧实现在收满一帧后直接 return，会丢掉同批数据里后面的帧，
     * 因此改为逐字节扫描、不提前返回。 */
    static uint8_t temp_buffer[sizeof(RECEIVE_DATA)];
    static uint16_t temp_received = 0;

    for (uint16_t i = 0; i < length; i++) {
        uint8_t byte = data[i];

        if (temp_received == 0)
        {
            if (byte == FRAME_HEADER_FF) {
                temp_buffer[temp_received++] = byte;
            }
        }
        else if (temp_received < sizeof(RECEIVE_DATA))
        {
            temp_buffer[temp_received++] = byte;

            if (temp_received == sizeof(RECEIVE_DATA)) {
                memcpy(&inputdata, temp_buffer + sizeof(FrameHeader), sizeof(InputData));
                temp_received = 0;
            }
        }
        else
        {
            temp_received = 0;
        }
    }
}

void usbd_cdc_receive_handle(uint8_t *data, uint16_t length)
{
    if (data == 0 || length == 0) {
        return;
    }
    VideoForward_Feed(data, length);
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
