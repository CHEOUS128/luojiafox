#ifndef SHOOT_H
#define SHOOT_H
#include "CAN_receive.h"
#include "pid.h"
#define MOTOR_ECD_TO_ANGLE          0.00007666
#define FULL_COUNT                  9.5
#define HALF_ECD_RANGE  4096
#define ECD_RANGE       8191
#define TRIGGER_SPEED_PID_KP        15.0f//25
#define TRIGGER_SPEED_PID_KI        0.15f//0.15
#define TRIGGER_SPEED_PID_KD        3.0f
#define TRIGGER_BULLET_PID_MAX_OUT  16000.0f
#define TRIGGER_BULLET_PID_MAX_IOUT 9000.0f

#define TRIGGER_ANGLE_PID_KP        4.0f
#define TRIGGER_ANGLE_PID_KI        0.0f
#define TRIGGER_ANGLE_PID_KD        0.50f
#define TRIGGER_ANGLE_PID_MAX_OUT  12000.0f
#define TRIGGER_ANGLE_PID_MAX_IOUT 9000.0f

#define PI_TEN    3800				//单发模式发一弹编码器增值	原3600


//电机rmp 变化成 旋转速度的比例
#define MOTOR_RPM_TO_SPEED          0.00290888208665721596153948461415f


//#define BLOCK_TIME                  700
//#define REVERSE_TIME                1000
//卡单时间 以及反转时间
//#define BLOCK_TRIGGER_SPEED         2500.0f
//拨弹速度
//#define TRIGGER_SPEED               10.0f
//#define CONTINUE_TRIGGER_SPEED      10.0f

/* -----------拨盘电机发射状态------------ */

typedef enum {
    SHOOT_START_SINGLE   = 1,
    SHOOT_START_CONTINUE = 2,
    SHOOT_FINISH         = 3,
    SHOOT_BACKING        = 4,
} shoot_state_e;






	 typedef struct
		{
			shoot_mode_e shoot_mode,shoot_last_mode	;
			const motor_measure_t *shoot_motor_measure;
			chassis_data_t *shoot_control_data;
			/* 分为速度环和位置环两种模式 */
			pid_type_def trigger_position_mode_speed_pid;
			pid_type_def trigger_speed_mode_speed_pid;
			pid_type_def trigger_motor_angle_pid;
			fp32 trigger_speed_set;
			fp32 speed , last_speed;
			fp32 speed_set;
			fp32 angle;
			fp32 set_angle;
			int16_t given_current;
			int32_t ecd_count;
			bool_t last_press_l;
			bool_t press_l;
			uint8_t shoot_friction_mode;
			uint8_t shooter_output;
			//uint16_t block_time;
			//uint16_t reverse_time;
			uint16_t shoot_heat_limit;
			uint16_t shoot_heat_value;
			uint16_t shoot_cooling_rate;
			uint16_t last_shoot_heat_value;
			float  bullet_frequent;
			float real_time_heat;
			
			shoot_state_e shoot_state;
			uint8_t single_shoot_state ;			//单发模式状态位，用于显示单发动作的执行情况
			uint8_t single_shoot_cnt;			//�?1表示本次单发动作完成，防止连续单�?
			uint32_t auto_back_cnt;
			uint32_t seem_back_cnt;
			uint8_t process_state;


		}shoot_control_t;


// typedef enum
// {
// 	SHOOT_FINISH = 0,
// 	SHOOT_START_SINGLE,
// 	SHOOT_START_CONTINUE,
// 	SHOOT_BACKING,
// } shoot_state_e;





extern shoot_control_t *shoot(void);
extern void shoot_init(void);
//extern float data[2];
#endif
