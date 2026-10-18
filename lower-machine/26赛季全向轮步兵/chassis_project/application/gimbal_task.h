#ifndef GIMBAL_TASK_H
#define GIMBAL_TASK_H
#include "main.h"
#include "cmsis_os.h"
#include "CAN_receive.h"
#include "pid.h"
#include "shoot.h"
#include "user_lib.h"
#include "smc.h"

//SMC parameters ---------------------
//AIM YAW
#define J_GIMBAL_AIM_YAW (0.3f)
#define K_GIMBAL_AIM_YAW (0.1f)
#define C_GIMBAL_AIM_YAW (0.003f)
#define epsilon_GIMBAL_AIM_YAW (0.5f)

#define SAT_LIMIT_GIMBAL_AIM_YAW (1)
#define POS_ESP_GIMBAL_AIM_YAW (0.001f)
#define U_MAX_GIMBAL_AIM_YAW (15000)

//PITCH轴SMC参数
#define J_GIMBAL_PITCH (4.0f)
#define K_GIMBAL_PITCH (200.0f)
#define C1_GIMBAL_PITCH (30.0f)
#define C2_GIMBAL_PITCH (30.0f)
#define epsilon_GIMBAL_PITCH (0.1f)

#define SAT_LIMIT_GIMBAL_PITCH (1)
#define POS_ESP_GIMBAL_PITCH (0.001f)
#define U_MAX_GIMBAL_PITCH (10000)



#define GIMBAL_CONTROL_TIME 2


/*云台yaw轴电机角度环PID参数 */
#define YAW_ANGLE_PID_KP 27.3f          //28.3
#define YAW_ANGLE_PID_KI 0.002f           //0.01
#define YAW_ANGLE_PID_KD 2.0f            //3.0

#define YAW_ANGLE_PID_MAX_OUT 1500.0f      // 3
#define YAW_ANGLE_PID_MAX_IOUT 400.0f      // 0.5

/*云台yaw轴电机速度环PID参数 */
#define YAW_SPEED_PID_KP 0.60f                //0.41f
#define YAW_SPEED_PID_KI 0.00f              //0.001
#define YAW_SPEED_PID_KD 0.7f                 //0.8
#define YAW_SPEED_PID_MAX_OUT 1500.0f      
#define YAW_SPEED_PID_MAX_IOUT 100.0f      



typedef struct
{
    Sliding yaw;
    Sliding pitch;
} SMC_t;

typedef enum
{
    GIMBAL_MOTOR_OFF = 0, // 电机原始值控制
    GIMBAL_MOTOR_GYRO,    // 电机陀螺仪角度控制
    GIMBAL_MOTOR_ROTATE,
    GIMBAL_MOTOR_INIT,   
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
    const motor_measure_t *gimbal_motor_measure;
    const lkmotor_measure_t *gimbal_lkmotor_measure;
	gimbal_PID_t gimbal_motor_angle1_pid;

    gimbal_PID_t gimbal_motor_speed_pid;
    pid_type_def gimbal_motor_gyro_pid;
    gimbal_motor_mode_e gimbal_motor_mode;
    fp32 absolute_angle;     //rad
    fp32 absolute_angle_set; //rad
	// fp32 absolute_angle_set1;
    fp32 relative_angle;
    fp32 relative_angle_set;
    fp32 motor_gyro;         //rad/s
    fp32 motor_gyro_set;
    fp32 motor_speed;
    fp32 current_set;
    fp32 yaw_given_current;
	first_order_filter_type_t *yaw_angle_set;
} gimbal_motor_t;
typedef struct
{
    const chassis_data_t  *yaw_ctrl_data;
    gimbal_motor_t gimbal_yaw_motor;
	shoot_control_t *shoot;
    uint8_t YAW_INIT_FLAG;

} gimbal_control_t;


extern gimbal_control_t gimbal_control;


void gimbal_task(void);
//extern float data[2];
#endif
