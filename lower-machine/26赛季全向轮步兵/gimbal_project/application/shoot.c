#include "main.h"
#include "shoot.h"
#include "bsp_laser.h"
#include <math.h>
#include "Self_aim.h"
#include "gimbal_task.h"

//发弹总结构体
shoot_control_t shoot_control;
//??????
int32_t time_cnt;

//与发弹有关的全局变量
int8_t continue_flag;
uint8_t friction_speed_set_state = 0;
uint8_t fric_flag = 0;

//自瞄数据结构体指针
static InputData *Self_aim_data;
uint8_t shoot_bullet_fre= 0;
float shoot_bullet_speed = 0;


static const fp32 friction_speed_pid[3]={FRICTION_SPEED_PID_KP ,FRICTION_SPEED_PID_KI,FRICTION_SPEED_PID_KD};

void shoot_feedback_update(void);
void shoot_speed_filter(void);
void shoot_set_mode(void);
void friction_control_set(void);



void shoot_Init()
{
	//摩擦轮速度环pid初始化
	PID_init(&shoot_control.friction_left_motor_speed_pid,PID_POSITION,friction_speed_pid,FRICTION_SPEED_PID_MAX_OUT,FRICTION_SPEED_PID_MAX_IOUT);
	PID_init(&shoot_control.friction_right_motor_speed_pid,PID_POSITION,friction_speed_pid,FRICTION_SPEED_PID_MAX_OUT,FRICTION_SPEED_PID_MAX_IOUT);
	
	//射击模式初始化
	shoot_control.shoot_mode = SHOOT_STOP;
	shoot_control.shoot_rc = get_remote_control_point();
	shoot_control.friction_motor_measure[0]=get_chassis_motor_measure_point(0);
	shoot_control.friction_motor_measure[1]=get_chassis_motor_measure_point(1);

	  //自瞄数据获取
  	Self_aim_data = get_selfaim_data();

}

const shoot_control_t *shoot_control_loop()
{
	//更新遥控和按键数据
	shoot_feedback_update(); 

	//速度赋值与滤波
	shoot_speed_filter();

	//射击模式设置，最终发到底盘
	shoot_set_mode();		
	
	//速度值设置
	friction_control_set();

	//速度环PID计算
	PID_calc(&shoot_control.friction_left_motor_speed_pid, shoot_control.friction_left_speed, shoot_control.friction_left_speed_set);
	PID_calc(&shoot_control.friction_right_motor_speed_pid, shoot_control.friction_right_speed, shoot_control.friction_right_speed_set);

	shoot_control.shoot_left_given_current = shoot_control.friction_left_motor_speed_pid.out;
	shoot_control.shoot_right_given_current= shoot_control.friction_right_motor_speed_pid.out;

	return &shoot_control;
} 


void shoot_feedback_update()
{
	shoot_control.last_press_l=shoot_control.press_l;
	shoot_control.last_press_r=shoot_control.press_r;
	shoot_control.last_press_fric=shoot_control.press_fric;
	shoot_control.lastkeyboard = shoot_control.keyboard;
	shoot_control.last_press_back = shoot_control.press_back;

	shoot_control.press_fric =shoot_control.shoot_rc->rc.s[3];
	shoot_control.press_back = shoot_control.shoot_rc->rc.s[1];

	shoot_control.press_l=shoot_control.shoot_rc->mouse.press_l;
	shoot_control.press_r=shoot_control.shoot_rc->mouse.press_r;
	shoot_control.keyboard = shoot_control.shoot_rc->key.v;
}

void shoot_set_mode()
{
	if((shoot_control.press_fric==1&&shoot_control.last_press_fric==0) || ((shoot_control.lastkeyboard & KEY_PRESSED_OFFSET_F) == 0 && shoot_control.keyboard & KEY_PRESSED_OFFSET_F))		//右上角自定义按键
	{
		fric_flag = !fric_flag;		//1 -> 开启摩擦轮  0 -> 关闭摩擦轮
		continue_flag = 0;
		if(fric_flag == 1)		
		{shoot_control.shoot_mode=SHOOT_READY_FRIC;}

		else if(fric_flag == 0)	
		{shoot_control.shoot_mode=SHOOT_STOP;}
	}

	if(fric_flag == 1) 
	{
		if( shoot_control.shoot_rc->rc.s[4]==1 || shoot_control.press_l == 1)	//扳机按下
		{
			shoot_control.shoot_mode=SHOOT_SINGLE;
		}

		if((shoot_control.shoot_rc->rc.s[4]==1 || shoot_control.press_l == 1) && continue_flag == 0)	
		{
			time_cnt++;
			if(time_cnt>23)
			{
				continue_flag=1;
				time_cnt=0;
			}
		}
		else {time_cnt = 0;}

		if(continue_flag==1)
		{
			shoot_control.shoot_mode = SHOOT_CONTINUE;
		}

		if(shoot_control.shoot_rc->rc.s[4]==0 && shoot_control.press_l == 0)
		{
			shoot_control.shoot_mode=SHOOT_READY_FRIC;
			continue_flag = 0;
		}


		//关于自瞄的开播弹盘
		if(aimflag == 1)
		        //视觉自动发弹(人工优先): 未扣扳机时, fire_advicec==1 就进入连发
        
        {
            if(shoot_control.press_l == 0 && shoot_control.shoot_rc->rc.s[4] == 0)   // 人没接管
            {
                if(Self_aim_data->fire_advicec == 1)
                {
                    shoot_control.shoot_mode = SHOOT_CONTINUE;   // 视觉连发
                }
            }
        }
			//if((Self_aim_data->fire_advicec == 1) && (shoot_control.press_l == 1 || shoot_control.shoot_rc->rc.s[4] == 1))
			//{
				//shoot_control.shoot_mode=SHOOT_SINGLE;
				//continue_flag = 1;
			//}
			// else{shoot_control.shoot_mode=SHOOT_READY_FRIC;}
		
	}
	else if(fric_flag == 0)
	{
		// if(shoot_control.press_back == 0 && shoot_control.last_press_back == 0 && (shoot_control.keyboard & KEY_PRESSED_OFFSET_B) == 0)		
		// {
			shoot_control.shoot_mode=SHOOT_STOP;
		// }
	}

	if((shoot_control.press_back == 1 && shoot_control.last_press_back == 1) || ((shoot_control.keyboard & KEY_PRESSED_OFFSET_B) && (shoot_control.lastkeyboard & KEY_PRESSED_OFFSET_B)))		//长按退弹
	{
		shoot_control.shoot_mode=SHOOT_BACK;
	}


}

void friction_control_set()
{
	if(fric_flag == 1)
	{
		if(friction_speed_set_state == 0)
		{
			// laser_on();		//意义不明的函数
			shoot_control.friction_left_speed_set+=50;  
			shoot_control.friction_right_speed_set-=50;   
			if(shoot_control.friction_left_speed_set >= 6100 && shoot_control.friction_right_speed_set <= -6100)                           
			{friction_speed_set_state = 1;}
		}
		else if(friction_speed_set_state == 1)
		{
			shoot_control.friction_left_speed_set=6100;  
			shoot_control.friction_right_speed_set=-6100;                              
			// laser_on();
		}
	}
	else if (fric_flag == 0)
	{
		shoot_control.friction_left_speed_set=0;  
		shoot_control.friction_right_speed_set=0;  
		friction_speed_set_state = 0;                            
		// laser_off();
	}
}

void shoot_speed_filter()
{
	
	shoot_control.friction_left_last_speed=shoot_control.friction_left_speed;
	shoot_control.friction_right_last_speed=shoot_control.friction_right_speed;

	//所谓一阶低通滤波
	shoot_control.friction_left_speed=0.5f*shoot_control.friction_motor_measure[0]->speed_rpm+0.5f*shoot_control.friction_left_last_speed;
	shoot_control.friction_right_speed=0.5f*shoot_control.friction_motor_measure[1]->speed_rpm+0.5f*shoot_control.friction_right_last_speed;

	//速度限幅
	shoot_control.friction_left_speed = fp32_constrain(shoot_control.friction_left_speed,-10000,10000);
	shoot_control.friction_right_speed = fp32_constrain(shoot_control.friction_right_speed,-10000,10000);
}

