#include "high.h"

#include "Communicate.h"
#include "cmsis_os.h"
#include "arm_math.h"
#include "tim.h"

#ifdef __cplusplus //告诉编译器，这部分代码按C语言的格式进行编译，而不是C++的
extern "C"
{
#include "user_lib.h"
}
#endif

uint8_t sucker_state;

int32_t HIGH_LEN[AUTO_MODE_NUM] = {
        HIGH_INIT_LEN,
        HIGH_SKY_LEN,
        HIGH_STANDARD_LEN,
        HIGH_GROUND_LEN,
        HIGH_DELIVERY_LEN
    };

High high;

/**
 * @brief          遥控器的死区判断，因为遥控器的拨杆在中位的时候，不一定为0，
 * @param          输入的遥控器值
 * @param          输出的死区处理后遥控器值
 * @param          死区值
 */
#define rc_deadband_limit(input, output, dealine)        \
    {                                                    \
        if ((input) > (dealine) || (input) < -(dealine)) \
        {                                                \
            (output) = (input);                          \
        }                                                \
        else                                             \
        {                                                \
            (output) = 0;                                \
        }                                                \
    }


void High::init()
{
    high_RC = remote_control.get_remote_control_point();
    last_high_RC = remote_control.get_last_remote_control_point();
    sucker_state = close;
    for (uint8_t i = 0; i < 2; ++i)
    {

        //动力电机数据
        high_motive_motor[i].init(can_receive.get_chassis_high_motor_measure_point(i));
        //初始化pid
        fp32 high_speed_pid_parm[5] = {MOTIVE_MOTOR_SPEED_PID_KP, MOTIVE_MOTOR_SPEED_PID_KI, MOTIVE_MOTOR_SPEED_PID_KD, MOTIVE_MOTOR_SPEED_PID_MAX_IOUT, MOTIVE_MOTOR_SPEED_PID_MAX_OUT};
        high_motive_motor[i].speed_pid.init(PID_SPEED, high_speed_pid_parm, &high_motive_motor[i].speed, &high_motive_motor[i].speed_set, NULL);
        high_motive_motor[i].speed_pid.pid_clear();

        fp32 high_angle_pid_parm[5] = {MOTIVE_MOTOR_ANGLE_PID_KP, MOTIVE_MOTOR_ANGLE_PID_KI, MOTIVE_MOTOR_ANGLE_PID_KD, MOTIVE_MOTOR_ANGLE_PID_MAX_IOUT, MOTIVE_MOTOR_ANGLE_PID_MAX_OUT};
        high_motive_motor[i].angle_pid.init(PID_ANGLE, high_angle_pid_parm, &high_motive_motor[i].total_angle , &high_motive_motor[i].angle_set, 0);
        high_motive_motor[i].angle_pid.pid_clear();
        
        //设置初始值
        
        high_motive_motor[i].max_speed = NORMAL_MAX_HIGH_SPEED;
        high_motive_motor[i].min_speed = -NORMAL_MAX_HIGH_SPEED;

        motor_status[i] = WAIT;
    }
    // 电机软件限位，需要测试后开启
    
    vTaskDelay(1000);
    //更新一下数据
    feedback_update();

    moto_start_angle[CAN_LIFT_L_MOTOR] = high_motive_motor[CAN_LIFT_L_MOTOR].total_angle;
    high_motive_motor[CAN_LIFT_L_MOTOR].max_angle = + LIFT_LIMIT_ANGLE;
    high_motive_motor[CAN_LIFT_L_MOTOR].min_angle = - LIFT_LIMIT_ANGLE;
    
    moto_start_angle[CAN_LIFT_R_MOTOR] = high_motive_motor[CAN_LIFT_R_MOTOR].total_angle;
    high_motive_motor[CAN_LIFT_R_MOTOR].max_angle = - LIFT_LIMIT_ANGLE;
    high_motive_motor[CAN_LIFT_R_MOTOR].min_angle = + LIFT_LIMIT_ANGLE;

    save_state = back;
}

/**
 * @brief          状态更新函数
 * @param[in]
 * @retval         none
 */
void High::feedback_update(){
    //记录上一次遥控器值
    high_last_key_v = high_RC->key.v;
    sucker_state = temp_flag;
    //更新电机数据
    for (uint8_t i = 0; i < 2; ++i)
    {
        //更新动力电机速度
        high_motive_motor[i].speed = HIGH_MOTOR_RPM_TO_VECTOR_SEN * high_motive_motor[i].motor_measure->speed_rpm;
        high_motive_motor[i].total_angle = high_motive_motor[i].motor_measure->total_angle - moto_start_angle[i];
        high_motive_motor[i].angle_error = high_motive_motor[i].total_angle - high_motive_motor[i].angle_set;
        if (high_motive_motor[i].angle_error < ANGLE_ERR_TOLERANT &&  high_motive_motor[i].angle_error > -ANGLE_ERR_TOLERANT)
            motor_status[i] = READY;
        else
            motor_status[i] = WAIT;
    }

}

/**
 * @brief          行为切换设置
 * @param[in]
 * @retval         none
 */
void High::set_mode(){
    behaviour_mode_set();
}

/**
 * @brief          通过逻辑判断，赋值"high_behaviour_mode"成哪种模式
 * @param[in]
 * @retval         none
 */
void High::behaviour_mode_set()
{
    last_high_behaviour_mode = high_behaviour_mode;
    last_high_mode = high_mode;

    //遥控器设置模式
    if (switch_is_up(high_RC->rc.s[HIGH_MODE_CHANNEL])) //左拨杆上  minepush时可以抬左
    {
        high_behaviour_mode = HIGH_OPEN;
    }
    else if (switch_is_mid(high_RC->rc.s[HIGH_MODE_CHANNEL])) //左拨杆中 保持当前状态
    {
        high_behaviour_mode = HIGH_HOLD;
    }
    else if (switch_is_down(high_RC->rc.s[HIGH_MODE_CHANNEL])) //左拨杆下 无力
    {
        high_behaviour_mode = HIGH_ZERO_FORCE;
    }
 
    //根据行为模式选择一个控制模式
    if (high_behaviour_mode == HIGH_ZERO_FORCE || high_behaviour_mode == HIGH_OPEN)
    {
        high_mode = HIGH_HAND;
    }
    else if(high_behaviour_mode == HIGH_CLOSE)
    {
        high_mode = HIGH_AUTO;
    }else if(high_behaviour_mode == HIGH_HOLD){

        high_mode = HIGH_HOLDM;
    }

    //键鼠控制
    // if (if_key_singal_pessed(high_RC , last_high_RC , KEY_PRESSED_SUCKER_STATE))
    // {
    //     if (sucker_state == open)
    //         sucker_state = close;
    //     else    
    //         sucker_state = open;
    // }
    //吸盘开关
    // if(if_key_pessed(high_RC, '!') && if_key_pessed(high_RC, 'F'))//开气泵
    // {
    //     sucker_state = close;
    // }
    // else if (if_key_pessed(high_RC, '!') && if_key_pessed(high_RC, 'G'))
    // {
    //     sucker_state = open;
    // }

}



/**
 * @brief          设置控制设置值, 运动控制值是通过behaviour_control_set函数设置的
 * @param[out]
 * @retval         none
 */
void High::set_control()
{
    //TODO:暂时只用到两个通道值，分别控制拨矿电机和伸爪电机
    //vlift_set控制电机速度，vstretch_set控制电机速度, 
    fp32 vlift_set = 0.0f;

    //获取控制设置值
    behaviour_control_set(&vlift_set);

    if (high_mode == HIGH_HAND)
    {
        //同轴有一个是相反的
        high_motive_motor[CAN_LIFT_L_MOTOR].angle_set += vlift_set;
        high_motive_motor[CAN_LIFT_R_MOTOR].angle_set -= vlift_set;
        angle_hold_set = high_motive_motor[CAN_LIFT_L_MOTOR].angle_set;
    }
    if (high_mode == HIGH_HOLD){
        high_motive_motor[CAN_LIFT_L_MOTOR].angle_set = angle_hold_set;
        high_motive_motor[CAN_LIFT_R_MOTOR].angle_set = -1*angle_hold_set;
    }
    // 做角度限幅
    for (int i = 0;i < 2;i++)
    {
        // motor_angle_limit(&high_motive_motor[i]);
    }


}



/**
 * @brief          设置控制量.根据不同底盘控制模式，三个参数会控制不同运动.在这个函数里面，会调用不同的控制函数.
 * @param[out]     vhigh_set, 通常翻转机构纵向移动.
 * @param[out]     vstretch_set, 通常控制横向移动.
 * @param[out]     angle_set, 通常控制旋转运动.
 * @param[in]      包括底盘所有信息.
 * @retval         none
 */
void High::behaviour_control_set(fp32 *vhigh_set)
{
    if (vhigh_set == NULL)
    {
        return;
    }
    //无力
    if (high_behaviour_mode == HIGH_ZERO_FORCE)
    {
        *vhigh_set = 0.0f;
    }
    else if (high_behaviour_mode == HIGH_OPEN)
    {
        high_open_set_control(vhigh_set);

    }

    last_high_RC->key.v = high_RC->key.v;
}

/**
 * @brief          底盘开环的行为状态机下，底盘模式是raw原生状态，故而设定值会直接发送到can总线上
 * @param[in]      vx_set夹爪翻转的速度
 * @param[in]      vy_set抓取机构YAW轴的速度
 * @param[in]      wz_set吸盘旋转速度
 * @param[in]      数据
 * @retval         none
 */
void High::high_open_set_control(fp32 *vx_set)
{
    if (vx_set == NULL )
    {
        return;
    }
    static int16_t high_channel = 0;

    rc_deadband_limit(high_RC->rc.ch[HIGH_X_CHANNEL], high_channel, RC_DEADBAND);
    *vx_set = high_RC->rc.ch[HIGH_X_CHANNEL] / HIGH_OPEN_RC_SCALE;

}


/**
 * @brief          解算数据,并进行pid计算
 * @param[out]
 * @retval         none
 */
void High::solve()
{
    if (high_behaviour_mode == HIGH_OPEN || high_behaviour_mode == HIGH_HOLD || high_behaviour_mode == HIGH_CLOSE)
        for (int i = 0; i < 2; i++)
        {
            motor_set_control(&high_motive_motor[i]);
        }
}

/**
 * @brief         输出电流
 * @param[in]
 * @retval         none
 */
void High::output()
{
     for (int i = 0; i < 2; i++)
    {
        high_motive_motor[i].current_give = (int16_t)high_motive_motor[i].current_set;
    }
    if (high_behaviour_mode == HIGH_ZERO_FORCE)
    {
        for (int i = 0; i < 2; i++)
        {
            high_motive_motor[i].current_give = 0.0f;
        }
    }

    can_receive.can_cmd_chassis_high_motor(high_motive_motor[CAN_LIFT_L_MOTOR].current_give, high_motive_motor[CAN_LIFT_R_MOTOR].current_give);
    // can_receive.can_cmd_chassis_high_motor(0, 0);
}

/**
 * @brief          云台控制模式:GIMBAL_MOTOR_ENCONDE，使用编码相对角进行控制
 * @param[out]     gimbal_motor:yaw电机或者pitch电机
 * @retval         none
 */

void High::motor_set_control(M3508_motor *motor)
{
    if (motor == NULL)
    {
        return;
    }

    motor->speed_set = motor->angle_pid.pid_calc();
    if (motor->speed_set > motor->max_speed)
        motor->speed_set = motor->max_speed;
    if (motor->speed_set < motor->min_speed)
        motor->speed_set = motor->min_speed;
    motor->current_set = motor->speed_pid.pid_calc();
    
}

// void High::motor_angle_limit(M3508_motor *motor)
// {
//     if (motor->total_angle < motor->min_angle)
//     {
//         motor->total_angle = motor->min_angle;
//     }
//     else if (motor->total_angle > motor->max_angle)
//     {
//         motor->total_angle = motor->max_angle;
//     }
// }


/**
 * @brief          自动模式控制电机转动角度
 * @param[out]     add: 角度增加量
 * @retval         none
 */
void High::auto_control(auto_mode_e *auto_mode)
{
    switch(*auto_mode)
    {
        case HIGH_INIT:
        case HIGH_SKY:
        case HIGH_STANDARD:
        case HIGH_GROUND:
        case HIGH_DELIVERY:
        {
            static int AUTO_MODE;
            AUTO_MODE = *auto_mode - HIGH_INIT;
            high_motive_motor[CAN_HIGH_L_MOTOR].angle_set = + HIGH_LEN[AUTO_MODE];
            high_motive_motor[CAN_HIGH_R_MOTOR].angle_set = - HIGH_LEN[AUTO_MODE];
        }
    }
}

//================================================================

void High::save_init()
{
    	HAL_TIM_Base_Start(&htim1);
	    HAL_TIM_Base_Start(&htim5);
        HAL_TIM_PWM_Start(&htim1, TIM_CHANNEL_1);
        HAL_TIM_PWM_Start(&htim1, TIM_CHANNEL_2);
		HAL_TIM_PWM_Start(&htim5, TIM_CHANNEL_1);
}

void High::save_rc_control(){
    if (switch_is_up(high_RC->rc.s[1])){//左拨杆向上

        if (left_rocker_right)
        {
            save_state = down; 
        }
        else if(left_rocker_left)
        {
            save_state = back;
        }

    }
}

void High::save_control_send(){
    if (SAVE_DOWN)
    {
        __HAL_TIM_SetCompare(&htim1, TIM_CHANNEL_1, SaveDownPosition_L);
		__HAL_TIM_SetCompare(&htim1, TIM_CHANNEL_2, SaveDownPosition_R);
    }
    else if (SAVE_BACK)
    {
        __HAL_TIM_SetCompare(&htim1, TIM_CHANNEL_1, SaveBackPosition_L);
		__HAL_TIM_SetCompare(&htim1, TIM_CHANNEL_2, SaveBackPosition_R);
    }
}

void High::sucker_contorl_send(){
        if (sucker_state == open)
        {
            HAL_GPIO_WritePin(GPIOB,GPIO_PIN_12,GPIO_PIN_SET);
        }
        else if (sucker_state == close)
        {
            HAL_GPIO_WritePin(GPIOB,GPIO_PIN_12,GPIO_PIN_RESET); 
        } 

}