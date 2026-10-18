#include "main.h"


//pitch speed close-loop PID params, max out and max iout
//pitch 速度环 PID参数以及 PID最大输出，积分输出
extern float PITCH_SPEED_PID_KP;
extern float PITCH_SPEED_PID_KI;
extern float PITCH_SPEED_PID_KD;
extern float PITCH_SPEED_PID_MAX_OUT;
extern float PITCH_SPEED_PID_MAX_IOUT;

//pitch encode angle close-loop PID params, max out and max iout
//pitch 角度环 角度由编码器 PID参数以及 PID最大输出，积分输出
extern float PITCH_ENCODE_RELATIVE_PID_KP;
extern float PITCH_ENCODE_RELATIVE_PID_KI;
extern float PITCH_ENCODE_RELATIVE_PID_KD;

extern float PITCH_ENCODE_RELATIVE_PID_MAX_OUT;
extern float PITCH_ENCODE_RELATIVE_PID_MAX_IOUT;

//yaw speed close-loop PID params, max out and max iout
//yaw 速度环 PID参数以及 PID最大输出，积分输出
extern float YAW_SPEED_PID_KP;
extern float YAW_SPEED_PID_KI;
extern float YAW_SPEED_PID_KD;
extern float YAW_SPEED_PID_MAX_OUT;
extern float YAW_SPEED_PID_MAX_IOUT;

//yaw encode angle close-loop PID params, max out and max iout
//yaw 角度环 角度由编码器 PID参数以及 PID最大输出，积分输出
extern float YAW_ENCODE_RELATIVE_PID_KP;
extern float YAW_ENCODE_RELATIVE_PID_KI;
extern float YAW_ENCODE_RELATIVE_PID_KD;
extern float YAW_ENCODE_RELATIVE_PID_MAX_OUT;
extern float YAW_ENCODE_RELATIVE_PID_MAX_IOUT;