/**
  ****************************(C) COPYRIGHT 2019 DJI****************************
  * @file       referee_usart_task.c/h
  * @brief      RM referee system data solve. RM裁判系统数据处理
  * @note       
  * @history
  *  Version    Date            Author          Modification
  *  V1.0.0     Nov-11-2019     RM              1. done
  *
  @verbatim
  ==============================================================================

  ==============================================================================
  @endverbatim
  ****************************(C) COPYRIGHT 2019 DJI****************************
  */
#include "referee_usart_task.h"
#include "main.h"
#include "cmsis_os.h"
#include "bsp_usart.h"
#include "detect_task.h"
#include "CRC8_CRC16.h"
#include "fifo.h"
#include "protocol.h"
#include "referee.h"
#include "ui.h"
#include "shoot.h"
#include "user_lib.h"
#include "gimbal_task.h"

/**
  * @brief          single byte upacked 
  * @param[in]      void
  * @retval         none
  */
/**
  * @brief          单字节解包
  * @param[in]      void
  * @retval         none
  */
static void referee_unpack_fifo_data(void);

 
extern UART_HandleTypeDef huart6;

uint8_t usart6_buf[2][USART_RX_BUF_LENGHT];

fifo_s_t referee_fifo;
uint8_t referee_fifo_buf[REFEREE_FIFO_BUF_LENGTH];
unpack_data_t referee_unpack_obj;

// uint32_t ui_period=100;
// uint32_t ui_tick;

//机器人ID
extern int ui_self_id; 
extern ext_game_robot_state_t robot_state;
extern ext_power_heat_data_t power_heat_data_t;

//裁判系统数据
extern ext_game_robot_state_t robot_state;
extern ext_power_heat_data_t power_heat_data_t;

//射击标志位数据
extern shoot_control_t shoot_control;
extern gimbal_control_t gimbal_control;

// uint8_t aimflag = 0,fric_flag = 0,Rotate_flag = 0;



void referee_usart_task(void const * argument)
{
    init_referee_struct_data();
    fifo_s_init(&referee_fifo, referee_fifo_buf, REFEREE_FIFO_BUF_LENGTH);
    usart6_init(usart6_buf[0], usart6_buf[1], USART_RX_BUF_LENGHT);
    osDelay(10);
      // ui_tick = HAL_GetTick();
	
	
    while(1)
    {
      referee_unpack_fifo_data();
      // ui_self_id = robot_state.robot_id;//需要确认机器人的ID号   //ID: 1 用于南航UI绘制调试


      osDelay(4);
      

      // if((HAL_GetTick()-ui_tick)>3*ui_period)
      // {
      //   ui_init_g_DynamicGroup();
      //   ui_init_g_StaticGraphicGroup();
      //   ui_init_g_StaticTextGroup();
      //   ui_tick = HAL_GetTick();

      // }
      
      // //摩擦轮
      // if(shoot_control.shoot_control_data->shoot_mode_rc!=SHOOT_STOP)     
      // {
      //   ui_g_DynamicGroup_FricRound->color = UI_Color_Main;
      // }
      // else{
      //   ui_g_DynamicGroup_FricRound->color = UI_Color_White;
      // }

      // //自瞄
      // if(shoot_control.shoot_control_data->shoot_mode_receive>>5 && 0x01)   
      // {
      //   ui_g_DynamicGroup_AutoRound->color = UI_Color_Main;
      // }
      // else
      // {
      //   ui_g_DynamicGroup_AutoRound->color = UI_Color_White;
      // }

      // //小陀螺
      // if(shoot_control.shoot_control_data->shoot_mode_receive>>6 && 0x01)   
      // {
      //   ui_g_DynamicGroup_RotateRound->color = UI_Color_Main;
      // }
      // else
      // {
      //   ui_g_DynamicGroup_RotateRound->color = UI_Color_White;
      // }


      // ui_g_DynamicGroup_DirectionArc->start_angle = 
      // 30 + gimbal_control.gimbal_yaw_motor.relative_angle;

      // ui_g_DynamicGroup_DirectionArc->end_angle = 
      // 330 + gimbal_control.gimbal_yaw_motor.relative_angle;

      // while(ui_g_DynamicGroup_DirectionArc->start_angle < 0)
      // {
      //   ui_g_DynamicGroup_DirectionArc->start_angle += 360;
      // }

      // while(ui_g_DynamicGroup_DirectionArc->end_angle > 360)
      // {
      //   ui_g_DynamicGroup_DirectionArc->start_angle -= 360;
      // }


      
      // //2006转速显示
      // ui_g_DynamicGroup_M2006_speed->number = (int32_t) shoot_control.shoot_motor_measure->speed_rpm;




      // ui_update_g_DynamicGroup();
      // ui_update_g_StaticGraphicGroup();
      // ui_update_g_StaticTextGroup();


    }
}


/**
  * @brief          single byte upacked 
  * @param[in]      void
  * @retval         none
  */
/**
  * @brief          单字节解包
  * @param[in]      void
  * @retval         none
  */
void referee_unpack_fifo_data(void)
{
  uint8_t byte = 0;
  uint8_t sof = HEADER_SOF;
  unpack_data_t *p_obj = &referee_unpack_obj;

  while ( fifo_s_used(&referee_fifo) )
  {
    byte = fifo_s_get(&referee_fifo);
    switch(p_obj->unpack_step)
    {
      case STEP_HEADER_SOF:
      {
        if(byte == sof)
        {
          p_obj->unpack_step = STEP_LENGTH_LOW;
          p_obj->protocol_packet[p_obj->index++] = byte;
        }
        else
        {
          p_obj->index = 0;
        }
      }break;
      
      case STEP_LENGTH_LOW:
      {
        p_obj->data_len = byte;
        p_obj->protocol_packet[p_obj->index++] = byte;
        p_obj->unpack_step = STEP_LENGTH_HIGH;
      }break;
      
      case STEP_LENGTH_HIGH:
      {
        p_obj->data_len |= (byte << 8);
        p_obj->protocol_packet[p_obj->index++] = byte;

        if(p_obj->data_len < (REF_PROTOCOL_FRAME_MAX_SIZE - REF_HEADER_CRC_CMDID_LEN))
        {
          p_obj->unpack_step = STEP_FRAME_SEQ;
        }
        else
        {
          p_obj->unpack_step = STEP_HEADER_SOF;
          p_obj->index = 0;
        }
      }break;
      case STEP_FRAME_SEQ:
      {
        p_obj->protocol_packet[p_obj->index++] = byte;
        p_obj->unpack_step = STEP_HEADER_CRC8;
      }break;

      case STEP_HEADER_CRC8:
      {
        p_obj->protocol_packet[p_obj->index++] = byte;

        if (p_obj->index == REF_PROTOCOL_HEADER_SIZE)
        {
          if ( verify_CRC8_check_sum(p_obj->protocol_packet, REF_PROTOCOL_HEADER_SIZE) )
          {
            p_obj->unpack_step = STEP_DATA_CRC16;
          }
          else
          {
            p_obj->unpack_step = STEP_HEADER_SOF;
            p_obj->index = 0;
          }
        }
      }break;  
      
      case STEP_DATA_CRC16:
      {
        if (p_obj->index < (REF_HEADER_CRC_CMDID_LEN + p_obj->data_len))
        {
           p_obj->protocol_packet[p_obj->index++] = byte;  
        }
        if (p_obj->index >= (REF_HEADER_CRC_CMDID_LEN + p_obj->data_len))
        {
          p_obj->unpack_step = STEP_HEADER_SOF;
          p_obj->index = 0;

          if ( verify_CRC16_check_sum(p_obj->protocol_packet, REF_HEADER_CRC_CMDID_LEN + p_obj->data_len) )
          {
            referee_data_solve(p_obj->protocol_packet);
          }
        }
      }break;

      default:
      {
        p_obj->unpack_step = STEP_HEADER_SOF;
        p_obj->index = 0;
      }break;
    }
  }
}


void USART6_IRQHandler(void)
{
    static volatile uint8_t res;
    if(USART6->SR & UART_FLAG_IDLE)
    {
        __HAL_UART_CLEAR_PEFLAG(&huart6);

        static uint16_t this_time_rx_len = 0;

        if ((huart6.hdmarx->Instance->CR & DMA_SxCR_CT) == RESET)
        {
            __HAL_DMA_DISABLE(huart6.hdmarx);
            this_time_rx_len = USART_RX_BUF_LENGHT - __HAL_DMA_GET_COUNTER(huart6.hdmarx);
            __HAL_DMA_SET_COUNTER(huart6.hdmarx, USART_RX_BUF_LENGHT);
            huart6.hdmarx->Instance->CR |= DMA_SxCR_CT;
            __HAL_DMA_ENABLE(huart6.hdmarx);
            fifo_s_puts(&referee_fifo, (char*)usart6_buf[0], this_time_rx_len);
            detect_hook(REFEREE_TOE);
        }
        else
        {
            __HAL_DMA_DISABLE(huart6.hdmarx);
            this_time_rx_len = USART_RX_BUF_LENGHT - __HAL_DMA_GET_COUNTER(huart6.hdmarx);
            __HAL_DMA_SET_COUNTER(huart6.hdmarx, USART_RX_BUF_LENGHT);
            huart6.hdmarx->Instance->CR &= ~(DMA_SxCR_CT);
            __HAL_DMA_ENABLE(huart6.hdmarx);
            fifo_s_puts(&referee_fifo, (char*)usart6_buf[1], this_time_rx_len);
            detect_hook(REFEREE_TOE);
        }
    }
}


