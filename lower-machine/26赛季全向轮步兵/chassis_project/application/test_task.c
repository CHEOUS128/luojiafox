/**
  ****************************(C) COPYRIGHT 2019 DJI****************************
  * @file       test_task.c/h
  * @brief      buzzer warning task.蜂鸣器报警任务
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

#include "test_task.h"
#include "main.h"
#include "cmsis_os.h"
#include "bsp_buzzer.h"
#include "detect_task.h"
#include "vofa.h"
#include "chassis_task.h"
#include "CAN_receive.h"
#include "shoot.h"
#include "gimbal_task.h"
#include "ui.h"
#include "referee.h"
#include "ui_interface.h"





static void buzzer_warn_error(uint8_t num);

const error_t *error_list_test_local;

extern __IO uint32_t uwTick;
uint32_t sendTick =0;

uint32_t ui_period=100;
uint32_t ui_tick;

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



void UI_RotateRound(void)
{

  // if(-180 <= chassis_move.chassis_relative_angle <=0)
  // {
  //   ui_side_DynamicGroup_DirectionArc->start_angle = 30 - chassis_move.chassis_relative_angle;

  //   ui_side_DynamicGroup_DirectionArc->end_angle = 330 - chassis_move.chassis_relative_angle;
  // }

  // if(0 <= chassis_move.chassis_relative_angle <= 180)
  // {
  //   ui_side_DynamicGroup_DirectionArc->start_angle = 30 - chassis_move.chassis_relative_angle + 360;
  //   ui_side_DynamicGroup_DirectionArc->end_angle = 330 - chassis_move.chassis_relative_angle + 360;
  // }
  

//底盘相对位置改变
        if(chassis_move.chassis_relative_angle<=0)
        {
            ui_side_DynamicGroup_DirectionArc->start_angle=-(chassis_move.chassis_relative_angle)+35;
            if(chassis_move.chassis_relative_angle<=-35)
            {
            ui_side_DynamicGroup_DirectionArc->end_angle =-(chassis_move.chassis_relative_angle)-35;
            }
            else
            {
            ui_side_DynamicGroup_DirectionArc->end_angle =-(chassis_move.chassis_relative_angle)+325;
            }
        }
        
        if(chassis_move.chassis_relative_angle>=0)
        {
            ui_side_DynamicGroup_DirectionArc->end_angle =-(chassis_move.chassis_relative_angle)+325;
            if(chassis_move.chassis_relative_angle>=35)
            {
            ui_side_DynamicGroup_DirectionArc->start_angle=-(chassis_move.chassis_relative_angle)+395;
            }
            else
            {
            ui_side_DynamicGroup_DirectionArc->start_angle=-(chassis_move.chassis_relative_angle)+35;
            }
        }



}









/**
  * @brief          test task
  * @param[in]      pvParameters: NULL
  * @retval         none
  */
/**
  * @brief          test任务
  * @param[in]      pvParameters: NULL
  * @retval         none
  */
void test_task(void const * argument)
{
    // static uint8_t error, last_error;
    // static uint8_t error_num;
    // error_list_test_local = get_error_list_point();
	
//      ui_tick = HAL_GetTick();

		ui_init_middle_StaticGraphicGroup();
		ui_init_side_DynamicGroup();
		ui_init_side_StaticTextGroup();
	

	
    while(1)
    {
		ui_self_id = robot_state.robot_id;//需要确认机器人的ID号   //ID: 1 用于南航UI绘制调试
		

		ui_init_middle_StaticGraphicGroup();
		ui_init_side_DynamicGroup();
		ui_init_side_StaticTextGroup();
		


      //摩擦轮
      if(shoot_control.shoot_control_data->shoot_mode_rc!=SHOOT_STOP)     
      {
        ui_side_DynamicGroup_FricRound->color = 0;
      }
//	  else
//	  {
//		ui_side_DynamicGroup_FricRound->color = UI_Color_White;
//	  }
		

      //自瞄
      if(shoot_control.shoot_control_data->shoot_mode_receive>>5 & 0x01)   
      {
        ui_side_DynamicGroup_AutoRound->color = 0;
      }
//      else
//      {
//        ui_side_DynamicGroup_AutoRound->color = UI_Color_White;
//      }

      //小陀螺
      if(shoot_control.shoot_control_data->shoot_mode_receive>>6 & 0x01)   
      {
        ui_side_DynamicGroup_RotateRound->color = 1;
      }
//      else
//      {
//        ui_side_DynamicGroup_RotateRound->color = UI_Color_White;
//      }








      UI_RotateRound();

//      ui_side_DynamicGroup_DirectionArc->start_angle = 
//      30 + gimbal_control.gimbal_yaw_motor.relative_angle;

//      ui_side_DynamicGroup_DirectionArc->end_angle = 
//      330 + gimbal_control.gimbal_yaw_motor.relative_angle;

//      while(ui_side_DynamicGroup_DirectionArc->start_angle <= 0)
//      {
//        ui_side_DynamicGroup_DirectionArc->start_angle += 360;
//      }

//      while(ui_side_DynamicGroup_DirectionArc->end_angle >= 360)
//      {
//        ui_side_DynamicGroup_DirectionArc->start_angle -= 360;
//      }


      
      //2006转速显示
      ui_side_DynamicGroup_M2006SpeedNumber->number = (int32_t) shoot_control.shoot_motor_measure->speed_rpm;
		


		ui_update_middle_StaticGraphicGroup();
		ui_update_side_DynamicGroup();
		ui_update_side_StaticTextGroup();
		
		
		
        vTaskDelay(1);
    }

}


/**
  * @brief          make the buzzer sound
  * @param[in]      num: the number of beeps 
  * @retval         none
  */
/**
  * @brief          使得蜂鸣器响
  * @param[in]      num:响声次数
  * @retval         none
  */
static void buzzer_warn_error(uint8_t num)
{
    static uint8_t show_num = 0;
    static uint8_t stop_num = 100;
    if(show_num == 0 && stop_num == 0)
    {
        show_num = num;
        stop_num = 100;
    }
    else if(show_num == 0)
    {
        stop_num--;
        buzzer_off();
    }
    else
    {
        static uint8_t tick = 0;
        tick++;
        if(tick < 50)
        {
            buzzer_off();
        }
        else if(tick < 100)
        {
            buzzer_on(1, 30000);
        }
        else
        {
            tick = 0;
            show_num--;
        }
    }
}


