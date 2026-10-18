#include "main.h"
#include "cmsis_os.h"
#include "Can_task.h"
#include "CAN_receive.h"
#include "referee.h"
#include "gimbal_task.h"
#include "referee_usart_task.h"
#include "chassis_task.h"


extern gimbal_control_t gimbal_control;
extern chassis_move_t chassis_move;
    uint8_t shoot_bullet_fre = 0;
    float shoot_bullet_speed = 0;
    uint16_t chassis_power_limit_get = 0;
    float chassis_power_buffer_get = 0;

void robot_id_to_enemy_color(uint8_t robot_id, uint8_t *enemy_color)
{
    switch(robot_id)
    {
        case UI_Data_RobotID_RHero:         
        case UI_Data_RobotID_REngineer:
        case UI_Data_RobotID_RStandard1:
        case UI_Data_RobotID_RStandard2:
        case UI_Data_RobotID_RStandard3:
        case UI_Data_RobotID_RAerial:
        case UI_Data_RobotID_RSentry:
        case UI_Data_RobotID_RRadar:
            *enemy_color = 0;
            break;
        case UI_Data_RobotID_BHero:
        case UI_Data_RobotID_BEngineer:
        case UI_Data_RobotID_BStandard1:
        case UI_Data_RobotID_BStandard2:
        case UI_Data_RobotID_BStandard3:
        case UI_Data_RobotID_BAerial:
        case UI_Data_RobotID_BSentry:
        case UI_Data_RobotID_BRadar:
            *enemy_color = 1;
            break;
        default:
            break;
    }
}
void Can_task(void const *pvParameters)
{
    uint8_t color = 0U;
    
    
    chassis_move.super_power.enableDCDC = 1;
    chassis_move.super_power.systemRestart = 0;
    chassis_move.super_power.clearError = 0;
    chassis_move.super_power.useNewFeedbackMessage = 1;
    chassis_move.super_power.enableActiveChargingLimit = 0;
    chassis_move.super_power.resv0 = 0;
    chassis_move.super_power.resv2 = 0;
    
    
    while(1)
	{
        robot_id_to_enemy_color(get_robot_id(), &color);
        (void)color;
        
        /* send referee and super capacitor control */
        get_chassis_power_limit(&chassis_power_limit_get);
        get_chassis_buffer(&chassis_power_buffer_get);
        get_shoot_speed_fre(&shoot_bullet_fre,&shoot_bullet_speed);
        
        chassis_move.super_power.refereePowerLimit = chassis_power_limit_get;
        chassis_move.super_power.refereeEnergyBuffer = (uint16_t)chassis_power_buffer_get;
        chassis_move.super_power.enableActiveChargingLimit = 1;
        chassis_move.super_power.activeChargingLimitRatio = 100;
        
		CAN_cmd_referee_data(shoot_bullet_fre,shoot_bullet_speed, chassis_power_limit_get);
        CAN_SuperPower_Control(chassis_move.super_power);

        CAN_INIT_STATUS(gimbal_control.YAW_INIT_FLAG);
		osDelay(10);
	}
}
