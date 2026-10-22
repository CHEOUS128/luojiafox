/**
  ****************************(C) 2026 WHU_infantry ****************************
  * @file       gimbal_task.c/h
  * @brief      gimbal control task, because use the euler angle calculated by
  *             gyro sensor, range (-pi,pi), angle set-point must be in this
  *             range.gimbal has two control mode, gyro mode and enconde mode
  *             gyro mode: use euler angle to control, encond mode: use enconde
  *             angle to control. and has some special mode:cali mode, motionless
  *             mode.
  *             完成云台控制任务，由于云台使用陀螺仪解算出的角度，其范围在（-pi,pi）
  *             故而设置目标角度均为范围，存在许多对角度计算的函数。云台主要分为2种
  *             状态，陀螺仪控制状态是利用板载陀螺仪解算的姿态角进行控制，编码器控制
  *             状态是通过电机反馈的编码值控制的校准，此外还有校准状态，停止状态等。
  * @note
  * @history
  *  Version    Date            Author          Modification
  *  V1.0.0     Dec-26-2018     RM              1. done
  *  V1.1.0     Nov-11-2019     RM              1. add some annotation
  *
  * 
  *  V2.0.0     Otc-1-2025      xzicr           增加注释以及增加滑模控制器
  @verbatim
  ==============================================================================

  ==============================================================================
  @endverbatim
  ****************************(C)  2026 WHU_infantry ****************************
  */

#include "gimbal_task.h"
#include "main.h"
#include "cmsis_os.h"
#include "arm_math.h"
#include "CAN_receive.h"
#include "user_lib.h"
#include "detect_task.h"
#include "remote_control.h"
#include "INS_task.h"
#include "pid.h"
#include "Self_aim.h"
#include "stm32f4xx_hal.h"
#include "usart.h"
#include "smc.h"
#include "bsp_buzzer.h"
/**
 * @brief          遥控器的死区判断，因为遥控器的拨杆在中位的时候，不一定为0，
 * @param          输入的遥控器值
 * @param          输出的死区处理后遥控器值
 * @param          死区值
 */
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
// motor enconde value format, range[0-8191]
// 电机编码值规整 0—8191
#define ecd_format(ecd)    \
  {                        \
    if ((ecd) > ECD_RANGE) \
      (ecd) -= ECD_RANGE;  \
    else if ((ecd) < 0)    \
      (ecd) += ECD_RANGE;  \
  }


#if INCLUDE_uxTaskGetStackHighWaterMark
uint32_t gimbal_high_water;

#endif
float yaw_angle_limit_func(float input)
{
  if (input >= 180)
  {
    return input - 360;
  }
  else if (input <= -180)
  {
    return input + 360;
  }
  else
  {
    return input;
  }
}

#define WINDOW_SIZE 10
float yaw_buffer[WINDOW_SIZE];
int data_index = 0;

// pitch 前馈系数: 先设0, 再从小往大调
#define KFF_VEL   0.0f    // 角速度前馈 (电流 / 度每秒)
#define KFF_ACC   0.0f    // 角加速度前馈 (电流 / 度每秒平方)

float sliding_average(float new_yaw)
{
  yaw_buffer[data_index] = new_yaw;
  data_index = (data_index + 1) % WINDOW_SIZE;
  float sum = 0;
  for (int i = 0; i < WINDOW_SIZE; i++)
    sum += yaw_buffer[i];
  return sum / WINDOW_SIZE;
}


void System_Reset(void)
{
    __disable_irq();              // 建议：关闭全局中断
    HAL_NVIC_SystemReset();       // 执行系统复位
    while(1);                     // 复位成功后不会执行到这里
}


/**
 * @brief          初始化"gimbal_control"变量，包括pid初始化， 遥控器指针初始化，云台电机指针初始化，陀螺仪角度指针初始化
 * @param[out]     init:"gimbal_control"变量指针.
 * @retval         none
 */
static void gimbal_init(gimbal_control_t *init);

/**
 * @brief           处理发送底盘数据
 * @param[in]       gimbal_control_set: "gimbal_control" 变量指针     
 * @return          none
 * @note 
 */
void chassis_rc_to_control_vector(gimbal_control_t *gimbal_control_set, chassis_data_t *chassis_data);

/**
 * @brief          设置云台控制模式，主要在'gimbal_behaviour_mode_set'函数中改变
 * @param[out]     gimbal_set_mode:"gimbal_control"变量指针.
 * @retval         none
 */
static void gimbal_set_mode(gimbal_control_t *set_mode);

/**
 * @brief          底盘测量数据更新，包括电机速度，欧拉角度，机器人速度
 * @param[out]     gimbal_feedback_update:"gimbal_control"变量指针.
 * @retval         none
 //  */
 static void gimbal_feedback_update(gimbal_control_t *feedback_update);
 
 /**
  * @brief          计算ecd与offset_ecd之间的相对角度
  * @param[in]      ecd: 电机当前编码
  * @param[in]      offset_ecd: 电机中值编码
  * @retval         相对角度，单位rad
  */
 static fp32 motor_ecd_to_angle_change(int16_t encoder);
 
 /**
  * @brief          设置云台控制设定值，控制值是通过gimbal_behaviour_control_set函数设置的
  * @param[out]     gimbal_set_control:"gimbal_control"变量指针.
  * @retval         none
  */
 static void gimbal_set_control(gimbal_control_t *set_control);
 
 /**
  * @brief          控制循环，根据控制设定值，计算电机电流值，进行控制
  * @param[out]     gimbal_control_loop:"gimbal_control"变量指针.
  * @retval         none
  */
 static void gimbal_control_loop(gimbal_control_t *control_loop);
 
 //开/关电机状态位
 uint8_t start;
 uint8_t close;
 
// 滑膜结构体
SMC_t gimbal_smc;

//云台数据结构体
gimbal_control_t gimbal_control;

// 底盘数据结构体
chassis_data_t chassis_data;

//自瞄数据结构体指针
InputData *Self_aim_data;

//PID参数数组
static const fp32 Pitch_angle_pid[3] = {PITCH_GYRO_ABSOLUTE_PID_KP_1, PITCH_GYRO_ABSOLUTE_PID_KI_1, PITCH_GYRO_ABSOLUTE_PID_KD_1};

//滑膜控制器参数
const static fp32 pitch_smc[9]   = {J_GIMBAL_PITCH,   K_GIMBAL_PITCH,   0.0f,  C1_GIMBAL_PITCH, C2_GIMBAL_PITCH, 0.0f, 0.0f, 0.0f, epsilon_GIMBAL_PITCH};

//		  底盘模式				    转向		      自瞄  		标志位
uint8_t chassis_flag = 0, turnflag = 0,aimflag = 0 ,reset_flag = 0;

//小陀螺控制标志位
uint8_t l_topflag = 0,r_topflag = 0;

first_order_filter_type_t chassis_self_aim_yaw;
// const static fp32 chassis_self_aim_yaw_filter[1] = {CHASSIS_ACCEL_Y_NUM};

//斜坡函数结构体
ramp_function_source_t  ramp_vector_x;
ramp_function_source_t  ramp_vector_y;


//自瞄数据一阶低通滤波结构体
first_order_filter_type_t gimbal_control_pitch_filter;
first_order_filter_type_t gimbal_control_yaw_filter;

//滤波参数
const float alpha[1] = {0.0001f};

void gimbal_task(void const *pvParameters)
{
  // 等待陀螺仪任务更新陀螺仪数据
  vTaskDelay(GIMBAL_TASK_INIT_TIME);

  // 云台初始化
  gimbal_init(&gimbal_control);

	//射击初始化
  shoot_Init();

  buzzer_on(2,45535);//蜂鸣器指示初始化完成，它他妈竟然会自己关

  while (1)
  {
    //射击控制
    gimbal_control.shoot = shoot_control_loop();

    // 设置底盘控制量
    chassis_rc_to_control_vector(&gimbal_control, &chassis_data); 

    // 云台数据反馈
    gimbal_feedback_update(&gimbal_control);           
    
    // 设置云台控制模式
    gimbal_set_mode(&gimbal_control);   
    
    //射击控制
    gimbal_control.shoot = shoot_control_loop();

    // 设置云台PITCH轴目标角度
    gimbal_set_control(&gimbal_control);            

    // 云台控制计算
    gimbal_control_loop(&gimbal_control);          

    // pitch电机电流发送
    CAN_LK_SPEED_Control(1200, -1000.0f*gimbal_control.gimbal_pitch_motor.given_current);   //-57295.0f
    // CAN_LK_Torque_Control(-gimbal_control.gimbal_pitch_motor.given_current);

    //摩擦轮电机电流发送
    CAN_cmd_chassis(gimbal_control.shoot->shoot_left_given_current,gimbal_control.shoot->shoot_right_given_current, 0, 0);


    vTaskDelay(GIMBAL_CONTROL_TIME);
  }
}

/**
 * @brief          返回yaw 电机数据指针
 * @param[in]      none
 * @retval         yaw电机指针
 */
const gimbal_motor_t *get_yaw_motor_point(void)
{
  return &gimbal_control.gimbal_yaw_motor;
}

/**
 * @brief          返回pitch 电机数据指针
 * @param[in]      none
 * @retval         pitch
 */
const gimbal_motor_t *get_pitch_motor_point(void)
{
  return &gimbal_control.gimbal_pitch_motor;
}



static void gimbal_init(gimbal_control_t *init)
{
  // 陀螺仪数据指针获取
  init->INS = get_INS();
  init->gimbal_INT_angle_point = get_INS_angle_point();
  init->gimbal_INT_gyro_point = get_gyro_data_point();

  //遥控器指令获取
  init->gimbal_rc_ctrl = get_remote_control_point();

  //自瞄数据获取
  Self_aim_data = get_selfaim_data();

  //初始化模式设置
  init->gimbal_pitch_motor.gimbal_motor_mode = init->gimbal_yaw_motor.last_gimbal_motor_mode = GIMBAL_MOTOR_OFF;
  chassis_data.chassis_mode = CHASSIS_MODE_OFF;
  chassis_data.shoot_mode = 0;

  //初始化目标角度角速度
  init->gimbal_pitch_motor.absolute_angle_set = init->gimbal_pitch_motor.absolute_angle;
  init->gimbal_pitch_motor.motor_gyro_set = init->gimbal_pitch_motor.motor_gyro;
  chassis_data.yaw_angle_set = init->gimbal_yaw_motor.absolute_angle;

  //初始化PID结构体
  PID_init(&init->gimbal_pitch_motor.gimbal_motor_angle_pid_1, PID_POSITION, Pitch_angle_pid, PITCH_GYRO_ABSOLUTE_PID_MAX_OUT_1, PITCH_GYRO_ABSOLUTE_PID_MAX_IOUT_1);

  //初始化smc
   SMC_init(&gimbal_smc.pitch, pitch_smc, EISMC, SAT_LIMIT_GIMBAL_PITCH, U_MAX_GIMBAL_PITCH, POS_ESP_GIMBAL_PITCH);
  
  //数据更新
  gimbal_feedback_update(init);

  //底盘速度斜坡初始化
  ramp_init(&ramp_vector_x,0.02,8,-8);
  ramp_init(&ramp_vector_y,0.02,8,-8);

  //一阶低通滤波初始化
  first_order_filter_init(&gimbal_control_pitch_filter, 0.002, alpha);
  first_order_filter_init(&gimbal_control_yaw_filter, 0.002,alpha);




}


static void gimbal_set_mode(gimbal_control_t *set_mode)
{
  if (set_mode == NULL)
  {
    return;
  }

  if (set_mode->gimbal_pitch_motor.gimbal_motor_mode == GIMBAL_INIT)
  {

  }
  if (set_mode->gimbal_rc_ctrl->rc.s[0] == 0) //|| toe_is_error(DBUS_TOE))
  {
    set_mode->gimbal_pitch_motor.gimbal_motor_mode = GIMBAL_MOTOR_OFF;
  }
  else if (set_mode->gimbal_rc_ctrl->rc.s[0] == 1 || set_mode->gimbal_rc_ctrl->rc.s[0] == 2)
  {
    set_mode->gimbal_pitch_motor.gimbal_motor_mode = GIMBAL_MOTOR_GYRO;
  }

  // 判断进入init状态机
  if (set_mode->gimbal_pitch_motor.last_gimbal_motor_mode == GIMBAL_MOTOR_OFF && set_mode->gimbal_pitch_motor.gimbal_motor_mode != GIMBAL_MOTOR_OFF)
  {
    set_mode->gimbal_pitch_motor.gimbal_motor_mode = GIMBAL_INIT;
  }
  set_mode->gimbal_pitch_motor.last_gimbal_motor_mode = set_mode->gimbal_pitch_motor.gimbal_motor_mode;
}


static void gimbal_feedback_update(gimbal_control_t *feedback_update)
{
  if (feedback_update == NULL)
  {
    return;
  }
  
  // 云台数据更新
  feedback_update->gimbal_pitch_motor.relative_angle = motor_ecd_to_angle_change(feedback_update->gimbal_pitch_motor.gimbal_motor_measure->last_encoder);
  feedback_update->gimbal_pitch_motor.absolute_angle = *(feedback_update->gimbal_INT_angle_point + INS_PITCH_ADDRESS_OFFSET);
  feedback_update->gimbal_pitch_motor.motor_gyro = *(feedback_update->gimbal_INT_gyro_point + INS_GYRO_Y_ADDRESS_OFFSET);
  feedback_update->gimbal_yaw_motor.relative_angle = motor_ecd_to_angle_change(feedback_update->gimbal_yaw_motor.gimbal_motor_measure->last_encoder);
  feedback_update->gimbal_yaw_motor.absolute_angle = INS.YawTotalAngle;
  feedback_update->gimbal_yaw_motor.motor_gyro = arm_cos_f32(feedback_update->gimbal_pitch_motor.relative_angle) * (*(feedback_update->gimbal_INT_gyro_point + INS_GYRO_Z_ADDRESS_OFFSET)) - arm_sin_f32(feedback_update->gimbal_pitch_motor.relative_angle) * (*(feedback_update->gimbal_INT_gyro_point + INS_GYRO_X_ADDRESS_OFFSET));
  
  // 键鼠数据获取
  feedback_update->last_press_l = feedback_update->press_l;
  feedback_update->press_l = feedback_update->gimbal_rc_ctrl->mouse.press_l;
  feedback_update->last_press_r = feedback_update->press_r;
  feedback_update->press_r = feedback_update->gimbal_rc_ctrl->mouse.press_r;
  feedback_update->lastkeyboard = feedback_update->keyboard;
  feedback_update->keyboard = feedback_update->gimbal_rc_ctrl->key.v;
  feedback_update->last_press_self_aim = feedback_update->press_self_aim;
  feedback_update->press_self_aim = feedback_update->gimbal_rc_ctrl->rc.s[2];

  // 自瞄数据获取  自瞄数据滤波计算，目前没有用到，如果有用到要改成角度制
   //first_order_filter_cali(&gimbal_control_pitch_filter,Self_aim_data->pitch*180.0f/PI);
   //first_order_filter_cali(&gimbal_control_yaw_filter,Self_aim_data->yaw*180.0f/PI);

  feedback_update->gimbal_pitch_motor.self_aim_pitch_angle =  Self_aim_data->pitch;//*180.0f/PI;  //gimbal_control_pitch_filter.out;
  feedback_update->gimbal_yaw_motor.self_aim_yaw_angle = Self_aim_data->yaw;//*180.0f/PI;

}


/**
 * @brief          计算ecd与offset_ecd之间的相对角度
 * @param[in]      ecd: 电机当前编码
 * @param[in]      offset_ecd: 电机中值编码
 * @retval         相对角度，单位rad
 */
static fp32 motor_ecd_to_angle_change(int16_t last_encoder)
{
  //    int32_t relative_ecd = ecd - offset_ecd;
  //    if (relative_ecd > HALF_ECD_RANGE)
  //    {
  //        relative_ecd -= ECD_RANGE;
  //    }
  //    else if (relative_ecd < -HALF_ECD_RANGE)
  //    {
  //        relative_ecd += ECD_RANGE;
  //    }
  float angle = (float)last_encoder * MOTOR_ECD_TO_RAD;
  return angle;
}

/**
 * @brief          设置云台PITCH轴目标角度
 * @param[out]     gimbal_control_t
 * @retval         none
 */
static void gimbal_set_control(gimbal_control_t *set_control)
{
  static int16_t pitch_channel = 0;
  rc_deadband_limit(set_control->gimbal_rc_ctrl->rc.ch[PITCH_CHANNEL], pitch_channel, RC_DEADBAND);
  if (set_control->gimbal_pitch_motor.gimbal_motor_mode == GIMBAL_MOTOR_GYRO && aimflag == 1)
  {
    if(set_control->gimbal_pitch_motor.self_aim_pitch_angle == 0)         // || Self_aim_data->fire_advicec == 0) //如果识别不到装甲板，就不动
    {
      set_control->gimbal_pitch_motor.absolute_angle_set = set_control->gimbal_pitch_motor.absolute_angle_set;
    }
    else
    {
      set_control->gimbal_pitch_motor.absolute_angle_set = set_control->gimbal_pitch_motor.self_aim_pitch_angle;// - set_control->gimbal_rc_ctrl->mouse.y * PITCH_MOUSE_SEN * 0.5;}
    }
    set_control->gimbal_pitch_motor.absolute_angle_set = fp32_constrain(set_control->gimbal_pitch_motor.absolute_angle_set, -27, 19 );//限幅，低头角度为正
  }
  else if (set_control->gimbal_pitch_motor.gimbal_motor_mode == GIMBAL_MOTOR_GYRO && aimflag == 0)
  {
    set_control->gimbal_pitch_motor.absolute_angle_set -= pitch_channel * PITCH_RC_SEN + set_control->gimbal_rc_ctrl->mouse.y * PITCH_MOUSE_SEN;
    set_control->gimbal_pitch_motor.absolute_angle_set = fp32_constrain(set_control->gimbal_pitch_motor.absolute_angle_set, -27, 19 );//限幅  
  }
  /* 初始化云台pitch轴角度 */
  else if (set_control->gimbal_pitch_motor.gimbal_motor_mode == GIMBAL_INIT)
  {
    set_control->gimbal_pitch_motor.absolute_angle_set = set_control->gimbal_pitch_motor.absolute_angle;//INIT_PITCH_SET;
    aimflag = 0;
  }
  else 
  {
    set_control->gimbal_pitch_motor.absolute_angle_set = set_control->gimbal_pitch_motor.absolute_angle;
  }
}


/**
 * @brief          控制循环，根据控制设定值，计算电机电流值，进行控制
 * @param[out]     gimbal_control_loop:"gimbal_control"变量指针.
 * @retval         none
 */
static void gimbal_control_loop(gimbal_control_t *control_loop)
{
  if (control_loop->gimbal_pitch_motor.gimbal_motor_mode == GIMBAL_MOTOR_OFF)
  {
    if(close==0)
    {
      CAN_LK_CLOSE_control();
      close=1;
    }
    else if(close==1)
    {
      // CAN_LK_Torque_Control(0);
      start=0;
    }
  }
  else if (control_loop->gimbal_pitch_motor.gimbal_motor_mode == GIMBAL_MOTOR_GYRO)
  {
        //PID控制环
    PID_calc(&control_loop->gimbal_pitch_motor.gimbal_motor_angle_pid_1,
             control_loop->gimbal_pitch_motor.absolute_angle,
             control_loop->gimbal_pitch_motor.absolute_angle_set);

    //前馈: 只在自瞄模式下生效
    float ff = 0.0f;
    if (aimflag == 1)
    {
        ff = KFF_VEL * Self_aim_data->pitch_vel + KFF_ACC * Self_aim_data->pitch_acc;
    }
    control_loop->gimbal_pitch_motor.given_current =
        control_loop->gimbal_pitch_motor.gimbal_motor_angle_pid_1.out + ff;
    //滑膜控制环
    // SMC_posErrorUpdate(&gimbal_smc.pitch, control_loop->gimbal_pitch_motor.absolute_angle_set, control_loop->gimbal_pitch_motor.absolute_angle,control_loop->gimbal_pitch_motor.motor_gyro);
    // control_loop->gimbal_pitch_motor.given_current = SMC_calc(&gimbal_smc.pitch);

    //PID控制环
   // PID_calc(&control_loop->gimbal_pitch_motor.gimbal_motor_angle_pid_1, control_loop->gimbal_pitch_motor.absolute_angle, control_loop->gimbal_pitch_motor.absolute_angle_set);
    //control_loop->gimbal_pitch_motor.given_current = control_loop->gimbal_pitch_motor.gimbal_motor_angle_pid_1.out;
  }

  else if (control_loop->gimbal_pitch_motor.gimbal_motor_mode == GIMBAL_INIT)
  {
    if (start == 0)
    {
      CAN_LK_START_control();

      close = 0;
      start = 1;
    }
    if (start == 1)
    {
      //PID控制环
      PID_calc(&control_loop->gimbal_pitch_motor.gimbal_motor_angle_pid_1, control_loop->gimbal_pitch_motor.absolute_angle, control_loop->gimbal_pitch_motor.absolute_angle_set);
      control_loop->gimbal_pitch_motor.given_current = control_loop->gimbal_pitch_motor.gimbal_motor_angle_pid_1.out;


      //滑膜控制环
      // SMC_posErrorUpdate(&gimbal_smc.pitch, control_loop->gimbal_pitch_motor.absolute_angle_set, control_loop->gimbal_pitch_motor.absolute_angle,control_loop->gimbal_pitch_motor.motor_gyro);
      // control_loop->gimbal_pitch_motor.given_current = SMC_calc(&gimbal_smc.pitch);
    }
  } 
}


void chassis_rc_to_control_vector(gimbal_control_t *gimbal_control_set, chassis_data_t *chassis_data)
{
  /* --------------进入函数前提条件------------------ */
  if (gimbal_control_set == NULL)
  {
    return;
  }
  if (chassis_data->chassis_mode == CHASSIS_MODE_INIT)
  {
    if ( fabs(gimbal_control_set->gimbal_pitch_motor.absolute_angle - INIT_PITCH_SET) < GIMBAL_INIT_ANGLE_ERROR)
    {
      return;
    }
  }

  /* --------------遥控器 键鼠数据处理------------------ */
  int16_t vx_channel, vy_channel;
  fp32 vx_set_channel, vy_set_channel;
  static int16_t yaw_channel = 0;
  const static fp32 chassis_x_order_filter[1] = {CHASSIS_ACCEL_X_NUM};
  const static fp32 chassis_y_order_filter[1] = {CHASSIS_ACCEL_Y_NUM};
  rc_deadband_limit(gimbal_control_set->gimbal_rc_ctrl->rc.ch[CHASSIS_X_CHANNEL], vx_channel, CHASSIS_RC_DEADLINE);
  rc_deadband_limit(gimbal_control_set->gimbal_rc_ctrl->rc.ch[CHASSIS_Y_CHANNEL], vy_channel, CHASSIS_RC_DEADLINE);
  rc_deadband_limit(gimbal_control_set->gimbal_rc_ctrl->rc.ch[YAW_CHANNEL], yaw_channel, RC_DEADBAND);
  vx_set_channel = ramp_calc_vector(&ramp_vector_x, vx_channel * CHASSIS_VX_RC_SEN) ;
  vy_set_channel = ramp_calc_vector(&ramp_vector_y,vy_channel * -CHASSIS_VY_RC_SEN);

  if (gimbal_control_set->gimbal_rc_ctrl->key.v & KEY_PRESSED_OFFSET_W)
  {
    vx_set_channel = NORMAL_MOVE_SPEED;
  }
  else if (gimbal_control_set->gimbal_rc_ctrl->key.v & KEY_PRESSED_OFFSET_S)
  {
    vx_set_channel = -NORMAL_MOVE_SPEED;
  }
  if (gimbal_control_set->gimbal_rc_ctrl->key.v & KEY_PRESSED_OFFSET_A)
  {
    vy_set_channel = NORMAL_MOVE_SPEED;
  }
  else if (gimbal_control_set->gimbal_rc_ctrl->key.v & KEY_PRESSED_OFFSET_D)
  {
    vy_set_channel = -NORMAL_MOVE_SPEED;
  }

  
  if (gimbal_control_set->gimbal_rc_ctrl->key.v & KEY_PRESSED_OFFSET_W && gimbal_control_set->gimbal_rc_ctrl->key.v & KEY_PRESSED_OFFSET_SHIFT)
  {
    vx_set_channel = SHIFT_MOVE_SPEED;
  }
  else if (gimbal_control_set->gimbal_rc_ctrl->key.v & KEY_PRESSED_OFFSET_S && gimbal_control_set->gimbal_rc_ctrl->key.v & KEY_PRESSED_OFFSET_SHIFT)
  {
    vx_set_channel = -SHIFT_MOVE_SPEED;
  }
  if (gimbal_control_set->gimbal_rc_ctrl->key.v & KEY_PRESSED_OFFSET_A && gimbal_control_set->gimbal_rc_ctrl->key.v & KEY_PRESSED_OFFSET_SHIFT)
  {
    vy_set_channel = SHIFT_MOVE_SPEED;
  }
  else if (gimbal_control_set->gimbal_rc_ctrl->key.v & KEY_PRESSED_OFFSET_D && gimbal_control_set->gimbal_rc_ctrl->key.v & KEY_PRESSED_OFFSET_SHIFT)
  {
    vy_set_channel = -SHIFT_MOVE_SPEED;
  }

  
  // 一阶低通滤波代替斜波作为底盘速度输入
  first_order_filter_init(&chassis_data->chassis_cmd_slow_set_vx, GIMBAL_CONTROL_TIME, chassis_x_order_filter);
  first_order_filter_init(&chassis_data->chassis_cmd_slow_set_vy, GIMBAL_CONTROL_TIME, chassis_y_order_filter);
  first_order_filter_cali(&chassis_data->chassis_cmd_slow_set_vx, vx_set_channel);
  first_order_filter_cali(&chassis_data->chassis_cmd_slow_set_vy, vy_set_channel);
  // 停止信号，不需要缓慢加速，直接减速到零
  if (vx_set_channel < CHASSIS_RC_DEADLINE * CHASSIS_VX_RC_SEN && vx_set_channel > -CHASSIS_RC_DEADLINE * CHASSIS_VX_RC_SEN)
  {
    chassis_data->chassis_cmd_slow_set_vx.out = 0.0f;
  }

  if (vy_set_channel < CHASSIS_RC_DEADLINE * CHASSIS_VY_RC_SEN && vy_set_channel > -CHASSIS_RC_DEADLINE * CHASSIS_VY_RC_SEN)
  {
    chassis_data->chassis_cmd_slow_set_vy.out = 0.0f;
  }
  
  //遥控器左上角按键开自瞄
  if (gimbal_control_set -> last_press_self_aim == 0 && gimbal_control_set->press_self_aim == 1)
  {
    aimflag = !aimflag;
  }
  //鼠标右键长按开自瞄，松开关自瞄
  if(gimbal_control_set-> press_r && gimbal_control_set-> last_press_r)
  {
    aimflag = 1;
  }
  else if(gimbal_control_set-> press_r == 0 && gimbal_control_set-> last_press_r == 1)
  {
    aimflag = 0;
  }

  if ((gimbal_control_set->keyboard & KEY_PRESSED_OFFSET_CTRL) && (gimbal_control_set->lastkeyboard & KEY_PRESSED_OFFSET_CTRL))
  {
    reset_flag = 1;
  }

  /*有关小陀螺的控制*/
  // chassis_data->wz_set = CHASSIS_WZ_RC_SEN * gimbal_control_set->gimbal_rc_ctrl->rc.ch[CHASSIS_WZ_CHANNEL];

  if (gimbal_control_set->keyboard & KEY_PRESSED_OFFSET_Q && (gimbal_control_set->lastkeyboard & KEY_PRESSED_OFFSET_Q) == 0)
  {
    l_topflag = !l_topflag; 
  }

  if(l_topflag == 1)       { chassis_data->wz_set = -NORMAL_LITTLETOP_SPEED + CHASSIS_WZ_RC_SEN * gimbal_control_set->gimbal_rc_ctrl->rc.ch[CHASSIS_WZ_CHANNEL]; }
  else if(l_topflag == 0)  { chassis_data->wz_set = CHASSIS_WZ_RC_SEN * gimbal_control_set->gimbal_rc_ctrl->rc.ch[CHASSIS_WZ_CHANNEL]; }

  chassis_data->wz_set = fp32_constrain(chassis_data->wz_set,-9.0f,9.0f);

  /* ---------------------------底盘数据传递------------------------------- */
  if (gimbal_control_set->gimbal_rc_ctrl->rc.s[0] == 0) //C挡
  {
    chassis_data->chassis_mode = CHASSIS_MODE_OFF;
  }
  else if (gimbal_control_set->gimbal_rc_ctrl->rc.s[0] == 1)  //N挡
  {
    chassis_data->chassis_mode = CHASSIS_MODE_NO_FOLLOW;
  }
  else if (gimbal_control_set->gimbal_rc_ctrl->rc.s[0] == 2)  //S挡
  {
    chassis_data->chassis_mode = CHASSIS_MODE_FOLLOW;
  }
  if (chassis_data->last_chassis_mode == CHASSIS_MODE_OFF && chassis_data->chassis_mode != CHASSIS_MODE_OFF)
  {
    chassis_data->chassis_mode = CHASSIS_MODE_INIT;
  }

  chassis_data->vx_set = chassis_data->chassis_cmd_slow_set_vx.out;
  chassis_data->vy_set = chassis_data->chassis_cmd_slow_set_vy.out;

  chassis_data->shoot_mode = gimbal_control_set->gimbal_rc_ctrl->mouse.press_l << 4 | shoot_control.shoot_mode | aimflag << 5;
  chassis_data->yaw_angle = gimbal_control_set->gimbal_yaw_motor.absolute_angle;
  chassis_data->yaw_gyro = gimbal_control_set->gimbal_yaw_motor.motor_gyro;
  
  if (chassis_data->chassis_mode == CHASSIS_MODE_INIT)
  {
    chassis_data->yaw_angle_set = chassis_data->yaw_angle;
  }
  else if (chassis_data->chassis_mode == CHASSIS_MODE_OFF)
  {
    chassis_data->yaw_angle_set = chassis_data->yaw_angle;
  }

  if ((chassis_data->chassis_mode == CHASSIS_MODE_NO_FOLLOW||chassis_data->chassis_mode == CHASSIS_MODE_FOLLOW) && aimflag == 0)
  {
     chassis_data->yaw_vel = 0.0f;   // 丢目标: 不给前馈
       chassis_data->yaw_acc = 0.0f;//新增，防止手动残留值
    chassis_data->yaw_angle_set -= yaw_channel * YAW_RC_SEN + gimbal_control_set->gimbal_rc_ctrl->mouse.x * YAW_MOUSE_SEN;
  }
  else if ((chassis_data->chassis_mode == CHASSIS_MODE_FOLLOW || chassis_data->chassis_mode == CHASSIS_MODE_NO_FOLLOW) && aimflag == 1)
  {
    if(gimbal_control_set->gimbal_yaw_motor.self_aim_yaw_angle == 0)    // || Self_aim_data->fire_advicec == 0)  
    {
       chassis_data->yaw_angle_set = chassis_data->yaw_angle_set ;
        chassis_data->yaw_vel = 0.0f;   // 丢目标: 不给前馈
       chassis_data->yaw_acc = 0.0f;//新增
    }
    else
    {chassis_data->yaw_angle_set = gimbal_control_set->gimbal_yaw_motor.self_aim_yaw_angle;
     chassis_data->yaw_vel = Self_aim_data->yaw_vel;   // 装进包裹
       chassis_data->yaw_acc = Self_aim_data->yaw_acc;//新增
  }
  }
  chassis_data->last_chassis_mode = chassis_data->chassis_mode;
  vTaskDelay(8);
}
/**
 * @brief          返回底盘数据指针供CAN_task任务使用
 * @param[out]     none
 * @retval         none
 */
chassis_data_t *get_chassis_data_point(void)
{
  return &chassis_data;
}
gimbal_control_t *get_gimbal_data(void)
{
  return &gimbal_control;
}
