/**
  ****************************(C) COPYRIGHT 2019 DJI****************************
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
  @verbatim
  ==============================================================================

  ==============================================================================
  @endverbatim
  ****************************(C) COPYRIGHT 2019 DJI****************************
  */

#ifndef GIMBAL_TASK_H
#define GIMBAL_TASK_H
#include "struct_typedef.h"
#include "CAN_receive.h"
#include "pid.h"
#include "remote_control.h"
#include "shoot.h"
#include "INS_task.h"

#include "smc.h"
//pitch speed close-loop PID params, max out and max iout
//pitch 速度环 PID参数以及 PID最大输出，积分输出
#define PITCH_SPEED_PID_KP        0.16f
#define PITCH_SPEED_PID_KI        0.8f
#define PITCH_SPEED_PID_KD        0.001f
#define PITCH_SPEED_PID_MAX_OUT   3000.0f
#define PITCH_SPEED_PID_MAX_IOUT  10.0f

//pitch gyro angle close-loop PID params, max out and max iout
//pitch 角度环 角度由陀螺仪解算 PID参数以及 PID最大输出，积分输出
#define PITCH_GYRO_ABSOLUTE_PID_KP_1       9.35f                        		//8.4    
#define PITCH_GYRO_ABSOLUTE_PID_KI_1       0.06f                           //0.006
#define PITCH_GYRO_ABSOLUTE_PID_KD_1       0.75f                             //0.5

#define PITCH_GYRO_ABSOLUTE_PID_MAX_OUT_1   1500.0f        //15.0f
#define PITCH_GYRO_ABSOLUTE_PID_MAX_IOUT_1     400.0f     // 0.0f






//任务初始化 空闲一段时间
#define GIMBAL_TASK_INIT_TIME    100           //201
//yaw,pitch控制通道以及状态开关通道
#define YAW_CHANNEL   0
#define PITCH_CHANNEL 1
#define GIMBAL_MODE_CHANNEL 0
//turn 180°
//掉头180 按键
#define TURN_KEYBOARD KEY_PRESSED_OFFSET_F
//turn speed
//掉头云台速度
#define TURN_SPEED    0.04f
//测试按键尚未使用
#define TEST_KEYBOARD KEY_PRESSED_OFFSET_R
//rocker value deadband
//遥控器输入死区，因为遥控器存在差异，摇杆在中间，其值不一定为零
#define RC_DEADBAND   10

#define YAW_RC_SEN    0.0030f //0.0030f
#define PITCH_RC_SEN  0.0015f //0.003

#define YAW_MOUSE_SEN   0.009f
#define PITCH_MOUSE_SEN 0.006f

#define YAW_ENCODE_SEN    0.01f
#define PITCH_ENCODE_SEN  0.01f

#define GIMBAL_CONTROL_TIME 1


#define PITCH_TURN  1
#define YAW_TURN    0

//电机码盘值最大以及中值
#define HALF_ECD_RANGE  4096
#define ECD_RANGE       8191
//云台初始化回中值，允许的误差,并且在误差范围内停止一段时间以及最大时间6s后解除初始化状态，
#define GIMBAL_INIT_ANGLE_ERROR     0.1f
#define GIMBAL_INIT_STOP_TIME       100
#define GIMBAL_INIT_TIME            6000
#define GIMBAL_CALI_REDUNDANT_ANGLE 0.1f
//云台初始化回中值的速度以及控制到的角度
#define GIMBAL_INIT_PITCH_SPEED     0.004f
#define GIMBAL_INIT_YAW_SPEED       0.005f

#define INIT_YAW_SET    0.0f
#define INIT_PITCH_SET  20.0f

//云台校准中值的时候，发送原始电流值，以及堵转时间，通过陀螺仪判断堵转
#define GIMBAL_CALI_MOTOR_SET   8000
#define GIMBAL_CALI_STEP_TIME   2000
#define GIMBAL_CALI_GYRO_LIMIT  0.1f

#define GIMBAL_CALI_PITCH_MAX_STEP  1
#define GIMBAL_CALI_PITCH_MIN_STEP  2
#define GIMBAL_CALI_YAW_MAX_STEP    3
#define GIMBAL_CALI_YAW_MIN_STEP    4

#define GIMBAL_CALI_START_STEP  GIMBAL_CALI_PITCH_MAX_STEP
#define GIMBAL_CALI_END_STEP    5

//判断遥控器无输入的时间以及遥控器无输入判断，设置云台yaw回中值以防陀螺仪漂移
#define GIMBAL_MOTIONLESS_RC_DEADLINE 10
#define GIMBAL_MOTIONLESS_TIME_MAX    3000

//电机编码值转化成角度值
#ifndef MOTOR_ECD_TO_RAD
#define MOTOR_ECD_TO_RAD           0.000095873799f                             // 0.000766990394f //      2*  PI  /8192
#endif
//有关底盘的定义

//遥控器前进摇杆（max 660）转化成车体前进速度（m/s）的比例
#define CHASSIS_VX_RC_SEN    0.008f                 //0.010f
//遥控器左右摇杆（max 660）转化成车体左右速度（m/s）的比例
#define CHASSIS_VY_RC_SEN    0.008f                //0.005f
//遥控器左摇杆死区
#define CHASSIS_RC_DEADLINE 30            
//the channel num of controlling vertial speed 
//前后的遥控器通道号码
#define CHASSIS_X_CHANNEL 2
//the channel num of controlling horizontal speed
//左右的遥控器通道号码
#define CHASSIS_Y_CHANNEL 3
//chassi forward, back, left, right key
//底盘前后左右控制按键
#define CHASSIS_FRONT_KEY KEY_PRESSED_OFFSET_W
#define CHASSIS_BACK_KEY KEY_PRESSED_OFFSET_S
#define CHASSIS_LEFT_KEY KEY_PRESSED_OFFSET_A
#define CHASSIS_RIGHT_KEY KEY_PRESSED_OFFSET_D
#define CHASSIS_ACCEL_X_NUM 0.1666666667f
#define CHASSIS_ACCEL_Y_NUM 0.3333333333f
#define CHASSIS_CONTROL_TIME 0.002f
#define CHASSIS_WZ_RC_SEN 0.015f
#define CHASSIS_WZ_CHANNEL 4
#define INS_GYRO_X_ADDRESS_OFFSET 0
#define INS_GYRO_Y_ADDRESS_OFFSET 1
#define INS_GYRO_Z_ADDRESS_OFFSET 2



/*以下是键鼠操作时的相关宏定义*/

#define NORMAL_MOVE_SPEED 1.0f
#define SHIFT_MOVE_SPEED 3.0f

#define NORMAL_LITTLETOP_SPEED 5.0f
#define SHIFT_LITTLETOP_SPEED 9.0f



















//SMC parameters ---------------------
//AIM YAW
#define J_GIMBAL_AIM_YAW (3.0f)
#define K_GIMBAL_AIM_YAW (200.0f)
#define C_GIMBAL_AIM_YAW (30.0f)
#define epsilon_GIMBAL_AIM_YAW (0.5f)
#define SAT_LIMIT_GIMBAL_AIM_YAW (1)
#define POS_ESP_GIMBAL_AIM_YAW (0.001f)
#define U_MAX_GIMBAL_AIM_YAW (32.0f)

//PITCH轴SMC参数
#define J_GIMBAL_PITCH (3.0f)
#define K_GIMBAL_PITCH (10.0f)
#define C1_GIMBAL_PITCH (0.1f)
#define C2_GIMBAL_PITCH (0.1f)
#define epsilon_GIMBAL_PITCH (0.01f)
#define SAT_LIMIT_GIMBAL_PITCH (1)
#define POS_ESP_GIMBAL_PITCH (0.001f)
#define U_MAX_GIMBAL_PITCH (32.0f)


typedef struct
{
    Sliding aim_yaw;
    Sliding pitch;
} SMC_t;


typedef enum
{
    GIMBAL_MOTOR_OFF = 0, //电机原始值控制
    GIMBAL_MOTOR_GYRO,    //电机陀螺仪角度控制
    GIMBAL_MOTOR_ENCONDE, //电机编码值角度控制
    GIMBAL_INIT,          //电机初始化
} gimbal_motor_mode_e;

typedef struct
{
    fp32 kp;
    fp32 ki;
    fp32 kd;

    fp32 set;
    fp32 get;
    fp32 err;

    fp32 max_out;
    fp32 max_iout;

    fp32 Pout;
    fp32 Iout;
    fp32 Dout;

    fp32 out;
} gimbal_PID_t;

typedef struct
{
  const lkmotor_measure_t *gimbal_motor_measure;
  //    const motor_measure_t *gimbal_motor_measure;
  gimbal_PID_t gimbal_motor_absolute_angle_pid;
  pid_type_def gimbal_motor_angle_pid_1;
  pid_type_def gimbal_motor_gyro_pid;
  gimbal_motor_mode_e gimbal_motor_mode;
  gimbal_motor_mode_e last_gimbal_motor_mode;
  fp32 max_relative_angle; // rad
  fp32 min_relative_angle; // rad

  fp32 relative_angle;     // rad
  fp32 relative_angle_set; // rad
  fp32 absolute_angle;     // rad
  fp32 absolute_angle_set; // rad
  fp32 motor_gyro;         // rad/s
  fp32 motor_gyro_set;
  fp32 motor_speed;
  fp32 raw_cmd_current;
  fp32 current_set;
  int16_t given_current;
  float self_aim_pitch_angle;
  float last_self_aim_pitch_angle;
  float self_aim_yaw_angle;
  float last_self_aim_yaw_angle;

} gimbal_motor_t;

typedef struct
{
    fp32 max_yaw;
    fp32 min_yaw;
    fp32 max_pitch;
    fp32 min_pitch;
    uint16_t max_yaw_ecd;
    uint16_t min_yaw_ecd;
    uint16_t max_pitch_ecd;
    uint16_t min_pitch_ecd;
    uint8_t step;
} gimbal_step_cali_t;

typedef struct
{
  const RC_ctrl_t *gimbal_rc_ctrl;
  const fp32 *gimbal_INT_angle_point;
  const fp32 *gimbal_INT_gyro_point;
  const INS_t *INS;
  gimbal_motor_t gimbal_pitch_motor;
  gimbal_motor_t gimbal_yaw_motor;
  const shoot_control_t *shoot;
  gimbal_step_cali_t gimbal_cali;
  bool_t press_l;
  bool_t last_press_l;
  uint8_t press_r;
  uint8_t last_press_r;
  uint16_t keyboard;
  uint16_t lastkeyboard;
  bool_t last_press_self_aim,press_self_aim;
  uint8_t YAW_INIT_FLAG;
} gimbal_control_t;

extern uint8_t  aimflag ;

/**
  * @brief          return yaw motor data point
  * @param[in]      none
  * @retval         yaw motor data point
  */
/**
  * @brief          返回yaw 电机数据指针
  * @param[in]      none
  * @retval         yaw电机指针
  */
extern const gimbal_motor_t *get_yaw_motor_point(void);
/**
  * @brief          返回底盘控制数据指针
  * @param[in]      none
  * @retval         yaw电机指针
  */
extern const gimbal_motor_t *get_yaw_motor_point(void);
/**
  * @brief          return pitch motor data point
  * @param[in]      none
  * @retval         pitch motor data point
  */
/**
  * @brief          返回pitch 电机数据指针
  * @param[in]      none
  * @retval         pitch
  */
extern const gimbal_motor_t *get_pitch_motor_point(void);

/**
  * @brief          gimbal task, osDelay GIMBAL_CONTROL_TIME (1ms) 
  * @param[in]      pvParameters: null
  * @retval         none
  */
/**
  * @brief          云台任务，间隔 GIMBAL_CONTROL_TIME 1ms
  * @param[in]      pvParameters: 空
  * @retval         none
  */

extern void gimbal_task(void const *pvParameters);
/**
  * @brief          返回底盘数据指针供CAN_task任务使用
  * @param[out]     none
  * @retval         none
  */
extern chassis_data_t *get_chassis_data_point(void);
extern gimbal_control_t *get_gimbal_data(void);
extern void System_Reset(void);

extern uint8_t aimflag;
extern uint8_t reset_flag;

#endif
