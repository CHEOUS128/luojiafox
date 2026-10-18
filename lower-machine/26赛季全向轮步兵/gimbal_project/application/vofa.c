#include "vofa.h"
#include "usart.h"

//pitch speed close-loop PID params, max out and max iout
//pitch 速度环 PID参数以及 PID最大输出，积分输出
float PITCH_SPEED_PID_KP=        3000.0f;  //3000
float PITCH_SPEED_PID_KI=        0.0f;//60.0f
float PITCH_SPEED_PID_KD=        0.0f;
float PITCH_SPEED_PID_MAX_OUT=   60000;//60000.0f;
float PITCH_SPEED_PID_MAX_IOUT=  10000.0f;

//pitch encode angle close-loop PID params, max out and max iout
//pitch 角度环 角度由编码器 PID参数以及 PID最大输出，积分输出
float PITCH_ENCODE_RELATIVE_PID_KP= 12.0f;
float PITCH_ENCODE_RELATIVE_PID_KI= 0.8f;
float PITCH_ENCODE_RELATIVE_PID_KD= 0.2f;
float PITCH_ENCODE_RELATIVE_PID_MAX_OUT= 12.0f;
float PITCH_ENCODE_RELATIVE_PID_MAX_IOUT= 1.0f;


//yaw speed close-loop PID params, max out and max iout
//yaw 速度环 PID参数以及 PID最大输出，积分输出
float YAW_SPEED_PID_KP =       6000;//6000.0f;
float YAW_SPEED_PID_KI =       20.0f;		//0.0f
float YAW_SPEED_PID_KD  =      0.0f;
float YAW_SPEED_PID_MAX_OUT=   28000.0f;//28000
float YAW_SPEED_PID_MAX_IOUT=  500.0f;

//yaw encode angle close-loop PID params, max out and max iout
//yaw 角度环 角度由编码器 PID参数以及 PID最大输出，积分输出
//float YAW_ENCODE_RELATIVE_PID_KP=        42.0f;//42
//float YAW_ENCODE_RELATIVE_PID_KI=        0.3f; //0.3
//float YAW_ENCODE_RELATIVE_PID_KD=        12.0f;//3.5f   6.5f
//float YAW_ENCODE_RELATIVE_PID_MAX_OUT=   8.0f;//10
//float YAW_ENCODE_RELATIVE_PID_MAX_IOUT = 2.0f;
float YAW_ENCODE_RELATIVE_PID_KP=        26.0f;//42
float YAW_ENCODE_RELATIVE_PID_KI=        0.0f; //0.3
float YAW_ENCODE_RELATIVE_PID_KD=        2.0f;//3.5f   6.5f
float YAW_ENCODE_RELATIVE_PID_MAX_OUT=   10.0f;//10
float YAW_ENCODE_RELATIVE_PID_MAX_IOUT = 0.0f;