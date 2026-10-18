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

#include "CAN_receive.h"

#include "cmsis_os.h"
#include "FreeRTOS.h"
#include "queue.h"
#include "task.h"

#include "main.h"

#include "detect_task.h"

extern CAN_HandleTypeDef hcan1;
extern CAN_HandleTypeDef hcan2;
// motor data read
#define get_motor_measure(ptr, data)                                 \
  {                                                                  \
    (ptr)->last_ecd = (ptr)->ecd;                                    \
    (ptr)->ecd = (uint16_t)((data)[0] << 8 | (data)[1]);             \
    (ptr)->speed_rpm = (uint16_t)((data)[2] << 8 | (data)[3]);       \
    (ptr)->given_current = (uint16_t)((data)[4] << 8 | (data)[5]);   \
    (ptr)->temperate = (data)[6];                                    \
    if ((ptr)->ecd - (ptr)->last_ecd > 4096)                         \
    {                                                                \
      (ptr)->ecd_count--;                                            \
    }                                                                \
    else if ((ptr)->ecd - (ptr)->last_ecd < -4096)                   \
    {                                                                \
      (ptr)->ecd_count++;                                            \
    }                                                                \
    (ptr)->angle = (ptr)->ecd_count * 360 + (ptr)->ecd * 360 / 8192; \
  }
#define get_lkmotor_measure(ptr, data)                                  \
  {                                                                     \
    (ptr)->temp = (int8_t)data[1];                                      \
    (ptr)->iq = (int16_t)(data[3] << 8 | data[2]);                      \
    (ptr)->speed = (int16_t)(data[5] << 8 | data[4]);                   \
    (ptr)->encoder = (uint16_t)(data[7] << 8 | data[6]);                \
    (ptr)->last_encoder = (ptr)->encoder;                               \
    (ptr)->angle = (float)(((float)(ptr)->last_encoder / 65536) * 360); \
  }
#define get_supercap_data(temp,data)                         \
  {                                                         \
    uint16_t *temp = (uint16_t *)data;                       \
    Power_data[0] =(float)temp[0]/100.f;                     \
    Power_data[1] =(float)temp[1]/100.f;                     \
    Power_data[2] =(float)temp[2]/100.f;                     \
    Power_data[3] =(float)temp[3]/100.f;                     \
  }
#define get_superpower_measure(ptr, data)                                  \
  {                                                                     \
    (ptr)->statusCode = (uint8_t)data[0];                                      \
    (ptr)->chassisPower = (((float)(uint16_t)(data[2] << 8 | data[1])) - 16384.0f) / 64.0f;                      \
    (ptr)->refereePower = (((float)(uint16_t)(data[4] << 8 | data[3])) - 16384.0f) / 64.0f;                   \
    (ptr)->chassisPowerLimit = (uint16_t)(data[6] << 8 | data[5]);                \
    (ptr)->capEnergy = (uint8_t)(((uint16_t)data[7] >= 250U) ? 100U : (((uint16_t)data[7] * 100U) / 250U));                               \
    (ptr)->updateTick = HAL_GetTick();                               \
  }
/*
motor data,  0:chassis motor1 3508;1:chassis motor3 3508;2:chassis motor3 3508;3:chassis motor4 3508;
4:yaw gimbal motor 6020;5:pitch gimbal motor 6020;6:trigger motor 2006;
电机数据, 0:底盘电机1 3508电机,  1:底盘电机2 3508电机,2:底盘电机3 3508电机,3:底盘电机4 3508电机;
4:yaw云台电机 6020电机; 5:pitch云台电机 6020电机; 6:拨弹电机 2006电机*/

motor_measure_t motor_chassis[7];
lkmotor_measure_t lkmotor_data;
super_power_receive_t super_power_data;
static chassis_data_t chassis_data;
#define CAN_TX_QUEUE_LENGTH 32U
#define CAN_TX_RETRY_DELAY_MS 1U
#define SUPER_POWER_OFFLINE_MS 100U

typedef struct
{
  CAN_HandleTypeDef *hcan;
  CAN_TxHeaderTypeDef header;
  uint8_t data[8];
} can_tx_frame_t;

static QueueHandle_t can_tx_queue = NULL;
static volatile uint32_t can_tx_drop_count = 0U;
static volatile uint32_t super_power_last_update_tick = 0U;
/* 超级电容反馈数据 */
float Power_data[4];
uint16_t power_data_temp[4];
uint8_t reset_flag = 0;

void CAN_tx_queue_init(void)
{
  if (can_tx_queue == NULL)
  {
    can_tx_queue = xQueueCreate(CAN_TX_QUEUE_LENGTH, sizeof(can_tx_frame_t));
  }
}

uint32_t CAN_get_tx_drop_count(void)
{
  return can_tx_drop_count;
}

uint8_t CAN_super_power_is_online(void)
{
  uint32_t now = HAL_GetTick();
  return (super_power_last_update_tick != 0U && (now - super_power_last_update_tick) < SUPER_POWER_OFFLINE_MS);
}

void CAN_get_super_power_data(super_power_receive_t *data)
{
  if (data != NULL)
  {
    taskENTER_CRITICAL();
    *data = super_power_data;
    taskEXIT_CRITICAL();
  }
}

static void CAN_queue_std_frame(CAN_HandleTypeDef *hcan, uint32_t std_id, uint8_t dlc, const uint8_t tx_data[8])
{
  can_tx_frame_t frame;

  if (hcan == NULL || tx_data == NULL || dlc > 8U)
  {
    can_tx_drop_count++;
    return;
  }

  CAN_tx_queue_init();
  if (can_tx_queue == NULL)
  {
    can_tx_drop_count++;
    return;
  }

  frame.hcan = hcan;
  frame.header.StdId = std_id;
  frame.header.ExtId = 0U;
  frame.header.IDE = CAN_ID_STD;
  frame.header.RTR = CAN_RTR_DATA;
  frame.header.DLC = dlc;
  frame.header.TransmitGlobalTime = DISABLE;

  for (uint8_t i = 0U; i < 8U; i++)
  {
    frame.data[i] = tx_data[i];
  }

  if (xQueueSend(can_tx_queue, &frame, 0U) != pdPASS)
  {
    can_tx_drop_count++;
  }
}

void CAN_tx_task(void const *pvParameters)
{
  can_tx_frame_t frame;
  uint32_t send_mail_box;
  uint8_t retry;
  (void)pvParameters;

  CAN_tx_queue_init();

  while (1)
  {
    if (can_tx_queue == NULL)
    {
      osDelay(10);
      CAN_tx_queue_init();
      continue;
    }

    if (xQueueReceive(can_tx_queue, &frame, portMAX_DELAY) == pdPASS)
    {
      retry = 0U;
      while (HAL_CAN_AddTxMessage(frame.hcan, &frame.header, frame.data, &send_mail_box) != HAL_OK)
      {
        if (++retry >= 5U)
        {
          can_tx_drop_count++;
          break;
        }
        osDelay(CAN_TX_RETRY_DELAY_MS);
      }
    }
  }
}


/**
 * @brief          将底盘速度设定值vx vy转化为浮点数
 * @param[in]      can接受数据
 * @retval         none
 */
void CAN_receive_chassis_data1(uint8_t rxdata[])
{
  send_float_typedef temp[2];
  temp[0].uint8_t[0] = rxdata[0];
  temp[0].uint8_t[1] = rxdata[1];
  temp[0].uint8_t[2] = rxdata[2];
  temp[0].uint8_t[3] = rxdata[3];

  temp[1].uint8_t[0] = rxdata[4];
  temp[1].uint8_t[1] = rxdata[5];
  temp[1].uint8_t[2] = rxdata[6];
  temp[1].uint8_t[3] = rxdata[7];
  chassis_data.vx_set = temp[0].float_t;
  chassis_data.vy_set = temp[1].float_t;
}
/**
 * @brief          将底盘旋转速度wz和底盘跟随云台角度angle转化为浮点数
 * @param[in]      can接受数据
 * @retval         none
 */
void CAN_receive_chassis_data2(uint8_t rxdata[])
{
  send_float_typedef temp[2];
  temp[0].uint8_t[0] = rxdata[0];
  temp[0].uint8_t[1] = rxdata[1];
  temp[0].uint8_t[2] = rxdata[2];
  temp[0].uint8_t[3] = rxdata[3];

  temp[1].uint8_t[0] = rxdata[4];
  temp[1].uint8_t[1] = rxdata[5];
  temp[1].uint8_t[2] = rxdata[6];
  temp[1].uint8_t[3] = rxdata[7];
  chassis_data.wz_set = temp[0].float_t;
  chassis_data.yaw_angle = temp[1].float_t;
  ;
}
/**
 * @brief          接受底盘模式和云台yaw轴角度
 * @param[in]      can接受数据
 * @retval         none
 */
void CAN_receive_chassis_data3(uint8_t rxdata[])
{
  send_float_typedef temp[1];
  temp[0].uint8_t[0] = rxdata[4];
  temp[0].uint8_t[1] = rxdata[5];
  temp[0].uint8_t[2] = rxdata[6];
  temp[0].uint8_t[3] = rxdata[7];
  chassis_data.chassis_mode = (uint16_t)(rxdata[0] << 8 | rxdata[1]);
  chassis_data.shoot_mode_receive = (uint16_t)(rxdata[2] << 8 | rxdata[3]);
  chassis_data.yaw_angle_set = temp[0].float_t;
}
/**
 * @brief          接受云台yaw轴角速度
 * @param[in]      can接受数据
 * @retval         none
 */
void CAN_receive_chassis_data4(uint8_t rxdata[])
{
  send_float_typedef temp[1];
  temp[0].uint8_t[0] = rxdata[0];
  temp[0].uint8_t[1] = rxdata[1];
  temp[0].uint8_t[2] = rxdata[2];
  temp[0].uint8_t[3] = rxdata[3];
  reset_flag = rxdata[4];
  chassis_data.yaw_gyro = temp[0].float_t;
}
/**
 * @brief          hal CAN fifo call back, receive motor data
 * @param[in]      hcan, the point to CAN handle
 * @retval         none
 */
/**
 * @brief          hal库CAN回调函数,接收电机数据
 * @param[in]      hcan:CAN句柄指针
 * @retval         none
 */
void CAN_receive_chassis_data5(uint8_t rxdata[])//新增
{
  send_float_typedef temp[2];
  temp[0].uint8_t[0] = rxdata[0];
  temp[0].uint8_t[1] = rxdata[1];
  temp[0].uint8_t[2] = rxdata[2];
  temp[0].uint8_t[3] = rxdata[3];
  temp[1].uint8_t[0] = rxdata[4];
  temp[1].uint8_t[1] = rxdata[5];
  temp[1].uint8_t[2] = rxdata[6];
  temp[1].uint8_t[3] = rxdata[7];
  chassis_data.yaw_vel = temp[0].float_t;
  chassis_data.yaw_acc = temp[1].float_t;
}
void HAL_CAN_RxFifo0MsgPendingCallback(CAN_HandleTypeDef *hcan)
{
  CAN_RxHeaderTypeDef rx_header;
  uint8_t rx_data[8];
  if (hcan == &hcan1)
  {
    HAL_CAN_GetRxMessage(hcan, CAN_RX_FIFO0, &rx_header, rx_data);

    switch (rx_header.StdId)
    {
    case CAN_3508_M1_ID:
    case CAN_3508_M2_ID:
    case CAN_3508_M3_ID:
    case CAN_3508_M4_ID:
    case CAN_YAW_MOTOR_ID:
    case CAN_TRIGGER_MOTOR_ID:
    {
      static uint8_t i = 0;
      // get motor id
      i = rx_header.StdId - CAN_3508_M1_ID;
      get_motor_measure(&motor_chassis[i], rx_data);
      detect_hook(CHASSIS_MOTOR1_TOE + i);
      break;
    }
    case CAN_LK_MOTOR_M1_ID:
    {
      get_lkmotor_measure(&lkmotor_data, rx_data);
      break;
        
    }
    case CAN_SuperPower_ID:
    {
      get_superpower_measure(&super_power_data, rx_data);
      super_power_last_update_tick = super_power_data.updateTick;
      break;
    }
    default:
    {
      break;
    }
    }
  }
  else if (hcan == &hcan2)
  {
    HAL_CAN_GetRxMessage(hcan, CAN_RX_FIFO0, &rx_header, rx_data);
    switch (rx_header.StdId)
    {
    case CAN_gmbial_chassis_data1:
    {
      CAN_receive_chassis_data1(rx_data);
      break;
    }
    case CAN_gmbial_chassis_data2:
    {
      CAN_receive_chassis_data2(rx_data);
      break;
    }
    case CAN_gmbial_chassis_data3:
    {
      CAN_receive_chassis_data3(rx_data);
      break;
    }
    case CAN_gmbial_chassis_data4:
    {
      CAN_receive_chassis_data4(rx_data);
      break;
    }
      case CAN_gimbal_chassis_data5:                 // 新增
    {
      CAN_receive_chassis_data5(rx_data);
      break;
    }
    default:
    {
      break;
    }
    }
  }
}

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
void CAN_cmd_gimbal(int16_t yaw, int16_t pitch, int16_t shoot, int16_t rev)
{
  uint8_t tx_data[8] = {0};
  tx_data[0] = (uint8_t)(yaw >> 8);
  tx_data[1] = (uint8_t)yaw;
  tx_data[2] = (uint8_t)(pitch >> 8);
  tx_data[3] = (uint8_t)pitch;
  tx_data[4] = (uint8_t)(shoot >> 8);
  tx_data[5] = (uint8_t)shoot;
  tx_data[6] = (uint8_t)(rev >> 8);
  tx_data[7] = (uint8_t)rev;
  CAN_queue_std_frame(&GIMBAL_CAN, CAN_GIMBAL_ALL_ID, 0x08, tx_data);
}

void CAN_cmd_chassis_reset_ID(void)
{
  uint8_t tx_data[8] = {0};
  CAN_queue_std_frame(&REFEREE_CAN, 0x700, 0x08, tx_data);
}

void CAN_cmd_chassis(int16_t motor1, int16_t motor2, int16_t motor3, int16_t motor4)
{
  uint8_t tx_data[8] = {0};
  tx_data[0] = (uint8_t)(motor1 >> 8);
  tx_data[1] = (uint8_t)motor1;
  tx_data[2] = (uint8_t)(motor2 >> 8);
  tx_data[3] = (uint8_t)motor2;
  tx_data[4] = (uint8_t)(motor3 >> 8);
  tx_data[5] = (uint8_t)motor3;
  tx_data[6] = (uint8_t)(motor4 >> 8);
  tx_data[7] = (uint8_t)motor4;
  CAN_queue_std_frame(&CHASSIS_CAN, CAN_CHASSIS_ALL_ID, 0x08, tx_data);
}

void CAN_cmd_referee_data(uint8_t bullet_freq, float bullet_speed, uint16_t chassis_power_limit_send)
{
  send_float_typedef temp;
  uint8_t tx_data[8] = {0};
  temp.float_t = bullet_speed;
  tx_data[0] = bullet_freq;
  tx_data[1] = temp.uint8_t[0];
  tx_data[2] = temp.uint8_t[1];
  tx_data[3] = temp.uint8_t[2];
  tx_data[4] = temp.uint8_t[3];
  tx_data[5] = (uint8_t)((chassis_power_limit_send >> 8) & 0xFF);
  tx_data[6] = (uint8_t)(chassis_power_limit_send & 0xFF);
  CAN_queue_std_frame(&hcan2, CAN_referee_data, 0x08, tx_data);
}

void CAN_INIT_STATUS(uint8_t status)
{
  uint8_t tx_data[8] = {0};
  tx_data[0] = status;
  CAN_queue_std_frame(&REFEREE_CAN, CAN_chassis_gimbal_ID, 0x01, tx_data);
}

void CAN_LK_START_control(void)
{
  uint8_t tx_data[8] = {0x88, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
  CAN_queue_std_frame(&CHASSIS_CAN, CAN_LK_MOTOR_M1_ID, 0x08, tx_data);
}

void CAN_LK_CLOSE_control(void)
{
  uint8_t tx_data[8] = {0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
  CAN_queue_std_frame(&CHASSIS_CAN, CAN_LK_MOTOR_M1_ID, 0x08, tx_data);
}

void CAN_LK_CLEAR_ERROR_control(void)
{
  uint8_t tx_data[8] = {0x9B, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
  CAN_queue_std_frame(&CHASSIS_CAN, CAN_LK_MOTOR_M1_ID, 0x08, tx_data);
}

void CAN_read_lkmotor_state(void)
{
  uint8_t tx_data[8] = {0x9C, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
  CAN_queue_std_frame(&hcan1, CAN_LK_MOTOR_M1_ID, 0x08, tx_data);
}

void CAN_LK_POSITION_Control(int32_t angleControl)
{
  uint8_t tx_data[8] = {0};
  tx_data[0] = 0xA3;
  tx_data[4] = *((uint8_t *)(&angleControl));
  tx_data[5] = *((uint8_t *)(&angleControl) + 1);
  tx_data[6] = *((uint8_t *)(&angleControl) + 2);
  tx_data[7] = *((uint8_t *)(&angleControl) + 3);
  CAN_queue_std_frame(&CHASSIS_CAN, CAN_LK_MOTOR_M1_ID, 0x08, tx_data);
}

void CAN_LK_SPEED_Control(int16_t iqControl, int32_t speedControl)
{
  uint8_t tx_data[8] = {0};
  tx_data[0] = 0xA2;
  tx_data[2] = *((uint8_t *)(&iqControl));
  tx_data[3] = *((uint8_t *)(&iqControl) + 1);
  tx_data[4] = *((uint8_t *)(&speedControl));
  tx_data[5] = *((uint8_t *)(&speedControl) + 1);
  tx_data[6] = *((uint8_t *)(&speedControl) + 2);
  tx_data[7] = *((uint8_t *)(&speedControl) + 3);
  CAN_queue_std_frame(&hcan1, CAN_LK_MOTOR_M1_ID, 0x08, tx_data);
}

void CAN_LK_Torque_Control(int16_t iqControl)
{
  uint8_t tx_data[8] = {0};
  tx_data[0] = 0xA1;
  tx_data[4] = *((uint8_t *)(&iqControl));
  tx_data[5] = *((uint8_t *)(&iqControl) + 1);
  CAN_queue_std_frame(&hcan1, CAN_LK_MOTOR_M1_ID, 0x08, tx_data);
}

void CAN_Send_Setpower(uint16_t setPower)
{
  uint8_t tx_data[8] = {0};
  tx_data[0] = (uint8_t)(setPower >> 8);
  tx_data[1] = (uint8_t)setPower;
  CAN_queue_std_frame(&CHASSIS_CAN, CAN_SUPER_CAP_SET_ID, 0x02, tx_data);
}

void CAN_SuperPower_Control(super_power_t super_power_data)
{
  uint8_t tx_data[8] = {0};
  uint8_t *p_data = (uint8_t *)&super_power_data;
  for (uint8_t i = 0U; i < 8U; i++)
  {
    tx_data[i] = p_data[i];
  }
  CAN_queue_std_frame(&CHASSIS_CAN, 0x61, 0x08, tx_data);
}

const motor_measure_t *get_yaw_gimbal_motor_measure_point(void)
{
  return &motor_chassis[4];
}
const lkmotor_measure_t *get_yaw_gimbal_lkmotor_measure_point(void)
{
  return &lkmotor_data;
}

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
const motor_measure_t *get_pitch_gimbal_motor_measure_point(void)
{
  return &motor_chassis[5];
}

/**
 * @brief          return the trigger 2006 motor data point
 * @param[in]      none
 * @retval         motor data point
 */
/**
 * @brief          返回拨弹电机 3508电机数据指针
 * @param[in]      none
 * @retval         电机数据指针
 */
const motor_measure_t *get_trigger_motor_measure_point(void)
{
  return &motor_chassis[6];
}

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
const motor_measure_t *get_chassis_motor_measure_point(uint8_t i)
{
  return &motor_chassis[(i & 0x03)];
}

/**
 * @brief          返回云台发送给底盘数据设定值指针
 * @retval         底盘数据设定值指针
 */
chassis_data_t *get_Chassisdata_point()
{
  return &chassis_data;
}
