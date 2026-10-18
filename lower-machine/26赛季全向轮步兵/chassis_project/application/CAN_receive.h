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

#define CHASSIS_CAN hcan1
#define GIMBAL_CAN hcan1
#define REFEREE_CAN hcan2
/* CAN send and receive ID */
typedef enum
{
  CAN_CHASSIS_ALL_ID = 0x200,
  CAN_3508_M1_ID = 0x201,
  CAN_3508_M2_ID = 0x202,
  CAN_3508_M3_ID = 0x203,
  CAN_3508_M4_ID = 0x204,

  CAN_YAW_MOTOR_ID = 0x205,
  CAN_PIT_MOTOR_ID = 0x206,
  CAN_TRIGGER_MOTOR_ID = 0x207,
  CAN_GIMBAL_ALL_ID = 0x1FF,

  CAN_gmbial_chassis_data1 = 0x01,
  CAN_gmbial_chassis_data2 = 0x02,
  CAN_gmbial_chassis_data3 = 0x03,
  CAN_gmbial_chassis_data4 = 0x04,
  CAN_gimbal_chassis_data5 = 0x06,      //  新增
  CAN_chassis_gimbal_ID = 0x123,
  CAN_referee_data = 0x05,

  CAN_LK_MOTOR_M1_ID = 0x141,
  /* 超级电容ID */
  CAN_SUPER_CAP_ID = 0x211,
  /* 超级电容设置ID */
  CAN_SUPER_CAP_SET_ID = 0x210,
  CAN_SuperPower_ID = 0x52,

} can_msg_id_e;

//rm motor data
typedef struct
{
    uint16_t ecd;
    int16_t speed_rpm;
    int16_t given_current;
    uint8_t temperate;
    int16_t last_ecd;
	fp32 angle;
    int32_t ecd_count;
} motor_measure_t;
typedef struct
{
	int8_t temp;
	int16_t iq;
	int16_t speed;
	uint16_t encoder;
	int16_t last_encoder;
	float angle;
}lkmotor_measure_t;
//extern lkmotor_measure_t;
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
}RC_chassis_mode_e;

typedef enum
{
    SHOOT_STOP = 0,   
    SHOOT_READY_FRIC,  
    SHOOT_READY_BULLET,


    SHOOT_SINGLE,
    SHOOT_CONTINUE,
    SHOOT_READY,       
    SHOOT_BULLET,      
    SHOOT_CONTINUE_BULLET,
    SHOOT_DONE,     
    SHOOT_BACK,       
} shoot_mode_e;

typedef struct
{
	float vx_set;//底盘x轴方向设定的速度控制量；
	float vy_set;//底盘y轴方向设定的速度控制量
	float wz_set;//底盘自旋时 设定的速度控制量；
	float yaw_angle;//yaw轴实时角度
	float yaw_angle_set;//设置yaw轴角度
	float yaw_gyro;//yaw轴角速度
  float yaw_vel;   // 新增
  float yaw_acc;   // 新增
	RC_chassis_mode_e chassis_mode;//底盘模式
	shoot_mode_e shoot_mode_rc;//接收到的射击模式
  uint16_t shoot_mode_receive;
}chassis_data_t;	


typedef struct
{
    uint8_t statusCode;
    float chassisPower;
    float refereePower;
    uint16_t chassisPowerLimit;
    uint8_t capEnergy;
    uint32_t updateTick;
} __attribute__((packed))super_power_receive_t;
typedef struct{
    uint8_t enableDCDC: 1;
    uint8_t systemRestart: 1;
    uint8_t resv0: 3;
    uint8_t clearError: 1;
    uint8_t enableActiveChargingLimit: 1;
    uint8_t useNewFeedbackMessage: 1;

    uint16_t refereePowerLimit;
    uint16_t refereeEnergyBuffer;
    uint8_t activeChargingLimitRatio; 
    int16_t resv2;
} __attribute__((packed)) super_power_t ; //
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
extern void CAN_cmd_referee_data(uint8_t bullet_freq, float bullet_speed, uint16_t chassis_buffer_send);
extern void CAN_INIT_STATUS(uint8_t status);
extern void CAN_LK_START_control(void);
extern void CAN_LK_CLOSE_control(void);
extern void CAN_read_lkmotor_state(void);
extern void CAN_LK_POSITION_Control(int32_t angleControl);
extern void CAN_LK_SPEED_Control(int16_t iqControl,int32_t speedControl);

extern void CAN_LK_Torque_Control(int16_t iqControl);
extern void CAN_LK_CLEAR_ERROR_control(void);


extern void CAN_Send_Setpower(uint16_t setPower);
extern void CAN_SuperPower_Control(super_power_t super_power_data);
extern void CAN_tx_queue_init(void);
extern void CAN_tx_task(void const *pvParameters);
extern uint32_t CAN_get_tx_drop_count(void);
extern uint8_t CAN_super_power_is_online(void);
extern void CAN_get_super_power_data(super_power_receive_t *data);

extern uint8_t reset_flag;





/**
  * @brief          return the yaw 6020 motor data point
  * @param[in]      none
  * @retval         motor data point
  */
/**
  * @brief          返回yaw 6020电机数据指针
  * @param[in]      none
  * @retval         电机数据指针
  */
extern const motor_measure_t *get_yaw_gimbal_motor_measure_point(void);
extern const lkmotor_measure_t *get_yaw_gimbal_lkmotor_measure_point(void);

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
  * @brief          return the trigger 2006 motor data point
  * @param[in]      none
  * @retval         motor data point
  */
/**
  * @brief          返回拨弹电机 2006电机数据指针
  * @param[in]      none
  * @retval         电机数据指针
  */
extern const motor_measure_t *get_trigger_motor_measure_point(void);

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
extern chassis_data_t *get_Chassisdata_point(void);

#endif
