#include "main.h"
#include "cmsis_os.h"
#include "CAN_receive.h"
#include "pid.h"
#include "gimbal_task.h"
#include "shoot.h"
#include "arm_math.h"
#include "user_lib.h"
#include "math.h"
#include "chassis_task.h"

#include "smc.h"

#define PID_CONTROL   1

#define KFF_VEL_YAW   0.0f    // 速度前馈 (rad/s -> 速度环目标)
#define KFF_ACC_YAW   0.0f    // 加速度前馈 (rad/s? -> 电流 iq) 新增部分


//云台 底盘数据结构体
gimbal_control_t gimbal_control;
extern chassis_move_t chassis_move;

//滑膜控制器参数
SMC_t gimbal_smc;

//开关LK电机标志位
uint8_t start;
uint8_t close;

//滑膜控制器参数
fp32 yaw_smc[9] = {J_GIMBAL_AIM_YAW, K_GIMBAL_AIM_YAW, C_GIMBAL_AIM_YAW, 0.0f,            0.0f,            0.0f, 0.0f, 0.0f, epsilon_GIMBAL_AIM_YAW};



float yaw_angle_limit_func(float input) 
{
    if (input >= 180) {
        return input - 360;
    } else if (input <= -180) {
        return input + 360;
    } else {
        return input;
    }
}	

void System_Reset(void)
{
    __disable_irq();              // 建议：关闭全局中断
    HAL_NVIC_SystemReset();       // 执行系统复位
    while(1);                     // 复位成功后不会执行到这里
}



/**
  * @brief          初始化"gimbal_control"变量，包括pid初始化， 遥控器指针初始化，云台电机指针初始化，陀螺仪角度指针初始化
  * @param[out]     gimbal_init:"gimbal_control"变量指针.
  * @retval         none
  */
static void gimbal_PID_init(gimbal_PID_t *pid, fp32 maxout, fp32 max_iout, fp32 kp, fp32 ki, fp32 kd)
{
    if (pid == NULL)
    {
        return;
    }
    pid->kp = kp;
    pid->ki = ki;
    pid->kd = kd;

    pid->err = 0.0f;
    pid->get = 0.0f;

    pid->max_iout = max_iout;
    pid->max_out = maxout;
}

static fp32 gimbal_PID_calc(gimbal_PID_t *pid, fp32 get, fp32 set, fp32 error_delta)
{
    fp32 err;
    if (pid == NULL)
    {
        return 0.0f;
    }
    pid->get = get;
    pid->set = set;

    err = set - get;
    pid->err = err;
    pid->Pout = pid->kp * pid->err;
    pid->Iout += pid->ki * pid->err;
    pid->Dout = pid->kd * error_delta;
    abs_limit(&pid->Iout, pid->max_iout);
    pid->out = pid->Pout + pid->Iout + pid->Dout;
    abs_limit(&pid->out, pid->max_out);
    return pid->out;
}
void gimbal_Init(gimbal_control_t *gimbal_control)
{
	//rc控制信号  yaw轴电机信号获取
	gimbal_control->yaw_ctrl_data=get_Chassisdata_point();
	gimbal_control->gimbal_yaw_motor.gimbal_lkmotor_measure=get_yaw_gimbal_lkmotor_measure_point();
	gimbal_control->gimbal_yaw_motor.absolute_angle_set=gimbal_control->gimbal_yaw_motor.absolute_angle;
	// gimbal_control->gimbal_yaw_motor.absolute_angle_set1=rad_format(gimbal_control->gimbal_yaw_motor.absolute_angle_set);

	//初始化模式设置
	gimbal_control->gimbal_yaw_motor.gimbal_motor_mode=GIMBAL_MOTOR_OFF;

	//初始化PID
	gimbal_PID_init(&gimbal_control->gimbal_yaw_motor.gimbal_motor_angle1_pid,YAW_ANGLE_PID_MAX_OUT,YAW_ANGLE_PID_MAX_IOUT,YAW_ANGLE_PID_KP,YAW_ANGLE_PID_KI, YAW_ANGLE_PID_KD);
	gimbal_PID_init(&gimbal_control->gimbal_yaw_motor.gimbal_motor_speed_pid,
						YAW_SPEED_PID_MAX_OUT,
						YAW_SPEED_PID_MAX_IOUT,
						YAW_SPEED_PID_KP,
						YAW_SPEED_PID_KI, 
						YAW_SPEED_PID_KD);

	//初始化滑模控制器
	#ifdef SMC_CONTROL
	SMC_init(&gimbal_smc.yaw, yaw_smc, EXPONENT, SAT_LIMIT_GIMBAL_AIM_YAW, U_MAX_GIMBAL_AIM_YAW, POS_ESP_GIMBAL_AIM_YAW);
	#endif
}
void gimbal_set_mode(gimbal_control_t *gimbal_control)
{
	if(gimbal_control->gimbal_yaw_motor.gimbal_motor_mode==GIMBAL_MOTOR_INIT)
	{
		if(fabs(gimbal_control->gimbal_yaw_motor.relative_angle-gimbal_control->gimbal_yaw_motor.relative_angle_set)>1)
		{
			return;
		}
		else
		{
			gimbal_control->YAW_INIT_FLAG=1;
		}
	}
	if(gimbal_control->yaw_ctrl_data->chassis_mode==CHASSIS_MODE_OFF)
	{
		gimbal_control->gimbal_yaw_motor.gimbal_motor_mode=GIMBAL_MOTOR_OFF;
	}
	else if(gimbal_control->yaw_ctrl_data->chassis_mode==CHASSIS_MODE_NO_FOLLOW)
	{
		gimbal_control->gimbal_yaw_motor.gimbal_motor_mode=GIMBAL_MOTOR_GYRO;
	}
	else if(gimbal_control->yaw_ctrl_data->chassis_mode==CHASSIS_MODE_FOLLOW)
	{
		gimbal_control->gimbal_yaw_motor.gimbal_motor_mode=GIMBAL_MOTOR_ROTATE;
	}
	if(gimbal_control->yaw_ctrl_data->chassis_mode==CHASSIS_MODE_INIT)
	{
		gimbal_control->gimbal_yaw_motor.gimbal_motor_mode=GIMBAL_MOTOR_INIT;
	}
}
void gimbal_feedback_update(gimbal_control_t *gimbal_control)
{

	gimbal_control->gimbal_yaw_motor.absolute_angle=gimbal_control->yaw_ctrl_data->yaw_angle;
	gimbal_control->gimbal_yaw_motor.motor_gyro=gimbal_control->yaw_ctrl_data->yaw_gyro;
	gimbal_control->gimbal_yaw_motor.relative_angle= gimbal_control->gimbal_yaw_motor.gimbal_lkmotor_measure->angle; //(rad_format((gimbal_control->yaw_ctrl_data->yaw_angle)*3.1415926535/180)-rad_format(chassis_move->chassis_yaw))*180/3.1415926535;

	
}
void gimbal_set_control(gimbal_control_t *gimbal_control)
{
	if(gimbal_control->gimbal_yaw_motor.gimbal_motor_mode==GIMBAL_MOTOR_OFF)
	{
		gimbal_control->gimbal_yaw_motor.absolute_angle_set=gimbal_control->gimbal_yaw_motor.absolute_angle;
	}
	else if(gimbal_control->gimbal_yaw_motor.gimbal_motor_mode==GIMBAL_MOTOR_GYRO)
	{
		gimbal_control->gimbal_yaw_motor.absolute_angle_set=gimbal_control->yaw_ctrl_data->yaw_angle_set;
	}
	else if(gimbal_control->gimbal_yaw_motor.gimbal_motor_mode==GIMBAL_MOTOR_ROTATE)
	{
		gimbal_control->gimbal_yaw_motor.absolute_angle_set=gimbal_control->yaw_ctrl_data->yaw_angle_set;
	}
	else  if(gimbal_control->gimbal_yaw_motor.gimbal_motor_mode==GIMBAL_MOTOR_INIT)
	{
		gimbal_control->gimbal_yaw_motor.relative_angle_set=gimbal_control->gimbal_yaw_motor.relative_angle;//0.0f;
	}
}
void gimbal_control_loop(gimbal_control_t *gimbal_control)
{
	if(gimbal_control->gimbal_yaw_motor.gimbal_motor_mode==GIMBAL_MOTOR_OFF)
	{
		if(close==0)
		{
			CAN_LK_CLOSE_control();
			close=1;
		}
		else if(close==1)
		{
			// CAN_read_lkmotor_state();
			start = 0;
		}
	}
	else if(gimbal_control->gimbal_yaw_motor.gimbal_motor_mode==GIMBAL_MOTOR_GYRO)
	{
		//PID控制环
		float target_speed = gimbal_PID_calc(&gimbal_control->gimbal_yaw_motor.gimbal_motor_angle1_pid,
													gimbal_control->gimbal_yaw_motor.absolute_angle,
														gimbal_control->gimbal_yaw_motor.absolute_angle_set,
															gimbal_control->gimbal_yaw_motor.motor_gyro);
//新增 速度前馈: 目标角速度(度/秒)转rad/s, 加到速度环目标
        target_speed += KFF_VEL_YAW * (gimbal_control->yaw_ctrl_data->yaw_vel * 3.1415926535f / 180.0f);
//加速度前馈: 目标角加速度(度/秒?)转rad/s?, 加到电流环目标
		gimbal_control->gimbal_yaw_motor.yaw_given_current = gimbal_PID_calc(&gimbal_control->gimbal_yaw_motor.gimbal_motor_speed_pid,
                    																gimbal_control->gimbal_yaw_motor.motor_gyro,  // 当前速度
                    																		target_speed,                         // 目标速度
                    																			0); 

	}
	else if(gimbal_control->gimbal_yaw_motor.gimbal_motor_mode==GIMBAL_MOTOR_ROTATE)
	{
		//PID控制环
		float target_speed = gimbal_PID_calc(&gimbal_control->gimbal_yaw_motor.gimbal_motor_angle1_pid,
													gimbal_control->gimbal_yaw_motor.absolute_angle,
														gimbal_control->gimbal_yaw_motor.absolute_angle_set,
															gimbal_control->gimbal_yaw_motor.motor_gyro);
//新增 速度前馈: 目标角速度(度/秒)转rad/s, 加到速度环目标
        target_speed += KFF_VEL_YAW * (gimbal_control->yaw_ctrl_data->yaw_vel * 3.1415926535f / 180.0f);
//加速度前馈: 目标角加速度(度/秒?)转rad/s?, 加到电流环目标	
		gimbal_control->gimbal_yaw_motor.yaw_given_current = gimbal_PID_calc(&gimbal_control->gimbal_yaw_motor.gimbal_motor_speed_pid,
                    																gimbal_control->gimbal_yaw_motor.motor_gyro,  // 当前速度
                    																		target_speed,                         // 目标速度
                    																			0); 

	}
	else if(gimbal_control->gimbal_yaw_motor.gimbal_motor_mode==GIMBAL_MOTOR_INIT)
	{
		if(start==0)
		{
			CAN_LK_CLEAR_ERROR_control();
			osDelay(2);
			CAN_LK_START_control();
			close=0;
			start=1;
		}
		if(start==1)
		{
		}
	}
}
void gimbal_task()
{
	gimbal_Init(&gimbal_control);
	shoot_init();
	while(1)
	{
		//加速度前馈: 目标角加速度(度/秒?)转rad/s?, 加到 LK 电机的电流前馈 iq 上
        int16_t yaw_iq_ff = (int16_t)(1200.0f + KFF_ACC_YAW * (gimbal_control.yaw_ctrl_data->yaw_acc * 3.1415926535f / 180.0f));
		gimbal_set_mode(&gimbal_control);
		gimbal_feedback_update(&gimbal_control);
		gimbal_set_control(&gimbal_control);
		gimbal_control_loop(&gimbal_control);
		gimbal_control.shoot=shoot();
		if(gimbal_control.gimbal_yaw_motor.gimbal_motor_mode!=GIMBAL_MOTOR_OFF)
		{
			CAN_cmd_gimbal(0,0,gimbal_control.shoot->given_current,0);	//拨弹盘发送电流
		}
		else 
		{
			CAN_cmd_gimbal(0,0,0,0);
		}
		
		if(gimbal_control.gimbal_yaw_motor.gimbal_motor_mode==GIMBAL_MOTOR_ROTATE)		//FOLLOW MODE
		{
			 CAN_LK_SPEED_Control(yaw_iq_ff,gimbal_control.gimbal_yaw_motor.yaw_given_current*3000.0f);		//33000.0f
			//CAN_LK_Torque_Control(62.060606f*gimbal_control.gimbal_yaw_motor.yaw_given_current);
		}
		else if(gimbal_control.gimbal_yaw_motor.gimbal_motor_mode==GIMBAL_MOTOR_GYRO)		//NO FOLLOW MODE
		{									//0.55
				 CAN_LK_SPEED_Control(yaw_iq_ff,-0.633*chassis_move.wz*57295.0f + gimbal_control.gimbal_yaw_motor.yaw_given_current*1000.0f);
			//CAN_LK_Torque_Control(62.060606f*gimbal_control.gimbal_yaw_motor.yaw_given_current);
		}

		if(reset_flag == 1)
		{
			System_Reset();
		}
		vTaskDelay(GIMBAL_CONTROL_TIME);
	}
}




