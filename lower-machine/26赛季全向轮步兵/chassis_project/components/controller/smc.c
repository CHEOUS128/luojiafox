/**
  ****************************(C) COPYRIGHT 2026 LuojiaFox****************************
  * @file       smc.c/h
  * @brief      云台Yaw轴滑模控制器
  * @note       包括初始化，目标量更新、状态量更新、控制量计算与直接控制量的发送
  * @history
  *  Version    Date            Author          Modification
  *  V1.0.0     2025-09-19      Bayek            1. done
  *  V2.0.0     2025-09-20      Bayek            1. 重建整体架构
  *                                              2. 增加了滑模控制器的模式设置
  *
  @verbatim
  ==============================================================================

  ==============================================================================
  @endverbatim
  ****************************(C) COPYRIGHT 2026 LuojiaFox****************************
*/
#include "smc.h"
#include "main.h"


/**
  * @brief          滑模控制器初始化
  * @param[out]     smc: 滑模控制器结构数据指针
  * @param[in]      param: 滑模参数(没用到的填0)
  * @param[in]      flag: 滑模控制类型标志
  * @param[in]      limit: 饱和函数限幅
  * @param[in]      u_max: 控制限幅
  * @param[in]      pos_esp: 误差阈值
  * @retval         none
  */
void SMC_init(Sliding *smc, const fp32 param_list[9], Rmode flag, float limit, float u_max, float pos_esp)
{
    smc->error.tar_now = 0;
    smc->error.tar_last = 0;
    smc->error.tar_differential = 0;

    smc->error.p_error = 0;
    smc->error.v_error = 0;
    smc->error.v_error_integral = 0;
//    smc.error.v_error_integral_max = 0;
    smc->error.pos_error_eps = 0;
    smc->error.vol_error_eps = 0;
    smc->error.pos_get = 0;
    smc->error.vol_get = 0;
    SMC_setParam(smc, param_list, flag, limit, u_max, pos_esp);
}

/**
  * @brief          滑模控制器参数设置
  * @param[out]     smc: 滑模控制器结构数据指针
  * @param[in]      param: 滑模参数(没用到的填0)
  * @param[in]      flag: 滑模控制类型标志
  * @param[in]      limit: 饱和函数限幅
  * @param[in]      u_max: 控制限幅
  * @param[in]      pos_esp: 误差阈值
  * @retval         none
  */
void SMC_setParam(Sliding *smc, const fp32 param_list[9], Rmode flag, float limit, float u_max, float pos_esp)
{
    smc->flag = flag;
    if(flag == EXPONENT || flag == POWER || flag == VELSMC)
    {
        smc->param.J = param_list[Jid];
        smc->param.K = param_list[Kid];
        smc->param.c = param_list[cid];
    }
    else if (flag == TFSMC)
    {
        smc->param.J = param_list[Jid];
        smc->param.K = param_list[Kid];
        smc->param.p = param_list[pid];
        smc->param.q = param_list[qid];
        smc->param.beta = param_list[betaid];
    }
    else if (flag == EISMC)
    {
        smc->param.J = param_list[Jid];
        smc->param.K = param_list[Kid];
        smc->param.c1 = param_list[c1id];
        smc->param.c2 = param_list[c2id];
    }
    smc->param.epsilon = param_list[epsilonid];
    smc->limit = limit;
    smc->error.pos_error_eps = pos_esp;
    smc->u_max = u_max;
    OutContinuation(smc);
}

/**
  * @brief          滑模控制器位置误差更新
  * @param[out]     smc: 滑模控制器结构数据指针
  * @param[in]      target: 目标位置
  * @param[in]      pos_now: 当前位置
  * @param[in]      vol_now: 当前速度
  * @retval         none
  */
void SMC_posErrorUpdate(Sliding *smc, fp32 target, fp32 pos_now, fp32 vol_now)
{
    smc->error.tar_now = target;
    smc->error.tar_differential = (fp32)((smc->error.tar_now - smc->error.tar_last)/(float)SAMPLE_PERIOD);

    smc->error.tar_differential_second = (fp32)((smc->error.tar_differential - smc->error.tar_differential_last)/(float)SAMPLE_PERIOD); ///二阶导

    smc->error.p_error = pos_now - target;
    smc->error.v_error = vol_now - smc->error.tar_differential;
    smc->error.tar_last = smc->error.tar_now;

    smc->error.p_error_integral += (fp32)(smc->error.p_error * (fp32)SAMPLE_PERIOD); ///位置误差积分

    smc->error.tar_differential_last = smc->error.tar_differential; ///二阶导保存
}

/**
  * @brief          滑模控制器速度误差更新
  * @param[out]     smc: 滑模控制器结构数据指针
  * @param[in]      target: 目标速度
  * @param[in]      vol_now: 当前速度
  * @retval         none
  */
void SMC_volErrorUpdate(Sliding *smc, fp32 target, fp32 vol_now)
{
    smc->error.tar_now = target;
    smc->error.tar_differential = (fp32)((smc->error.tar_now - smc->error.tar_last)/(float)SAMPLE_PERIOD);

//    smc.error.tar_differential_second = (float)((smc.error.tar_differential- smc.error.tar_differential_last)/SAMPLE_PERIOD); ///二阶导

    smc->error.v_error = vol_now - smc->error.tar_now;
    smc->error.v_error_integral += (fp32)(smc->error.v_error * (fp32)SAMPLE_PERIOD); ///速度误差积分
//    if(std::abs(smc.error.v_error_integral) > smc.error.v_error_integral_max) //积分限幅
//    {
//        smc->error.v_error_integral = smc->error.v_error_integral_max;
//    }

    smc->error.tar_last = smc->error.tar_now;

//    smc->error.tar_differential_last = smc->error.tar_differential; ///目标值一阶导保存 

}

/**
  * @brief          滑模控制器输出清除
  * @param[out]     smc: 滑模控制器结构数据指针
  * @retval         none
  */
void SMC_clear(Sliding *smc)
{
    smc->error.tar_now = 0;
    smc->error.tar_last = 0;
    smc->error.tar_differential = 0;

    smc->error.p_error = 0;
    smc->error.v_error = 0;
    smc->error.v_error_integral = 0;
//    smc->error.v_error_integral_max = 0;
    smc->error.pos_error_eps = 0;
    smc->error.vol_error_eps = 0;
    smc->error.pos_get = 0;
    smc->error.vol_get = 0;

    smc->error.tar_differential_second = 0;
    smc->error.tar_differential_last = 0;
    smc->error.p_error_integral = 0;
}

/**
  * @brief          滑模控制器积分值清除
  * @param[out]     smc: 滑模控制器结构数据指针
  * @retval         none
  */
void SMC_intgValClear(Sliding *smc)
{
    smc->error.v_error_integral = 0;
    smc->error.p_error_integral = 0;
}

/**
  * @brief          滑模控制器计算
  * @param[out]     smc: 滑模控制器结构数据指针
  * @retval         none
  */
fp32 SMC_calc(Sliding *smc)
{
    fp32 u,fun;
    
    switch (smc->flag) {
        case EXPONENT:///指数滑模面，指数趋近律
            
            if (fabs(smc->error.p_error) - smc->error.pos_error_eps < 0)
            {
                smc->error.p_error = 0;
                return 0;
            }
            
            smc->s = smc->param.c * smc->error.p_error + smc->error.v_error; //滑模面
            fun = Sat(smc);//饱和函数近似符号函数
//            u =  smc->param.J * ( (-smc->param.c * smc->error.v_error) - smc->param.K * smc->s - smc->param.epsilon * fun + smc->error.tar_differential_second); //控制律计算,指数趋近律
            u =  smc->param.J * ( (-smc->param.c * smc->error.v_error) - smc->param.K * smc->s - smc->param.epsilon * fun); //控制律计算,指数趋近律
            break;

        case POWER:///幂次滑模面，幂次趋近律
            
            if (fabs(smc->error.p_error) - smc->error.pos_error_eps < 0)
            {
                smc->error.p_error = 0;
                return 0;
            }
            
            smc->s = smc->param.c * smc->error.p_error + smc->error.v_error; //滑模面
            fun = Sat(smc);//饱和函数近似符号函数 
//            u =  smc->param.J * ( (-smc->param.c * smc->error.v_error) - smc->param.K * smc->s - smc->param.K * (std::pow(std::abs(smc->s),smc->param.epsilon)) * fun + smc->error.tar_differential_second); //控制律计算,幂次趋近律
            u =  smc->param.J * ( (-smc->param.c * smc->error.v_error) - smc->param.K * smc->s - smc->param.K * (pow(fabs(smc->s),smc->param.epsilon)) * fun); //控制律计算,幂次趋近律 
            break;

        case TFSMC:///tfsmc
        {
            static float pos_pow;//tfsmc 位置 幂
            if (fabs(smc->error.p_error) - smc->error.pos_error_eps < 0)
            {
                smc->error.p_error = 0;
                return 0;
            }
            
            pos_pow = pow(fabs(smc->error.p_error),smc->param.q/smc->param.p);
            if(smc->error.p_error<=0) pos_pow = -pos_pow;
            
            
            smc->s = smc->param.beta * pos_pow + smc->error.v_error; //滑模面
            
            fun = Sat(smc);//饱和函数近似符号函数  
            
            if(smc->error.p_error!=0)
            {
                u = smc->param.J * (smc->error.tar_differential_second//目标值的二阶导 根据需要是否保留前馈项
                             -smc->param.K * smc->s //s*K
                             -smc->param.epsilon * fun  //epsilon*SAT(S)
                             -smc->error.v_error * ((smc->param.q * smc->param.beta) * pos_pow) / (smc->param.p * smc->error.p_error)); //控制律计算    
            }
            else u = 0;
            break;
        }
    
        case VELSMC:///速度滑模面，指数趋近律，速度环
            smc->s = smc->error.v_error + smc->param.c * smc->error.v_error_integral; //滑模面
            fun = Sat(smc);//饱和函数近似符号函数
            u =  smc->param.J * (smc->error.tar_differential - (smc->param.c * smc->error.v_error) - smc->param.K * smc->s - smc->param.epsilon * fun); //控制律计算，速度环
            break;
            
        case EISMC:///积分滑模面，指数趋近律，位置环
            if (fabs(smc->error.p_error) - smc->error.pos_error_eps < 0)
            {
                smc->error.p_error = 0;
                return 0;
            }
            
            smc->s = smc->param.c1 * smc->error.p_error + smc->error.v_error + smc->param.c2 * smc->error.p_error_integral; //滑模面
            fun = Sat(smc);//饱和函数近似符号函数
            u =  smc->param.J * ( (-smc->param.c1 * smc->error.v_error)- smc->param.c2 * smc->error.p_error - smc->param.K * smc->s - smc->param.epsilon * fun); //控制律计算,指数趋近律
            break;
    }

    smc->error.error_last = smc->error.p_error; //保存上一次误差

    //控制量限幅
    if (u > smc->u_max)
    {
        u = smc->u_max;
    }
    if (u < -smc->u_max)
    {
        u = -smc->u_max;
    }
    smc->u = u;
    return u;
}

/**
  * @brief          滑模控制器输出连续性处理
  * @param[out]     smc: 滑模控制器结构数据指针
  * @retval         none
  */
void OutContinuation(Sliding *smc)
{
    if(smc->param.K != 0 && smc->param.c2 != 0)
    {
        smc->error.p_error_integral = (smc->param_last.K / smc->param.K) * (smc->param_last.c2 / smc->param.c2) * smc->error.p_error_integral;
        smc->error.v_error_integral = (smc->param_last.K / smc->param.K) * (smc->param_last.c / smc->param.c) * smc->error.v_error_integral;
    }
    smc->param_last = smc->param;
}

/**
  * @brief          滑模控制器符号函数
  * @param[in]      s: 滑模面
  * @retval         符号
  */
fp32 Signal(fp32 s)
{
    if (s > 0)
        return 1;
    else if (s == 0)
        return 0;
    else
        return -1;
}

/**
  * @brief          滑模控制器饱和函数
  * @param[in]      s: 滑模面
  * @retval         饱和后的值
  */
fp32 Sat(Sliding *smc)
{
    float y;
    y = smc->s / smc->param.epsilon;
    if (fabs(y) <= smc->limit)
        return y;
    else
        return Signal(y);
}
