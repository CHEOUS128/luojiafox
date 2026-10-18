/**
  ****************************(C) COPYRIGHT 2019 DJI****************************
  * @file       can_receive.c/h
  * @brief      there is CAN interrupt function  to receive motor data,
  *             and CAN send function to send motor current to control motor.
  *             这里是CAN中断接收函数，接收电机数据,CAN发送函数发送电机电流控制电机.
  * @note       
  * @history
  *  Version    Date            Author          Modification
  *  V1.0.0     Dec-26-2018     RM              1. done
  *  V1.1.0     Nov-11-2019     RM              1. support hal lib
  *
  @verbatim
  ==============================================================================

  ==============================================================================
  @endverbatim
  ****************************(C) COPYRIGHT 2019 DJI****************************
  */

#ifndef CAN_RECEIVE_H
#define CAN_RECEIVE_H

#include "struct_typedef.h"
#include "user_lib.h"
#define CHASSIS_CAN hcan1
#define GIMBAL_CAN hcan2

/* CAN send and receive ID */
typedef enum
{
  CAN_CHASSIS_ALL_ID = 0x200,
  CAN_3508_M1_ID = 0x201,
  CAN_3508_M2_ID = 0x202,
  CAN_3508_M3_ID = 0x203,
  CAN_3508_M4_ID = 0x204,
  CAN_FRICTION_LEFT_ID = 0x207,
  CAN_FRICTION_RIGHT_ID = 0x208,
  CAN_YAW_MOTOR_ID = 0x205,
  CAN_PIT_MOTOR_ID = 0x206,
  // 瓴控电机控ID
  CAN_LK_MOTOR_M1_ID = 0x141,
  CAN_GIMBAL_ALL_ID = 0x1FF,
  CAN_gmbial_chassis_data1 = 0x01,
  CAN_gmbial_chassis_data2 = 0x02,
  CAN_gmbial_chassis_data3 = 0x03,
  CAN_gimbal_chassis_data4 = 0x04,
  CAN_gimbal_chassis_data5 = 0x06,      //  新增
  CAN_chassis_gimbal_ID=0x123,
  CAN_referee_data = 0x05,

} can_msg_id_e;
typedef union
{
	float float_t;
	uint8_t uint8_t[4];
} send_float_typedef;
typedef enum
{
  CHASSIS_MODE_OFF=0,
  CHASSIS_MODE_NO_FOLLOW,
  CHASSIS_MODE_FOLLOW,
  CHASSIS_MODE_INIT,
	CHASSIS_MODE_L_TOP,
	CHASSIS_MODE_R_TOP
}chassis_mode_e;

typedef struct
{
	float vx_set;//底盘x轴方向设定的速度控制量；
	float vy_set;//底盘y轴方向设定的速度控制量
	float wz_set;//底盘自旋时 设定的速度控制量；
	float yaw_angle;//yaw轴角度实时值
	float yaw_angle_set;//yaw轴角度设定值
	float yaw_gyro;//yaw轴角速度实时值
  float yaw_vel;   // 新增
  float yaw_acc;   // 新增
	first_order_filter_type_t chassis_cmd_slow_set_vx;  //use first order filter to slow set-point.使用一阶低通滤波减缓设定值
	first_order_filter_type_t chassis_cmd_slow_set_vy;  //use first order filter to slow set-point.使用一阶低通滤波减缓设定值
  chassis_mode_e last_chassis_mode;
	chassis_mode_e chassis_mode;//底盘模式
	uint16_t shoot_mode;//射击模式
}chassis_data_t;
//rm motor data
typedef struct
{
    uint16_t ecd;
    int16_t speed_rpm;
    int16_t given_current;
    uint8_t temperate;
    int16_t last_ecd;
} motor_measure_t;
// extern motor_measure_t;

//瓴控电机数据
typedef struct
{
	int8_t temp;
	int16_t iq;
	int16_t speed;
	uint16_t encoder;
	int16_t last_encoder;
	float angle;
}lkmotor_measure_t;
// extern lkmotor_measure_t;




/**
  * @brief          send control current of motor (0x205, 0x206, 0x207, 0x208)
  * @param[in]      yaw: (0x205) 6020 motor control current, range [-30000,30000] 
  * @param[in]      pitch: (0x206) 6020 motor control current, range [-30000,30000]
  * @param[in]      shoot: (0x207) 2006 motor control current, range [-10000,10000]
  * @param[in]      rev: (0x208) reserve motor control current
  * @retval         none
  */
/**
  * @brief          发送电机控制电流(0x205,0x206,0x207,0x208)
  * @param[in]      yaw: (0x205) 6020电机控制电流, 范围 [-30000,30000]
  * @param[in]      pitch: (0x206) 6020电机控制电流, 范围 [-30000,30000]
  * @param[in]      shoot: (0x207) 2006电机控制电流, 范围 [-10000,10000]
  * @param[in]      rev: (0x208) 保留，电机控制电流
  * @retval         none
  */
extern void CAN_cmd_gimbal(int16_t yaw, int16_t pitch, int16_t shoot, int16_t rev);
/**
  * @brief          发送角速度控制瓴控电机
  * @param[in]      iqControl 控制电流，数值范围围[-2048, 2048]
  * @param[in]      speedControl控制角速度，对应实际转速为 0.01dps/LSB
  * @retval         none
  */


/**
  * @brief          send CAN packet of ID 0x700, it will set chassis motor 3508 to quick ID setting
  * @param[in]      none
  * @retval         none
  */
/**
  * @brief          发送ID为0x700的CAN包,它会设置3508电机进入快速设置ID
  * @param[in]      none
  * @retval         none
  */
extern void CAN_cmd_chassis_reset_ID(void);

/**
  * @brief          send control current of motor (0x201, 0x202, 0x203, 0x204)
  * @param[in]      motor1: (0x201) 3508 motor control current, range [-16384,16384] 
  * @param[in]      motor2: (0x202) 3508 motor control current, range [-16384,16384] 
  * @param[in]      motor3: (0x203) 3508 motor control current, range [-16384,16384] 
  * @param[in]      motor4: (0x204) 3508 motor control current, range [-16384,16384] 
  * @retval         none
  */
/**
  * @brief          发送电机控制电流(0x201,0x202,0x203,0x204)
  * @param[in]      motor1: (0x201) 3508电机控制电流, 范围 [-16384,16384]
  * @param[in]      motor2: (0x202) 3508电机控制电流, 范围 [-16384,16384]
  * @param[in]      motor3: (0x203) 3508电机控制电流, 范围 [-16384,16384]
  * @param[in]      motor4: (0x204) 3508电机控制电流, 范围 [-16384,16384]
  * @retval         none
  */
extern void CAN_cmd_chassis(int16_t motor1, int16_t motor2, int16_t motor3, int16_t motor4);
/**
  * @brief          发送底盘速度设定值vxvy
  * @param[in]      none
  * @retval         none
  */
extern void can_gimbal_chassis_data1(chassis_data_t *chassis_data);
/**
  * @brief          发送底盘旋转速度和跟随云台角度
  * @param[in]      none
  * @retval         none
  */
extern void can_gimbal_chassis_data2(chassis_data_t *chassis_data);
/**
  * @brief          发送底盘模式和云台yaw轴角度
  * @param[in]      none
  * @retval         none
  */
extern void can_gimbal_chassis_data3(chassis_data_t *chassis_data);
/**
  * @brief          发送云台yaw轴角速度
  * @param[in]      none
  * @retval         none
  */
extern void can_gimbal_chassis_data4(chassis_data_t *chassis_data);
/**
  * @brief          发送读取pitch轴瓴控电机数据指令
  * @param[in]      none
  * @retval         none
  */
extern void can_gimbal_chassis_data5(chassis_data_t *chassis_data);//新增

extern void CAN_LK_START_control(void);
extern void CAN_LK_CLOSE_control(void);
extern void CAN_read_lkmotor_state(void);
extern void CAN_LK_SPEED_Control(int16_t iqControl,int32_t speedControl);
extern void CAN_LK_Torque_Control(int16_t iqControl);
extern void CAN_LK_CLEAR_ERROR_control(void);


/**
  * @brief          返回yaw 6020电机数据指针
  * @param[in]      none
  * @retval         电机数据指针
  */
extern const motor_measure_t *get_yaw_gimbal_motor_measure_point(void);

/**
  * @brief          return the pitch 6020 motor data point
  * @param[in]      none
  * @retval         motor data point
  */
/**
  * @brief          返回pitch 6020电机数据指针
  * @param[in]      none
  * @retval         电机数据指针
  */
extern const motor_measure_t *get_pitch_gimbal_motor_measure_point(void);
/**
  * @brief          return the pitch  lkmotor data point
  * @param[in]      none
  * @retval         motor data point
  */
/**
  * @brief          返回pitch 瓴控电机数据指针
  * @param[in]      none
  * @retval         电机数据指针
  */
extern const lkmotor_measure_t *get_pitch_gimbal_lkmotor_measure_point(void);


/**
  * @brief          返回左摩擦轮电机数据指针
  * @param[in]      none
  * @retval         电机数据指针
  */
extern const motor_measure_t *get_left_friction_motor_measure_point(void);

/**
  * @brief          返回右摩擦轮电机数据指针
  * @param[in]      none
  * @retval         电机数据指针
  */
extern const motor_measure_t *get_right_friction_motor_measure_point(void);

/**
  * @brief          return the chassis 3508 motor data point
  * @param[in]      i: motor number,range [0,3]
  * @retval         motor data point
  */
/**
  * @brief          返回底盘电机 3508电机数据指针
  * @param[in]      i: 电机编号,范围[0,3]
  * @retval         电机数据指针
  */
extern const motor_measure_t *get_chassis_motor_measure_point(uint8_t i);

extern void get_enemy_color(uint8_t *enemy_color);
#endif
