/**
  ****************************(C) WHU_INFANTRY_CHASSIS 2026****************************
  * @file       chassis.c/h
  * @brief      chassis control task,
  *             底盘控制任务
  * @note
  * @history
  *  Version    Date            Author          Modification
  *  V1.0.0     Dec-26-2018     RM              1. done
  *  V1.1.0     Nov-11-2019     RM              1. add chassis power control
  *
  * 
  *  v2.0.0     Otc-10-22       xzicr           1.增加smc控制yaw轴
  @verbatim
  ==============================================================================

  ==============================================================================
  @endverbatim
  ****************************(C)  WHU_INFANTRY_CHASSIS 2026****************************
  */

#include "chassis_task.h"
#include "chassis_behaviour.h"
#include "cmsis_os.h"

#include "arm_math.h"
#include "pid.h"
#include "remote_control.h"
#include "CAN_receive.h"
#include "detect_task.h"
#include "INS_task.h"
#include "vofa.h"
#include "Chassis_power_control.h"



#define rc_deadband_limit(input, output, dealine)    \
  {                                                  \
    if ((input) > (dealine) || (input) < -(dealine)) \
    {                                                \
      (output) = (input);                            \
    }                                                \
    else                                             \
    {                                                \
      (output) = 0;                                  \
    }                                                \
  }

fp32 angle_change(float angle)
{
  float angle1;
  if (angle >= 0 && angle < 180)
  {
    angle1 = angle;
  }
  else if (angle >= 0 && angle > 180)
  {
    angle1 = angle - 360;
  }
  else if (angle < 0 && angle > -180)
  {
    angle1 = angle;
  }
  else if (angle < 0 && angle < -180)
  {
    angle1 = angle + 360;
  }
  return angle1;
}

// 底盘运动数据
chassis_move_t chassis_move;

// 底盘速度环pid值
const static fp32 motor_speed_pid[3] = {M3505_MOTOR_SPEED_PID_KP, M3505_MOTOR_SPEED_PID_KI, M3505_MOTOR_SPEED_PID_KD};
const static fp32 chassis_yaw_pid[3] = {CHASSIS_FOLLOW_GIMBAL_PID_KP, CHASSIS_FOLLOW_GIMBAL_PID_KI, CHASSIS_FOLLOW_GIMBAL_PID_KD};

//滤波参数
const static fp32 chassis_x_order_filter[1] = {CHASSIS_ACCEL_X_NUM};
const static fp32 chassis_y_order_filter[1] = {CHASSIS_ACCEL_Y_NUM};




/**
 * @brief          初始化"chassis_move"变量，包括pid初始化， 遥控器指针初始化，3508底盘电机指针初始化，云台电机初始化，陀螺仪角度指针初始化
 * @param[out]     chassis_move_init:"chassis_move"变量指针.
 * @retval         none
 */
static void chassis_init(chassis_move_t *chassis_move_init);

/**
 * @brief          设置底盘控制模式，主要在'chassis_behaviour_mode_set'函数中改变
 * @param[out]     chassis_move_mode:"chassis_move"变量指针.
 * @retval         none
 */
static void chassis_set_mode(chassis_move_t *chassis_move_mode);

/**
 * @brief          底盘模式改变，有些参数需要改变，例如底盘控制yaw角度设定值应该变成当前底盘yaw角度
 * @param[out]     chassis_move_transit:"chassis_move"变量指针.
 * @retval         none
 */
void chassis_mode_change_control_transit(chassis_move_t *chassis_move_transit);

/**
 * @brief          底盘测量数据更新，包括电机速度，欧拉角度，机器人速度
 * @param[out]     chassis_move_update:"chassis_move"变量指针.
 * @retval         none
 */
static void chassis_feedback_update(chassis_move_t *chassis_move_update);

/**
 * @brief
 * @param[out]     chassis_move_update:"chassis_move"变量指针.
 * @retval         none
 */
static void chassis_set_contorl(chassis_move_t *chassis_move_control);

/**
 * @brief          控制循环，根据控制设定值，计算电机电流值，进行控制
 * @param[out]     chassis_move_control_loop:"chassis_move"变量指针.
 * @retval         none
 */
static void chassis_control_loop(chassis_move_t *chassis_move_control_loop);

#if INCLUDE_uxTaskGetStackHighWaterMark
uint32_t chassis_high_water;
#endif





void chassis_task(void const *pvParameters)
{
  // 空闲一段时间
  vTaskDelay(CHASSIS_TASK_INIT_TIME);

  // 底盘初始化
  chassis_init(&chassis_move);

  while (1)
  {
    //底盘模式设置
    chassis_set_mode(&chassis_move);

    //根据云台模式设置yaw轴控制参数
    chassis_mode_change_control_transit(&chassis_move);

    //底盘数据更新
    chassis_feedback_update(&chassis_move);

    //速度设置
    chassis_set_contorl(&chassis_move);

    //底盘功率控制
    chassis_power_limit(&chassis_move);

    //控制环
    chassis_control_loop(&chassis_move);

    // 发送控制电流
    if (!(toe_is_error(CHASSIS_MOTOR1_TOE) && toe_is_error(CHASSIS_MOTOR2_TOE) && toe_is_error(CHASSIS_MOTOR3_TOE) && toe_is_error(CHASSIS_MOTOR4_TOE))
    &&chassis_move.chassis_data_->chassis_mode!=CHASSIS_MODE_OFF)
    {
      CAN_cmd_chassis(chassis_move.motor_chassis[0].give_current, chassis_move.motor_chassis[1].give_current,
                      chassis_move.motor_chassis[2].give_current, chassis_move.motor_chassis[3].give_current);
    }
    vTaskDelay(CHASSIS_CONTROL_TIME_MS);

#if INCLUDE_uxTaskGetStackHighWaterMark
    chassis_high_water = uxTaskGetStackHighWaterMark(NULL);
#endif
  }
}


static void chassis_init(chassis_move_t *chassis_move_init)
{
  if (chassis_move_init == NULL)
  {
    return;
  }
  // 获取陀螺仪姿态角指针   获取底盘数据设定值指针   获取云台电机数据指针
  chassis_move_init->chassis_INS_angle = get_INS_angle_point();
  chassis_move_init->chassis_data_ = get_Chassisdata_point();
  chassis_move_init->gimbal_lkmotor_measure = get_yaw_gimbal_lkmotor_measure_point();
  
  // 获取底盘电机数据指针，初始化底盘电机 云台yaw轴电机PID
  uint8_t i;
  for (i = 0; i < 4; i++)
  {
    chassis_move_init->motor_chassis[i].chassis_motor_measure = get_chassis_motor_measure_point(i);
    PID_init(&chassis_move_init->motor_speed_pid[i], PID_POSITION, motor_speed_pid, M3505_MOTOR_SPEED_PID_MAX_OUT, M3505_MOTOR_SPEED_PID_MAX_IOUT);
  }
  PID_init(&chassis_move_init->chassis_angle_pid, PID_POSITION, chassis_yaw_pid, CHASSIS_FOLLOW_GIMBAL_PID_MAX_OUT, CHASSIS_FOLLOW_GIMBAL_PID_MAX_IOUT);

  // 底盘开机状态为原始
  chassis_move_init->chassis_mode = CHASSIS_VECTOR_RAW;

  // 用一阶滤波代替斜波函数生成
  first_order_filter_init(&chassis_move_init->chassis_cmd_slow_set_vx, CHASSIS_CONTROL_TIME, chassis_x_order_filter);
  first_order_filter_init(&chassis_move_init->chassis_cmd_slow_set_vy, CHASSIS_CONTROL_TIME, chassis_y_order_filter);

  // 最大 最小速度
  chassis_move_init->vx_max_speed = NORMAL_MAX_CHASSIS_SPEED_X;
  chassis_move_init->vx_min_speed = -NORMAL_MAX_CHASSIS_SPEED_X;
  chassis_move_init->vy_max_speed = NORMAL_MAX_CHASSIS_SPEED_Y;
  chassis_move_init->vy_min_speed = -NORMAL_MAX_CHASSIS_SPEED_Y;

  // 更新一下数据
  chassis_feedback_update(chassis_move_init);

}


static void chassis_set_mode(chassis_move_t *chassis_move_mode)
{
  if (chassis_move_mode == NULL)
  {
    return;
  }

  //根据云台发送模式更改底盘模式
  if (chassis_move_mode->chassis_data_->chassis_mode==CHASSIS_MODE_OFF)
  {
      chassis_move_mode->chassis_mode = CHASSIS_VECTOR_RAW;
  }
  else if (chassis_move_mode->chassis_data_->chassis_mode==CHASSIS_MODE_NO_FOLLOW)
  {
      chassis_move_mode->chassis_mode = CHASSIS_VECTOR_NO_FOLLOW_YAW;
  }
  else if (chassis_move_mode->chassis_data_->chassis_mode==CHASSIS_MODE_FOLLOW)
  {
      chassis_move_mode->chassis_mode = CHASSIS_VECTOR_FOLLOW_CHASSIS_YAW;
  }
}


static void chassis_mode_change_control_transit(chassis_move_t *chassis_move_transit)
{
  if (chassis_move_transit == NULL)
  {
    return;
  }
  if (chassis_move_transit->last_chassis_mode == chassis_move_transit->chassis_mode)
  {
    return;
  }

  //底盘模式对应yaw轴控制
  if ((chassis_move_transit->last_chassis_mode != CHASSIS_VECTOR_NO_FOLLOW_YAW) && chassis_move_transit->chassis_mode == CHASSIS_VECTOR_NO_FOLLOW_YAW)
  {
    chassis_move_transit->chassis_yaw_set = chassis_move_transit->chassis_yaw;
  }
  else if ((chassis_move_transit->last_chassis_mode != CHASSIS_VECTOR_FOLLOW_CHASSIS_YAW) && chassis_move_transit->chassis_mode == CHASSIS_VECTOR_FOLLOW_CHASSIS_YAW)
  {
    chassis_move_transit->chassis_relative_angle_set = 0.0f;
  }
  /* 暂无该功能 */
  // change to follow chassis yaw angle
  // 切入跟随底盘角度模式
  // else if ((chassis_move_transit->last_chassis_mode != CHASSIS_VECTOR_FOLLOW_CHASSIS_YAW) && chassis_move_transit->chassis_mode == CHASSIS_VECTOR_FOLLOW_CHASSIS_YAW)
  // {
  //   chassis_move_transit->chassis_yaw_set = chassis_move_transit->chassis_yaw;
  // }
  // chassis_move_transit->last_chassis_mode = chassis_move_transit->chassis_mode;
}


static void chassis_feedback_update(chassis_move_t *chassis_move_update)
{
  if (chassis_move_update == NULL)
  {
    return;
  }

  // 更新电机速度，加速度是速度的PID微分
  uint8_t i = 0;
  for (i = 0; i < 4; i++)
  {
    chassis_move_update->motor_chassis[i].speed = CHASSIS_MOTOR_RPM_TO_VECTOR_SEN * chassis_move_update->motor_chassis[i].chassis_motor_measure->speed_rpm;
    chassis_move_update->motor_chassis[i].accel = chassis_move_update->motor_speed_pid[i].Dbuf[0] * CHASSIS_CONTROL_FREQUENCE;
  }

  // 更新底盘纵向速度 x， 平移速度y，旋转速度wz，坐标系为右手系
  chassis_move_update->vx = (chassis_move_update->motor_chassis[0].speed - chassis_move_update->motor_chassis[1].speed - chassis_move_update->motor_chassis[2].speed + chassis_move_update->motor_chassis[3].speed) * MOTOR_SPEED_TO_CHASSIS_SPEED_VX / OMNIDIRECTIONAL_WHEEL;
  chassis_move_update->vy = (chassis_move_update->motor_chassis[0].speed + chassis_move_update->motor_chassis[1].speed - chassis_move_update->motor_chassis[2].speed - chassis_move_update->motor_chassis[3].speed) * MOTOR_SPEED_TO_CHASSIS_SPEED_VY / OMNIDIRECTIONAL_WHEEL;
  chassis_move_update->wz = (chassis_move_update->motor_chassis[0].speed + chassis_move_update->motor_chassis[1].speed + chassis_move_update->motor_chassis[2].speed + chassis_move_update->motor_chassis[3].speed) * MOTOR_SPEED_TO_CHASSIS_SPEED_WZ / OMNIDIRECTIONAL_WHEEL / MOTOR_DISTANCE_TO_CENTER;

  // 计算底盘姿态角度, 如果底盘上有陀螺仪请更改这部分代码
  chassis_move_update->chassis_yaw = rad_format(*(chassis_move_update->chassis_INS_angle + INS_YAW_ADDRESS_OFFSET));     // - chassis_move_update->chassis_yaw_motor->relative_angle);
  chassis_move_update->chassis_pitch = rad_format(*(chassis_move_update->chassis_INS_angle + INS_PITCH_ADDRESS_OFFSET)); //- chassis_move_update->chassis_pitch_motor->relative_angle);
  chassis_move_update->chassis_roll = *(chassis_move_update->chassis_INS_angle + INS_ROLL_ADDRESS_OFFSET);
  chassis_move_update->chassis_relative_angle = chassis_move_update->gimbal_lkmotor_measure->angle;//(rad_format((chassis_move_update->chassis_data_->yaw_angle) * 3.1415926535 / 180) - rad_format(*(chassis_move_update->chassis_INS_angle + INS_YAW_ADDRESS_OFFSET))) * 180 / 3.1415926535; // angle_change((chassis_move_update->gimbal_yaw_motor.gimbal_motor_measure->ecd-2)*360/8192);
}


static void chassis_set_contorl(chassis_move_t *chassis_move_control)
{

  if (chassis_move_control == NULL)
  {
    return;
  }

  //中间变量
  fp32 vx1_set = 0.0f, vy1_set = 0.0f;
  static fp32 angle;

  //跟随模式
  if (chassis_move_control->chassis_mode == CHASSIS_VECTOR_FOLLOW_CHASSIS_YAW)
  {
    chassis_move_control->vx_set = fp32_constrain(chassis_move_control->chassis_data_->vx_set, chassis_move_control->vx_min_speed, chassis_move_control->vx_max_speed);
    chassis_move_control->vy_set = fp32_constrain(chassis_move_control->chassis_data_->vy_set, chassis_move_control->vy_min_speed, chassis_move_control->vy_max_speed);

    // 设置控制相对云台角度
    // chassis_move_control->chassis_relative_angle_set = 0;
    // if (chassis_move_control->chassis_relative_angle > -1.0f && chassis_move_control->chassis_relative_angle < 1.0f)
    // {
    //   chassis_move_control->wz_set = 0;
    // }
    // else
      chassis_move_control->wz_set = PID_calc(&chassis_move_control->chassis_angle_pid, chassis_move_control->chassis_relative_angle, chassis_move_control->chassis_relative_angle_set);
      // chassis_move_control->wz_set = chassis_move_control->chassis_data_->wz_set;

  }

//小陀螺模式
else if (chassis_move_control->chassis_mode == CHASSIS_VECTOR_NO_FOLLOW_YAW)
{
    chassis_move_control->wz_set = chassis_move_control->chassis_data_->wz_set;
    
    // 添加固定角度偏移补偿（单位：度），根据实际测试调整这个值
    #define GYRO_ANGLE_OFFSET 10.0f  // 比如偏移5度，往哪个方向偏就调正负
    fp32 compensated_angle;
    if(chassis_move_control->wz>0)
    {
      compensated_angle = chassis_move_control->chassis_relative_angle - GYRO_ANGLE_OFFSET;
    }
    else
    {
      compensated_angle = chassis_move_control->chassis_relative_angle + GYRO_ANGLE_OFFSET;
    }
    angle = compensated_angle / 180.0f * 3.141592654f;

    vx1_set = chassis_move_control->chassis_data_->vx_set * arm_cos_f32(angle) - chassis_move_control->chassis_data_->vy_set * arm_sin_f32(angle);
    vy1_set = chassis_move_control->chassis_data_->vx_set * arm_sin_f32(angle) + chassis_move_control->chassis_data_->vy_set * arm_cos_f32(angle);  
    chassis_move_control->vx_set = fp32_constrain(vx1_set, chassis_move_control->vx_min_speed, chassis_move_control->vx_max_speed);
    chassis_move_control->vy_set = fp32_constrain(vy1_set, chassis_move_control->vy_min_speed, chassis_move_control->vy_max_speed);
}

  // 在原始模式，设置值是发送到CAN总线
  else if (chassis_move_control->chassis_mode == CHASSIS_VECTOR_RAW)
  {
    chassis_move_control->vx_set = 0.0f;
    chassis_move_control->vy_set = 0.0f;
    chassis_move_control->wz_set = 0.0f;
    chassis_move_control->chassis_cmd_slow_set_vx.out = 0.0f;
    chassis_move_control->chassis_cmd_slow_set_vy.out = 0.0f;
  }
}


static void chassis_vector_to_mecanum_wheel_speed(const fp32 vx_set, const fp32 vy_set, const fp32 wz_set, fp32 wheel_speed[4])
{
  // because the gimbal is in front of chassis, when chassis rotates, wheel 0 and wheel 1 should be slower and wheel 2 and wheel 3 should be faster
  // 旋转的时候， 由于云台靠前，所以是前面两轮 0 ，1 旋转的速度变慢， 后面两轮 2,3 旋转的速度变快
  wheel_speed[0] = (vx_set + vy_set + (CHASSIS_WZ_SET_SCALE - 1.0f) * MOTOR_DISTANCE_TO_CENTER * wz_set) * 1.41421356f;   //这是根号二
  wheel_speed[1] = (-vx_set + vy_set + (CHASSIS_WZ_SET_SCALE - 1.0f) * MOTOR_DISTANCE_TO_CENTER * wz_set) * 1.41421356f;
  wheel_speed[2] = (-vx_set - vy_set + (-CHASSIS_WZ_SET_SCALE - 1.0f) * MOTOR_DISTANCE_TO_CENTER * wz_set) * 1.41421356f;
  wheel_speed[3] = (vx_set - vy_set + (-CHASSIS_WZ_SET_SCALE - 1.0f) * MOTOR_DISTANCE_TO_CENTER * wz_set) * 1.41421356f;
}

static void chassis_control_loop(chassis_move_t *chassis_move_control_loop)
{
  fp32 max_vector = 0.0f, vector_rate = 0.0f;
  fp32 temp = 0.0f;
  fp32 current_set = 0.0f;
  fp32 wheel_speed[4] = {0.0f, 0.0f, 0.0f, 0.0f};
  uint8_t i = 0;

  //速度解算成电机转速
  chassis_vector_to_mecanum_wheel_speed(chassis_move_control_loop->vx_set,
                                        chassis_move_control_loop->vy_set, chassis_move_control_loop->wz_set, wheel_speed);

  if (chassis_move_control_loop->chassis_mode == CHASSIS_VECTOR_RAW)
  {
    for (i = 0; i < 4; i++)
    {
      current_set = fp32_constrain(wheel_speed[i], -16000.0f, 16000.0f);
      chassis_move_control_loop->motor_chassis[i].give_current = (int16_t)current_set;
    }
    return;
  }

  for (i = 0; i < 4; i++)
  {
    chassis_move_control_loop->motor_chassis[i].speed_set = wheel_speed[i];
    temp = fabs(chassis_move_control_loop->motor_chassis[i].speed_set);
    if (max_vector < temp)
    {
      max_vector = temp;
    }
  }

  if (max_vector > MAX_WHEEL_SPEED)
  {
    vector_rate = MAX_WHEEL_SPEED / max_vector;
    for (i = 0; i < 4; i++)
    {
      chassis_move_control_loop->motor_chassis[i].speed_set *= vector_rate;
    }
  }

  // 计算pid
  for (i = 0; i < 4; i++)
  {
    PID_calc(&chassis_move_control_loop->motor_speed_pid[i], chassis_move_control_loop->motor_chassis[i].speed, chassis_move_control_loop->motor_chassis[i].speed_set);
  }
  // 赋值电流值
    for (i = 0; i < 4; i++)
    {                                                                           //0.5是毫无意义的系数，只是为了缩小电流
       current_set = Klimit * 0.5f * Plimit * chassis_move_control_loop->motor_speed_pid[i].out;
       current_set = fp32_constrain(current_set, -16000.0f, 16000.0f);
       chassis_move_control_loop->motor_chassis[i].give_current = (int16_t)current_set;
    }
}
