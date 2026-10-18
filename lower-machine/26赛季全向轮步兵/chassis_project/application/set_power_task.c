#include "set_power_task.h"
#include "cmsis_os.h"

void send_setpower_task(void const *pvParameters)
{
    (void)pvParameters;

    while (1)
    {
        /* Legacy 0x210 super-cap protocol is disabled; use CAN_SuperPower_Control(0x061). */
        osDelay(1000);
    }
}
