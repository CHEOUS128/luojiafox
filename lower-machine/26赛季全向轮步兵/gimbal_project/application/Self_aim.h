#include "struct_typedef.h"
typedef struct {
    uint8_t     mode;       //0:自瞄红 1:自瞄蓝 2:小符红 3:小符蓝 4：大符红 5：大符蓝
    float       roll;
    float       pitch;      //角度制
    float       yaw;        //角度制
} __attribute__((packed)) OutputData;


typedef struct {
   // uint8_t fire_advicec;       //0:不发射 1:发射
   // float pitch;   
   // float yaw;             
   // float distance;之前的结构体
    uint8_t fire_advicec;   // 0:不发 1:发(视觉决策)
    float   yaw;            // 云台 yaw 目标角 (度)
    float   yaw_vel;        // yaw 角速度 (度/秒)
    float   yaw_acc;        // yaw 角加速度 (度/秒²)
    float   pitch;          // pitch 目标角 (度)
    float   pitch_vel;      // pitch 角速度 (度/秒)
    float   pitch_acc;      // pitch 角加速度 (度/秒²)
    float   distance;       // 目标距离 (米)
} __attribute__((packed)) InputData;

typedef struct  {
    uint8_t     ff;
} __attribute__((packed))FrameHeader;

typedef struct {
    uint8_t     crc8;       //目前不开校验，始终发0
    uint8_t     end ;       //结束位，0x0D
} __attribute__((packed))FrameTailer ;



typedef struct  {            
    FrameHeader frame_header;
    InputData   input_data;
    FrameTailer frame_tailer;   
} __attribute__((packed))RECEIVE_DATA;

typedef struct  {         
    FrameHeader frame_header;
    OutputData  output_data;
    FrameTailer frame_tailer;
	
} __attribute__((packed))SEND_DATA;

#define RECEIVE_DATA_SIZE 32//12之前的数据长度
#define SEND_DATA_SIZE 19
#define FRAME_HEADER      0X7B //Frame_header 
#define FRAME_TAIL        0X7D //Frame_tail 

#define FRAME_HEADER_FF  0xFF
// #define FRAME_HEADER_SOF  0xA5  
#define FRAME_HEADER_CRC8 0xFF  
#define FRAME_TAILER_CRC16_init 0xFFFF  
void Self_aim_task(void const *pvParameters);
InputData *get_selfaim_data(void);


extern uint8_t Usart_Receive[40]; //用于接收单个字节的数据
extern void usbd_cdc_receive_handle(uint8_t *data, uint16_t length);




