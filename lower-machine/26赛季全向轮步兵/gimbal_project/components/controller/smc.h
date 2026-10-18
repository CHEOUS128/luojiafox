/**
  ****************************(C) COPYRIGHT 2026 LuojiaFox****************************
  * @file       slidingmodec.c/h
  * @brief      云台Yaw轴滑模控制器
  * @note       包括初始化，目标量更新、状态量更新、控制量计算与直接控制量的发送
  * @history
  *  Version    Date            Author          Modification
  *  V1.0.0     2025-09-19      Bayek            1. done
  *
  @verbatim
  ==============================================================================

  ==============================================================================
  @endverbatim
  ****************************(C) COPYRIGHT 2026 LuojiaFox****************************
*/

#ifndef SLIDINGMODEC_H_
#define SLIDINGMODEC_H_

#include "struct_typedef.h"
#include "math.h"

#define SAMPLE_PERIOD 0.002
#define V_EORROR_INTEGRAL_MAX 2000
#define P_EORROR_INTEGRAL_MAX 2000

/**
位置控制器(需两个反馈值)
EXPONENT -> 线性关系滑模面， 指数趋近率 -> 适用于Yaw(推荐)
POWER -> 线性关系滑模面， 幂次趋近率 -> 适用于Yaw(较推荐)
EISMC -> 比例积分关系滑模面， 指数趋近率 -> 适用于Pitch，拨弹轮
TFSMC -> 快速终端滑模控制 -> 适用于Yaw(较不推荐)

速度控制器(仅一个反馈值)
VELSMC -> 比例积分关系滑模面， 指数趋近率 -> 适用于摩擦轮、底盘电机等速度控制场景
*/
typedef enum {
    EXPONENT,
    POWER,
    TFSMC,
    VELSMC,
    EISMC
} Rmode;

typedef struct {

    fp32 tar_now;//当前目标值
    fp32 tar_last;//上一次目标值
    fp32 tar_differential;//目标值一阶导
    fp32 tar_differential_last;//上一次目标值一阶导
    fp32 tar_differential_second;//目标值二阶导

    float pos_get;//当前位置
    float vol_get;//当前速度

    float p_error;//位置误差
    float v_error;//速度误差（位置误差一阶导数）

    float p_error_integral;//位置误差积分
    float v_error_integral;//速度误差积分
//    float v_error_integral_max; //积分限幅

    float pos_error_eps;   //误差阈值
    float vol_error_eps;   //误差阈值
    float error_last;
}RError;

typedef struct {
    fp32 J;
    fp32 K;
    fp32 c;

    fp32 c1;   //EISMC增益系数1
    fp32 c2;   //EISMC增益系数2

    fp32 p;    //tfsmc幂次参数1 p>q
    fp32 q;    //tfsmc幂次参数2
    fp32 beta; //tfsmc比例系数
    fp32 epsilon; //趋近律参数
}SlidingParam;

typedef enum{
    Jid = 0,
    Kid = 1,
    cid = 2,
    c1id = 3,
    c2id = 4,
    pid = 5,
    qid = 6,
    betaid = 7,
    epsilonid = 8,
}SlidingParamIndex;

//EXPONENT
// J3 K200 C30 epsilon0.5

typedef struct {
    fp32 u; //控制输出
    fp32 s; //滑模面函数

    SlidingParam param;
    SlidingParam param_last;

    RError error;
    fp32 u_max; //控制限幅
    Rmode flag; //滑模控制类型标志
    fp32 limit; //饱和函数限幅
}Sliding;

/**
  * @brief          滑模控制器初始化
  * @param[out]     smc: 滑模控制器结构数据指针
  * @param[in]      param_list: 滑模参数列表
  * @param[in]      flag: 滑模控制类型标志
  * @param[in]      limit: 饱和函数限幅
  * @param[in]      u_max: 控制限幅
  * @param[in]      pos_esp: 误差阈值
  * @retval         none
  */
extern void SMC_init(Sliding *smc, const fp32 param_list[9], Rmode flag, float limit, float u_max, float pos_esp);

/**
  * @brief          滑模控制器参数设置
  * @param[out]     smc: 滑模控制器结构数据指针
  * @param[in]      param_list: 滑模参数列表
  * @param[in]      flag: 滑模控制类型标志
  * @param[in]      limit: 饱和函数限幅
  * @param[in]      u_max: 控制限幅
  * @param[in]      pos_esp: 误差阈值
  * @retval         none
  */
extern void SMC_setParam(Sliding *smc, const fp32 param_list[9], Rmode flag, float limit, float u_max, float pos_esp);

/**
  * @brief          滑模控制器位置误差更新
  * @param[out]     smc: 滑模控制器结构数据指针
  * @param[in]      target: 目标位置
  * @param[in]      pos_now: 当前位置
  * @param[in]      vol_now: 当前速度
  * @retval         none
  */
extern void SMC_posErrorUpdate(Sliding *smc, fp32 target, fp32 pos_now, fp32 vol_now); 

/**
  * @brief          滑模控制器速度误差更新
  * @param[out]     smc: 滑模控制器结构数据指针
  * @param[in]      target: 目标速度
  * @param[in]      vol_now: 当前速度
  * @retval         none
  */
extern void SMC_volErrorUpdate(Sliding *smc, fp32 target, fp32 vol_now);

/**
  * @brief          滑模控制器输出清除
  * @param[out]     smc: 滑模控制器结构数据指针
  * @retval         none
  */
extern void SMC_clear(Sliding *smc);

/**
  * @brief          滑模控制器积分值清除
  * @param[out]     smc: 滑模控制器结构数据指针
  * @retval         none
  */
extern void SMC_intgValClear(Sliding *smc);

/**
  * @brief          滑模控制器计算
  * @param[out]     smc: 滑模控制器结构数据指针
  * @param[in]      angle_now: 当前角度
  * @param[in]      angle_vel: 当前角速度
  * @retval         none
  */
extern fp32 SMC_calc(Sliding *smc);

/**
  * @brief          滑模控制器输出连续性处理
  * @param[out]     smc: 滑模控制器结构数据指针
  * @retval         none
  */
extern void OutContinuation(Sliding *smc);

/**
  * @brief          滑模控制器符号函数
  * @param[in]      s: 滑模面
  * @retval         符号
  */
extern fp32 Signal(fp32 s);

/**
  * @brief          滑模控制器饱和函数
  * @param[in]      s: 滑模面
  * @retval         饱和后的值
  */
extern fp32 Sat(Sliding *smc);

#endif
