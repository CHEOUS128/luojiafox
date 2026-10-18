#include "Chassis_power_control.h"
#include "CAN_receive.h"

#define POWER_LIMIT_START_RATIO 0.90f
#define POWER_LIMIT_RELEASE_RATIO 0.85f
#define POWER_SCALE_MIN 0.05f
#define POWER_SCALE_RELEASE_STEP 0.005f
#define POWER_LIMIT_MIN_VALID 1.0f

fp32 Plimit = 1.0f;
super_power_receive_t feedback;

static fp32 clamp_float(fp32 value, fp32 min_value, fp32 max_value)
{
    if (value > max_value)
    {
        return max_value;
    }
    if (value < min_value)
    {
        return min_value;
    }
    return value;
}

void chassis_power_limit(chassis_move_t *chassis_move_control)
{
    // fp32 target_scale = Plimit;

    // (void)chassis_move_control;
    // CAN_get_super_power_data(&feedback);

    // if (!CAN_super_power_is_online() ||
    //     feedback.chassisPowerLimit < POWER_LIMIT_MIN_VALID)
    // {
    //     target_scale = POWER_SCALE_MIN;
    // }
    // else
    // {
    //     const fp32 power_limit = (fp32)feedback.chassisPowerLimit;
    //     const fp32 limit_start = POWER_LIMIT_START_RATIO * power_limit;
    //     const fp32 limit_release = POWER_LIMIT_RELEASE_RATIO * power_limit;

    //     if (feedback.chassisPower > limit_start)
    //     {
    //         target_scale = limit_start / feedback.chassisPower;
    //     }
    //     else if (feedback.chassisPower < limit_release)
    //     {
    //         target_scale = 1.0f;
    //     }
    // }

    // target_scale = clamp_float(target_scale, POWER_SCALE_MIN, 1.0f);

    // /* Reduce immediately on overload, but recover slowly to avoid oscillation. */
    // if (target_scale < Plimit)
    // {
    //     Plimit = target_scale;
    // }
    // else if (target_scale > Plimit)
    // {
    //     Plimit += POWER_SCALE_RELEASE_STEP;
    //     if (Plimit > target_scale)
    //     {
    //         Plimit = target_scale;
    //     }
    // }
}
