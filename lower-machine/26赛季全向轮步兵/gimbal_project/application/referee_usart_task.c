/**
  ****************************(C) COPYRIGHT 2019 DJI****************************
  * @file       referee_usart_task.c/h
  * @brief      RM referee system data solve. RM?????????????
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
#include "stm32f4xx_hal.h"
#include "bsp_usart.h"
#include "detect_task.h"

#include "CRC8_CRC16.h"
#include "fifo.h"
#include "protocol.h"
#include "referee.h"
#include "RM_Cilent_UI.h"
#include "string.h"

Graph_Data G1,G2,G3,G4,G5;

/**
  * @brief          single byte upacked 
  * @param[in]      void
  * @retval         none
  */
/**
  * @brief          ???????
  * @param[in]      void
  * @retval         none
  */
static void referee_unpack_fifo_data(void);

 
extern UART_HandleTypeDef huart1;

uint8_t usart6_buf[2][USART_RX_BUF_LENGHT];

fifo_s_t referee_fifo;
uint8_t referee_fifo_buf[REFEREE_FIFO_BUF_LENGTH];
unpack_data_t referee_unpack_obj;

int Char_sizeof=0;
char Dr_Char_1[]={" \nyaw\npit\n"};
char Dr_Char_2[30]={0};
/**
  * @brief          referee task
  * @param[in]      pvParameters: NULL
  * @retval         none
  */
/**
  * @brief          ??????????
  * @param[in]      pvParameters: NULL
  * @retval         none
  */
// void referee_usart_task(void const * argument)
// {
//    init_referee_struct_data();
//    fifo_s_init(&referee_fifo, referee_fifo_buf, REFEREE_FIFO_BUF_LENGTH);
//    usart6_init(usart6_buf[0], usart6_buf[1], USART_RX_BUF_LENGHT);

//    while(1)
//    {

//        referee_unpack_fifo_data();
//        osDelay(10);
//    }
// }
uint32_t uiTick;
extern uint32_t uwTick;
void referee_usart_task(void const * argument)
{
   init_referee_struct_data();
   fifo_s_init(&referee_fifo, referee_fifo_buf, REFEREE_FIFO_BUF_LENGTH);
   usart6_init(usart6_buf[0], usart6_buf[1], USART_RX_BUF_LENGHT);
	memset(&G1,0,sizeof(G1));
	memset(&G2,0,sizeof(G2));
	memset(&G3,0,sizeof(G3));
	memset(&G4,0,sizeof(G4));
	memset(&G5,0,sizeof(G5));
	
	Line_Draw(&G1,"001",UI_Graph_ADD,9,UI_Color_White,2,955,475+20,955,475-20);
	Line_Draw(&G2,"002",UI_Graph_ADD,9,UI_Color_White,2,955-20,475,955+20,475);
	UI_ReFresh(2,G1,G2);
   while(1)
   {
       referee_unpack_fifo_data();
       osDelay(10);
			if((uwTick-uiTick)>1000)
			{
				uiTick = uwTick;
				 Line_Draw(&G1,"001",UI_Graph_ADD,9,UI_Color_White,2,955,475+20,955,475-20);
				Line_Draw(&G2,"002",UI_Graph_ADD,9,UI_Color_White,2,955-20,475,955+20,475);
				UI_ReFresh(2,G1,G2);
				Line_Draw(&G1,"001",UI_Graph_Change,9,UI_Color_White,2,960,475+20,960,475-20);
				Line_Draw(&G2,"002",UI_Graph_Change,9,UI_Color_White,2,960-20,475,960+20,475);
				UI_ReFresh(2,G1,G2);
			}
   }
}

/**
  * @brief          single byte upacked 
  * @param[in]      void
  * @retval         none
  */
/**
  * @brief          ???????
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


//void USART6_IRQHandler(void)
//{
//    static volatile uint8_t res;
//    if(USART6->SR & UART_FLAG_IDLE)
//    {
//        __HAL_UART_CLEAR_PEFLAG(&huart6);

//        static uint16_t this_time_rx_len = 0;

//        if ((huart6.hdmarx->Instance->CR & DMA_SxCR_CT) == RESET)
//        {
//            __HAL_DMA_DISABLE(huart6.hdmarx);
//            this_time_rx_len = USART_RX_BUF_LENGHT - __HAL_DMA_GET_COUNTER(huart6.hdmarx);
//            __HAL_DMA_SET_COUNTER(huart6.hdmarx, USART_RX_BUF_LENGHT);
//            huart6.hdmarx->Instance->CR |= DMA_SxCR_CT;
//            __HAL_DMA_ENABLE(huart6.hdmarx);
//            fifo_s_puts(&referee_fifo, (char*)usart6_buf[0], this_time_rx_len);
//            detect_hook(REFEREE_TOE);
//        }
//        else
//        {
//            __HAL_DMA_DISABLE(huart6.hdmarx);
//            this_time_rx_len = USART_RX_BUF_LENGHT - __HAL_DMA_GET_COUNTER(huart6.hdmarx);
//            __HAL_DMA_SET_COUNTER(huart6.hdmarx, USART_RX_BUF_LENGHT);
//            huart6.hdmarx->Instance->CR &= ~(DMA_SxCR_CT);
//            __HAL_DMA_ENABLE(huart6.hdmarx);
//            fifo_s_puts(&referee_fifo, (char*)usart6_buf[1], this_time_rx_len);
//            detect_hook(REFEREE_TOE);
//        }
//    }
//}


