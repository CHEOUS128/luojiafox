/**
  ****************************(C) COPYRIGHT****************************
  * @file       RM_Cilent_UI.h
  * @brief      RM裁判系统客户端UI绘制库
  * @note       通过0x0301交互数据包在客户端绘制图形,
  *             图形数据结构与2021赛季裁判系统串口协议附录中
  *             graphic_data_struct_t(15字节)一致
  * @history
  *  Version    Date            Author          Modification
  *  V1.0.0                     luojiafox       1. done
  *
  ****************************(C) COPYRIGHT****************************
  */
#ifndef RM_CILENT_UI_H
#define RM_CILENT_UI_H

#include "struct_typedef.h"

/*----------------------图形操作类型----------------------*/
#define UI_Graph_Override 0u  //空操作
#define UI_Graph_ADD      1u  //增加图形
#define UI_Graph_Change   2u  //修改图形
#define UI_Graph_Del      3u  //删除图形

/*----------------------图形颜色----------------------*/
#define UI_Color_Main        0u  //红/蓝主色
#define UI_Color_Yellow      1u  //黄色
#define UI_Color_Green       2u  //绿色
#define UI_Color_Orange      3u  //橙色
#define UI_Color_Purplish_red 4u //紫红色
#define UI_Color_Pink        5u  //粉红色
#define UI_Color_Cyan        6u  //青色
#define UI_Color_Black       7u  //黑色
#define UI_Color_White       8u  //白色

/*----------------------图形类型----------------------*/
#define UI_Graph_Line      0u  //直线
#define UI_Graph_Rectangle 1u  //矩形
#define UI_Graph_Circle    2u  //整圆
#define UI_Graph_Ellipse   3u  //椭圆
#define UI_Graph_Arc       4u  //圆弧
#define UI_Graph_Float     5u  //浮点数
#define UI_Graph_Int       6u  //整型数
#define UI_Graph_Char      7u  //字符

#define UI_MAX_GRAPHIC_NUM 7u  //一帧最多图形个数

/**
  * @brief 图形数据结构体(用户填写用, 发送时按协议位域打包成15字节)
  * @note  圆弧: start_angle/end_angle为起始/终止角度, radius为半径;
  *        浮点/整型: 数值(32bit)存放在radius+end_x+end_y区域;
  *        字符: 内容3字节存放在start_angle+end_angle+radius区域.
  */
typedef struct
{
    char     graphic_name[3];

    uint32_t graphic_type;
    uint32_t operate_type;
    uint32_t layer;
    uint32_t color;

    uint16_t start_angle;
    uint16_t end_angle;
    uint16_t width;

    uint16_t start_x;
    uint16_t start_y;
    uint16_t radius;
    uint16_t end_x;
    uint16_t end_y;
} Graph_Data;

/*----------------------函数声明----------------------*/
extern void UI_ReFresh(uint16_t cnt, ...);
extern void UI_ReFresh_Buffer(uint16_t cnt, Graph_Data *graph_list);

extern void Line_Draw(Graph_Data *Ui_Grap_Data, char *Graphname, uint32_t Graph_Operate_Tpye, uint32_t Graph_Layer, uint32_t Color, uint32_t Line_Width, uint32_t Start_x, uint32_t Start_y, uint32_t End_x, uint32_t End_y);
extern void Circle_Draw(Graph_Data *Ui_Grap_Data, char *Graphname, uint32_t Graph_Operate_Tpye, uint32_t Graph_Layer, uint32_t Color, uint32_t Line_Width, uint32_t Center_x, uint32_t Center_y, uint32_t Rad);
extern void Arc_Draw(Graph_Data *Ui_Grap_Data, char *Graphname, uint32_t Graph_Operate_Tpye, uint32_t Graph_Layer, uint32_t Color, uint32_t Line_Width, uint32_t Start_Angle, uint32_t End_Angle, uint32_t Center_x, uint32_t Center_y, uint32_t Rad);
extern void Ellipse_Draw(Graph_Data *Ui_Grap_Data, char *Graphname, uint32_t Graph_Operate_Tpye, uint32_t Graph_Layer, uint32_t Color, uint32_t Line_Width, uint32_t Center_x, uint32_t Center_y, uint32_t Length_x, uint32_t Length_y);
extern void Float_Draw(Graph_Data *Ui_Grap_Data, char *Graphname, uint32_t Graph_Operate_Tpye, uint32_t Graph_Layer, uint32_t Color, uint32_t Int_Wid, uint32_t Start_x, uint32_t Start_y, uint32_t Dec_Wid, float Float_Data);
extern void Int_Draw(Graph_Data *Ui_Grap_Data, char *Graphname, uint32_t Graph_Operate_Tpye, uint32_t Graph_Layer, uint32_t Color, uint32_t Int_Wid, uint32_t Start_x, uint32_t Start_y, int32_t Int_Data);
extern void Char_Draw(Graph_Data *Ui_Grap_Data, char *Graphname, uint32_t Graph_Operate_Tpye, uint32_t Graph_Layer, uint32_t Color, uint32_t Char_Size, uint32_t Start_x, uint32_t Start_y, char *Char_Data);
extern void Graph_Del(Graph_Data *Ui_Grap_Data, char *Graphname, uint32_t Graph_Layer, uint32_t Graph_Operate_Tpye);

#endif
