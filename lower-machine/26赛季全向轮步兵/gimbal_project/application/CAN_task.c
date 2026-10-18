#include "main.h"
#include "cmsis_os.h"
#include "CAN_task.h"
#include "can.h"
#include "CAN_receive.h"
#include "gimbal_task.h"

void can_task(void const *pvParameters)
{
	chassis_data_t *chassis_data;
	chassis_data=get_chassis_data_point();
	while(1)
	{
		can_gimbal_chassis_data1(chassis_data);
		can_gimbal_chassis_data2(chassis_data);
		can_gimbal_chassis_data3(chassis_data);
		can_gimbal_chassis_data4(chassis_data);
		can_gimbal_chassis_data5(chassis_data);//新增

		if(reset_flag == 1)
		{
			// System_Reset();
			reset_flag = 0;
		}
		osDelay(1);
	}
}


