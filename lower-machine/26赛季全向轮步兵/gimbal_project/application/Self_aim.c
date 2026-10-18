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
    static uint8_t temp_buffer[sizeof(RECEIVE_DATA)];
    static uint16_t temp_received = 0;
    
    for (uint16_t i = 0; i < length; i++) {
        uint8_t byte = data[i];
        
        // 使用状态机解析数据包
        if (temp_received == 0) 
        {
            // 寻找帧头
            if (byte == FRAME_HEADER_FF) {
                temp_buffer[temp_received++] = byte;        //就一个帧头，接完直接接数据
            }
        } 
        else if (temp_received >= sizeof(FrameHeader) && 
                   temp_received < sizeof(RECEIVE_DATA)) {
            // 接收数据部分
            temp_buffer[temp_received++] = byte;
            
            // 检查是否接收完整数据包
            if (temp_received == sizeof(RECEIVE_DATA)) {
                // 验证CRC
                // if (verify_CRC16_check_sum(temp_buffer, sizeof(RECEIVE_DATA))) {
                    memcpy(&inputdata, temp_buffer + sizeof(FrameHeader), sizeof(InputData));
                    // 数据包处理完成
                // }
                temp_received = 0; // 重置状态
                return ;
            }
        } else {
            temp_received = 0; // 异常状态重置
        }
    }
}

/*虚拟串口接收中断处理函数*/ 
void usbd_cdc_receive_handle(void)
{
    process_usb_data_packet(Usart_Receive, 40);
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
    //使用micro usb通信，中断在usbd_cdc_if.c中
	while(1)
	{
		USART1_SEND();
		osDelay(5);
	}
}
InputData *get_selfaim_data(void)
{
	return &inputdata;
}
