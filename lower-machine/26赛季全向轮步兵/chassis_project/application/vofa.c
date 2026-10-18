#include "vofa.h"
#include "usart.h"
void sendfloatdata(float data[],uint8_t number)
{
	char tail[4]={0x00, 0x00, 0x80, 0x7f};
	HAL_UART_Transmit(&huart1, (uint8_t *)data, sizeof(float)*number, 100);
	HAL_UART_Transmit(&huart1, (uint8_t *)tail, 4, 100);
    HAL_Delay(100);
}