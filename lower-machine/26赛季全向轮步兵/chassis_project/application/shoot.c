#include "main.h"
#include "cmsis_os.h"
#include "CAN_receive.h"
#include "shoot.h"
#include "pid.h"
#include "referee.h"
#include "detect_task.h"
#include "math.h"
#include "user_lib.h"

extern ext_bullet_remaining_t bullet_remaining_t;

shoot_control_t shoot_control;
const chassis_data_t *shoot_enable;





void shoot_heat_limit(void);  
uint16_t shoot_single_control(void)
{
    if (shoot_control.single_shoot_state == 0)
    {
        shoot_control.set_angle = shoot_control.angle + PI_TEN;
        shoot_control.single_shoot_state = 1;
        return 0;  // 动作开�?
    }
    else if(shoot_control.single_shoot_state == 1)
    {
        if (fabs(shoot_control.set_angle - shoot_control.angle) > 1.0f)
        {
			/* ------位置环PID-------- */
            PID_calc(&shoot_control.trigger_motor_angle_pid, shoot_control.angle, shoot_control.set_angle);
            PID_calc(&shoot_control.trigger_position_mode_speed_pid, shoot_control.shoot_motor_measure->speed_rpm, shoot_control.trigger_motor_angle_pid.out);
            shoot_control.given_current=(int16_t)(shoot_control.trigger_position_mode_speed_pid.out);
            return 0;  // 动作进行�?
        }
        else 
        {
            shoot_control.single_shoot_state = 0;
            shoot_control.single_shoot_cnt = 1;
            return 1;  // 动作完成
        }
    }
    return 0;  // 默认返回�?
}

static void shoot_feedback_update(void)
{
	shoot_control.last_shoot_heat_value = shoot_control.shoot_heat_value;
	shoot_control.last_speed = shoot_control.speed;
	shoot_control.speed = shoot_control.shoot_motor_measure->speed_rpm ;
	shoot_control.angle=shoot_control.shoot_motor_measure->angle;
	shoot_control.last_press_l=shoot_control.press_l;
	shoot_control.press_l=(shoot_control.shoot_control_data->shoot_mode_receive >>4) & 0x01;
	shoot_control.shoot_control_data->shoot_mode_rc = shoot_control.shoot_control_data->shoot_mode_receive&0x0F;
	get_power_shooter_output(&shoot_control.shooter_output);
	get_shoot_heat0_limit_and_heat0(&shoot_control.shoot_heat_limit,&shoot_control.shoot_heat_value , &shoot_control.shoot_cooling_rate);
}

void shoot_init()
{
	static const fp32 Trigger_speed_pid[3] = {TRIGGER_SPEED_PID_KP, TRIGGER_SPEED_PID_KI, TRIGGER_SPEED_PID_KD};
	static const fp32 Trigger_angle_pid[3]={TRIGGER_ANGLE_PID_KP ,TRIGGER_ANGLE_PID_KI ,TRIGGER_ANGLE_PID_KD };

	shoot_control.shoot_motor_measure=get_trigger_motor_measure_point();
	shoot_control.shoot_control_data=get_Chassisdata_point();

	PID_init(&shoot_control.trigger_position_mode_speed_pid, PID_POSITION, Trigger_speed_pid, TRIGGER_BULLET_PID_MAX_OUT, TRIGGER_BULLET_PID_MAX_IOUT);
	PID_init(&shoot_control.trigger_speed_mode_speed_pid, PID_POSITION, Trigger_speed_pid, TRIGGER_BULLET_PID_MAX_OUT, TRIGGER_BULLET_PID_MAX_IOUT);
	PID_init(&shoot_control.trigger_motor_angle_pid,PID_POSITION,Trigger_angle_pid,TRIGGER_ANGLE_PID_MAX_OUT ,TRIGGER_ANGLE_PID_MAX_IOUT);
	
	shoot_control.angle =shoot_control.shoot_motor_measure->angle;
	shoot_control.set_angle = shoot_control.angle;
    shoot_control.speed_set=0.0f;
	shoot_control.speed = 0.0f;
	shoot_control.shoot_state = SHOOT_FINISH;

	shoot_control.single_shoot_state = 0;
    shoot_control.single_shoot_cnt = 0;
    shoot_control.auto_back_cnt = 0;
    shoot_control.seem_back_cnt = 0;
    shoot_control.bullet_frequent = 0.0f;
    shoot_control.real_time_heat = 0.0f;
	
	
}

static void shoot_set_mode(void)
{
	if(shoot_control.shoot_control_data->shoot_mode_rc==SHOOT_SINGLE)
	{
		shoot_control.shoot_mode=SHOOT_SINGLE;
	}
	else if(shoot_control.shoot_control_data->shoot_mode_rc==SHOOT_CONTINUE)
	{
		shoot_control.shoot_mode=SHOOT_CONTINUE;
	}
	else if(shoot_control.shoot_control_data->shoot_mode_rc==SHOOT_BACK)	//退弹
	{
		shoot_control.shoot_mode=SHOOT_BACK;
	}
	else if(shoot_control.shoot_control_data->shoot_mode_rc==SHOOT_STOP||shoot_control.shoot_control_data->shoot_mode_rc==SHOOT_READY_FRIC)
	{
		shoot_control.shoot_mode=SHOOT_STOP;
	}

}

/*自动退弹版*/
void shoot_control_set()
{
	if(shoot_control.shoot_mode==SHOOT_SINGLE  && shoot_control.shoot_state == SHOOT_FINISH)
	{
		if((float)shoot_control.shoot_heat_limit - shoot_control.shoot_heat_value >= 18.0f )
		{
			shoot_control.shoot_state = SHOOT_START_SINGLE;
		}
		else 
		{
			shoot_control.shoot_state = SHOOT_FINISH;
		}
	}

	if(shoot_control.shoot_state == SHOOT_START_SINGLE)
	{
		shoot_control.process_state = shoot_single_control();
		if (shoot_control.process_state==1)
		{
			shoot_control.process_state=0;
			shoot_control.given_current=0;
			shoot_control.shoot_state = SHOOT_FINISH;
		}
	}

	if(shoot_control.shoot_mode==SHOOT_CONTINUE && shoot_control.shoot_state == SHOOT_FINISH)
	{
		shoot_control.last_shoot_heat_value = shoot_control.shoot_heat_value;	//刚开连发时赋值一下，应该准确
		shoot_control.shoot_state = SHOOT_START_CONTINUE;
	}
	if(shoot_control.shoot_state == SHOOT_START_CONTINUE)	
	{

		shoot_control.bullet_frequent = 27 ;	//弹频

		shoot_heat_limit();
		
							//	弹频/拨弹盘一圈弹数*60s*大齿轮齿数/小齿轮齿数*2006减速比
		shoot_control.speed_set= shoot_control.bullet_frequent/9.0f*60.0f*5.0f/2.0f*36.0f;
		
		shoot_control.single_shoot_cnt = 0;			//单发的这个标志位置0

		/* -------计算PID速度�?-------- */
		PID_calc(&shoot_control.trigger_speed_mode_speed_pid, shoot_control.shoot_motor_measure->speed_rpm, shoot_control.speed_set);
		shoot_control.given_current=(int16_t)(shoot_control.trigger_speed_mode_speed_pid.out);

		if(shoot_control.shoot_mode!=SHOOT_CONTINUE)
		{
			shoot_control.shoot_state = SHOOT_FINISH;
		}

		shoot_control.auto_back_cnt ++ ;
		if(shoot_control.auto_back_cnt > 80)		//刚开连发不去判断，开一段时间再判断
		{
			//退弹条件1
			if( shoot_control.given_current >= 15000 && shoot_control.speed < 0.04f * shoot_control.speed_set && shoot_control.last_speed < 0.04f * shoot_control.speed_set)
			{
				shoot_control.seem_back_cnt++;
				//卡住时间要长达450个任务间隔才判断为卡弹
				if(shoot_control.seem_back_cnt > 300)
				{
					shoot_control.shoot_state = SHOOT_BACKING;
					shoot_control.seem_back_cnt = 0;
				}
			}
			else 
			{
				shoot_control.seem_back_cnt = 0;
			}
		}

	}

	if(shoot_control.shoot_state == SHOOT_BACKING)
	{
		shoot_control.speed_set = -800;		//退弹速度慢一点
		PID_calc(&shoot_control.trigger_speed_mode_speed_pid, shoot_control.shoot_motor_measure->speed_rpm, shoot_control.speed_set);
		shoot_control.given_current=(int16_t)(shoot_control.trigger_speed_mode_speed_pid.out);
		shoot_control.auto_back_cnt ++ ;

		if(shoot_control.auto_back_cnt > 600)
		{
			shoot_control.shoot_state = SHOOT_FINISH;
			//计数标志记得清零
			shoot_control.auto_back_cnt = 0;
			shoot_control.seem_back_cnt = 0;
		}

	}

	if(shoot_control.shoot_mode==SHOOT_BACK && shoot_control.shoot_state == SHOOT_FINISH)		//手动退弹
	{
		shoot_control.speed_set = -800;		//退弹速度慢一点
		
		PID_calc(&shoot_control.trigger_speed_mode_speed_pid, shoot_control.shoot_motor_measure->speed_rpm, shoot_control.speed_set);
		shoot_control.given_current=(int16_t)(shoot_control.trigger_speed_mode_speed_pid.out);

	}


	if((shoot_control.shoot_mode!=SHOOT_CONTINUE)&&(shoot_control.shoot_mode!=SHOOT_SINGLE)&& (shoot_control.shoot_mode!=SHOOT_BACK) && shoot_control.shoot_state == SHOOT_FINISH)
	{	//停拨弹盘
		shoot_control.speed_set=0;
		PID_calc(&shoot_control.trigger_speed_mode_speed_pid, shoot_control.shoot_motor_measure->speed_rpm, shoot_control.speed_set);
		shoot_control.given_current=(int16_t)(shoot_control.trigger_speed_mode_speed_pid.out);
		shoot_control.single_shoot_cnt = 0;	
		shoot_control.auto_back_cnt = 0;
		shoot_control.real_time_heat = shoot_control.shoot_heat_value;
	}
	/*此判断是为了防止按一次单发却多次触发*/
	if(shoot_control.shoot_mode==SHOOT_SINGLE && shoot_control.shoot_state == SHOOT_FINISH && shoot_control.single_shoot_cnt == 1)
	{
		shoot_control.speed_set=0;    
		PID_calc(&shoot_control.trigger_speed_mode_speed_pid, shoot_control.shoot_motor_measure->speed_rpm, shoot_control.speed_set);
		shoot_control.given_current=(int16_t)(shoot_control.trigger_speed_mode_speed_pid.out);
	}

}



/*我自己写的热量控制*/
void shoot_heat_limit(void)
{
	if(shoot_control.shoot_heat_value != shoot_control.last_shoot_heat_value)	//认为更新了
	{
		shoot_control.real_time_heat = shoot_control.shoot_heat_value;
	}
	else 
	{
		shoot_control.real_time_heat =shoot_control.real_time_heat + shoot_control.bullet_frequent*0.002f*10 - shoot_control.shoot_cooling_rate*0.002f;		//我的任务间隔是0.002s
		if(shoot_control.real_time_heat <= 0)		{shoot_control.real_time_heat = 0;}
	}
	//以上保证更新没问题

	if(shoot_control.real_time_heat >= (float)shoot_control.shoot_heat_limit / 2.0f)		//热量值达到最大的1/3开始减速
	{
		shoot_control.bullet_frequent = (float)(shoot_control.shoot_heat_limit - shoot_control.real_time_heat)/(float)shoot_control.shoot_heat_limit*20.0f;
	}
	else if((float)shoot_control.shoot_heat_limit - shoot_control.real_time_heat <= 18.0f)	//直接停
	{
		shoot_control.bullet_frequent = 0;
	}
	else 
	{
		shoot_control.bullet_frequent = 27.0f;		//维持理想弹频
	}


}
shoot_control_t *shoot()
{
		shoot_feedback_update();
		shoot_set_mode();
		shoot_control_set();
		return &shoot_control;
	
}

