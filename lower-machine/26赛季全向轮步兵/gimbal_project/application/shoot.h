#ifndef SHOOT_H
#define SHOOT_H
#include "CAN_receive.h"
#include "pid.h"
#include "remote_control.h"

#define FRICTION_SPEED_PID_KP        3.8f       //7
#define FRICTION_SPEED_PID_KI        0.04f       //0.0001
#define FRICTION_SPEED_PID_KD        30.0f      //2
#define FRICTION_SPEED_PID_MAX_OUT   30000.0f
#define FRICTION_SPEED_PID_MAX_IOUT  5000.0f


//灏勫嚮鍙戝皠寮€鍏抽€氶亾鏁版嵁
#define SHOOT_RC_MODE_CHANNEL       0

#define SHOOT_CONTROL_TIME          GIMBAL_CONTROL_TIME


#define FRIC_SPEED_MAX 4000
#define FRIC_SPEED_MIN 0

//榧犳爣闀挎寜鍒ゆ柇
#define PRESS_LONG_TIME             400
//閬ユ帶鍣ㄥ皠鍑诲紑鍏虫墦涓嬫。涓€娈垫椂闂村悗 杩炵画鍙戝皠瀛愬脊 鐢ㄤ簬娓呭崟
#define RC_S_LONG_TIME              800

typedef enum
{
    SHOOT_STOP = 0,   //鍋滄灏勫嚮
    SHOOT_READY_FRIC,    //鎽╂摝鍑嗗灏辩华
    SHOOT_READY_BULLET,  //寮硅嵂鍑嗗灏辩华


    SHOOT_SINGLE,
    SHOOT_CONTINUE,
    SHOOT_READY,         //灏勫嚮鍑嗗灏辩华
    SHOOT_BULLET,        //姝ｅ湪灏勫嚮
    SHOOT_CONTINUE_BULLET,   //缁х画灏勫嚮
    SHOOT_DONE,            //灏勫嚮瀹屾垚
    SHOOT_BACK,
}shoot_mode_e;

/* 浣庨€氭护娉㈠櫒缁撴瀯浣? */
typedef struct
{
	fp32 output;
	fp32 alpha;
}low_pass_filter_t;

typedef struct
{
	shoot_mode_e shoot_mode;
	const motor_measure_t *friction_motor_measure[2];
	const RC_ctrl_t *shoot_rc;
    ramp_function_source_t fric_ramp;      //鏂滄尝鍑芥暟缁撴瀯浣?
    /* 宸﹁疆 */
	pid_type_def friction_left_motor_speed_pid; 
    /* 鍙宠疆 */
	pid_type_def friction_right_motor_speed_pid; 
    /* 宸﹁疆 */
	pid_type_def friction_left_motor_current_pid; 
    /* 鍙宠疆 */
	pid_type_def friction_right_motor_current_pid; 
    /* 浣庨€氭护娉㈠櫒*/ 
    low_pass_filter_t left_speed_filter;
    low_pass_filter_t right_speed_filter;
    fp32 friction_left_last_speed;
	fp32 friction_right_last_speed;    
    fp32 friction_left_speed;
	fp32 friction_right_speed;
    fp32 friction_left_speed_set;
    fp32 friction_right_speed_set;
	int16_t shoot_left_given_current;
	int16_t shoot_right_given_current;
	bool_t press_l,last_press_l;
    bool_t press_r,last_press_r;
    bool_t press_fric,last_press_fric;
    bool_t press_back,last_press_back;  //退弹按键
    uint16_t press_l_time;
    uint16_t press_r_time;
	uint16_t rc_s_time;
    uint16_t keyboard;
    uint16_t lastkeyboard;
}shoot_control_t;



const shoot_control_t *shoot_control_loop(void);
extern shoot_control_t shoot_control;
extern void shoot_Init(void);


extern uint8_t shoot_bullet_fre;
extern float shoot_bullet_speed;



#endif
