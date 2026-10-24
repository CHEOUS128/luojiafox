/**
  ****************************(C) COPYRIGHT****************************
  * @file       RM_Cilent_UI.c
  * @brief      RM裁判系统客户端UI绘制库
  * @note       通过0x0301交互数据包在客户端绘制图形, 经USART6 DMA发送
  * @history
  *  Version    Date            Author          Modification
  *  V1.0.0                     luojiafox       1. done
  *
  ****************************(C) COPYRIGHT****************************
  */
#include "RM_Cilent_UI.h"
#include "string.h"
#include "stdarg.h"
#include "referee.h"
#include "protocol.h"
#include "CRC8_CRC16.h"
#include "bsp_usart.h"

/* 0x0301交互数据发送函数(定义在bsp_usart.c) */
extern void usart6_tx_dma_enable(uint8_t *data, uint16_t len);

#define UI_FRAME_CMD_ID     0x0301  //交互数据命令码
#define UI_GRAPH_ADD_CMD    0x0110  //图形增加
#define UI_GRAPH_CHANGE_CMD 0x0111  //图形修改
#define UI_GRAPH_DEL_CMD    0x0112  //图形删除

/* 客户端ID = 0x0100 + 机器人ID */
#define UI_CLIENT_ID_OFFSET 0x0100

/* 帧头(5) + cmdID(2) + 数据段(6+15*n) + CRC16(2), n最大7 */
static uint8_t UI_frame_buf[9 + 6 + 15 * UI_MAX_GRAPHIC_NUM + 2];
static uint8_t UI_seq = 0;

/**
  * @brief  按LSB优先将val的bits位填入buf(从第*pos位开始), 与协议位域排布一致
  */
static void UI_put_bits(uint8_t *buf, uint32_t *pos, uint32_t val, uint8_t bits)
{
    uint8_t i;
    for (i = 0; i < bits; i++)
    {
        if ((val >> i) & 1u)
        {
            buf[3 + ((*pos + i) >> 3)] |= (uint8_t)(1u << ((*pos + i) & 7u));
        }
    }
    *pos += bits;
}

/**
  * @brief  将Graph_Data按协议打包成15字节图形数据
  */
static void UI_pack_graph(const Graph_Data *g, uint8_t *out)
{
    uint32_t pos = 0;
    uint8_t i;

    for (i = 0; i < 15; i++)
    {
        out[i] = 0;
    }
    out[0] = (uint8_t)g->graphic_name[0];
    out[1] = (uint8_t)g->graphic_name[1];
    out[2] = (uint8_t)g->graphic_name[2];

    UI_put_bits(out, &pos, g->operate_type, 3);
    UI_put_bits(out, &pos, g->graphic_type, 3);
    UI_put_bits(out, &pos, g->layer, 4);
    UI_put_bits(out, &pos, g->color, 4);
    UI_put_bits(out, &pos, g->start_angle, 9);
    UI_put_bits(out, &pos, g->end_angle, 9);
    UI_put_bits(out, &pos, g->width, 10);
    UI_put_bits(out, &pos, g->start_x, 11);
    UI_put_bits(out, &pos, g->start_y, 11);
    UI_put_bits(out, &pos, g->radius, 10);
    UI_put_bits(out, &pos, g->end_x, 11);
    UI_put_bits(out, &pos, g->end_y, 11);
}

/**
  * @brief  按首图形操作类型选择data_cmd_id并发送
  * @param  cnt: 图形个数(1~7)
  * @param  graph_list: 图形数组
  */
void UI_ReFresh_Buffer(uint16_t cnt, Graph_Data *graph_list)
{
    uint8_t robot_id;
    uint16_t data_len;
    uint16_t frame_len;
    uint16_t i;
    uint16_t data_cmd_id;

    if (cnt == 0 || cnt > UI_MAX_GRAPHIC_NUM || graph_list == NULL)
    {
        return;
    }
    robot_id = get_robot_id();
    if (robot_id == 0) //裁判系统离线
    {
        return;
    }

    switch (graph_list[0].operate_type)
    {
        case UI_Graph_ADD:
        {
            data_cmd_id = UI_GRAPH_ADD_CMD;
            break;
        }
        case UI_Graph_Change:
        {
            data_cmd_id = UI_GRAPH_CHANGE_CMD;
            break;
        }
        default:
        {
            data_cmd_id = UI_GRAPH_DEL_CMD;
            break;
        }
    }

    data_len = 6 + 15 * cnt;
    frame_len = REF_HEADER_CRC_CMDID_LEN + data_len;

    memset(UI_frame_buf, 0, frame_len);
    UI_frame_buf[0] = HEADER_SOF;
    UI_frame_buf[1] = (uint8_t)(data_len & 0xFF);
    UI_frame_buf[2] = (uint8_t)((data_len >> 8) & 0xFF);
    UI_frame_buf[3] = UI_seq;
    if (++UI_seq > 0xEF)
    {
        UI_seq = 0;
    }
    append_CRC8_check_sum(UI_frame_buf, 5);

    UI_frame_buf[5] = (uint8_t)(UI_FRAME_CMD_ID & 0xFF);
    UI_frame_buf[6] = (uint8_t)((UI_FRAME_CMD_ID >> 8) & 0xFF);

    UI_frame_buf[7] = (uint8_t)(data_cmd_id & 0xFF);
    UI_frame_buf[8] = (uint8_t)((data_cmd_id >> 8) & 0xFF);
    UI_frame_buf[9] = robot_id;
    UI_frame_buf[10] = 0;
    UI_frame_buf[11] = (uint8_t)((UI_CLIENT_ID_OFFSET + robot_id) & 0xFF);
    UI_frame_buf[12] = (uint8_t)(((UI_CLIENT_ID_OFFSET + robot_id) >> 8) & 0xFF);

    for (i = 0; i < cnt; i++)
    {
        UI_pack_graph(&graph_list[i], &UI_frame_buf[13 + 15 * i]);
    }

    append_CRC16_check_sum(UI_frame_buf, frame_len);
    usart6_tx_dma_enable(UI_frame_buf, (uint16_t)frame_len);
}

/**
  * @brief  刷新cnt个图形, 用法: UI_ReFresh(2, G1, G2);
  */
void UI_ReFresh(uint16_t cnt, ...)
{
    va_list ap;
    uint16_t i;
    Graph_Data graph_list[UI_MAX_GRAPHIC_NUM];

    if (cnt == 0 || cnt > UI_MAX_GRAPHIC_NUM)
    {
        return;
    }

    va_start(ap, cnt);
    for (i = 0; i < cnt; i++)
    {
        graph_list[i] = va_arg(ap, Graph_Data);
    }
    va_end(ap);

    UI_ReFresh_Buffer(cnt, graph_list);
}

/**
  * @brief  图形公共参数填充
  */
static void UI_graph_common(Graph_Data *g, char *Graphname, uint32_t op, uint32_t layer, uint32_t color, uint32_t type)
{
    memset(g, 0, sizeof(Graph_Data));
    g->graphic_name[0] = Graphname[0];
    g->graphic_name[1] = Graphname[1];
    g->graphic_name[2] = Graphname[2];
    g->operate_type = op;
    g->graphic_type = type;
    g->layer = layer;
    g->color = color;
}

void Line_Draw(Graph_Data *Ui_Grap_Data, char *Graphname, uint32_t Graph_Operate_Tpye, uint32_t Graph_Layer, uint32_t Color, uint32_t Line_Width, uint32_t Start_x, uint32_t Start_y, uint32_t End_x, uint32_t End_y)
{
    UI_graph_common(Ui_Grap_Data, Graphname, Graph_Operate_Tpye, Graph_Layer, Color, UI_Graph_Line);
    Ui_Grap_Data->width = Line_Width;
    Ui_Grap_Data->start_x = Start_x;
    Ui_Grap_Data->start_y = Start_y;
    Ui_Grap_Data->end_x = End_x;
    Ui_Grap_Data->end_y = End_y;
}

void Circle_Draw(Graph_Data *Ui_Grap_Data, char *Graphname, uint32_t Graph_Operate_Tpye, uint32_t Graph_Layer, uint32_t Color, uint32_t Line_Width, uint32_t Center_x, uint32_t Center_y, uint32_t Rad)
{
    UI_graph_common(Ui_Grap_Data, Graphname, Graph_Operate_Tpye, Graph_Layer, Color, UI_Graph_Circle);
    Ui_Grap_Data->width = Line_Width;
    Ui_Grap_Data->start_x = Center_x;
    Ui_Grap_Data->start_y = Center_y;
    Ui_Grap_Data->radius = Rad;
}

void Arc_Draw(Graph_Data *Ui_Grap_Data, char *Graphname, uint32_t Graph_Operate_Tpye, uint32_t Graph_Layer, uint32_t Color, uint32_t Line_Width, uint32_t Start_Angle, uint32_t End_Angle, uint32_t Center_x, uint32_t Center_y, uint32_t Rad)
{
    UI_graph_common(Ui_Grap_Data, Graphname, Graph_Operate_Tpye, Graph_Layer, Color, UI_Graph_Arc);
    Ui_Grap_Data->start_angle = Start_Angle;
    Ui_Grap_Data->end_angle = End_Angle;
    Ui_Grap_Data->width = Line_Width;
    Ui_Grap_Data->start_x = Center_x;
    Ui_Grap_Data->start_y = Center_y;
    Ui_Grap_Data->radius = Rad;
}

void Ellipse_Draw(Graph_Data *Ui_Grap_Data, char *Graphname, uint32_t Graph_Operate_Tpye, uint32_t Graph_Layer, uint32_t Color, uint32_t Line_Width, uint32_t Center_x, uint32_t Center_y, uint32_t Length_x, uint32_t Length_y)
{
    UI_graph_common(Ui_Grap_Data, Graphname, Graph_Operate_Tpye, Graph_Layer, Color, UI_Graph_Ellipse);
    Ui_Grap_Data->width = Line_Width;
    Ui_Grap_Data->start_x = Center_x;
    Ui_Grap_Data->start_y = Center_y;
    Ui_Grap_Data->end_x = Length_x;
    Ui_Grap_Data->end_y = Length_y;
}

void Float_Draw(Graph_Data *Ui_Grap_Data, char *Graphname, uint32_t Graph_Operate_Tpye, uint32_t Graph_Layer, uint32_t Color, uint32_t Int_Wid, uint32_t Start_x, uint32_t Start_y, uint32_t Dec_Wid, float Float_Data)
{
    union
    {
        float f;
        uint32_t u;
    } val;

    UI_graph_common(Ui_Grap_Data, Graphname, Graph_Operate_Tpye, Graph_Layer, Color, UI_Graph_Float);
    Ui_Grap_Data->width = Int_Wid; //整数位数
    Ui_Grap_Data->start_x = Start_x;
    Ui_Grap_Data->start_y = Start_y;
    Ui_Grap_Data->start_angle = Dec_Wid; //小数位数
    val.f = Float_Data;
    Ui_Grap_Data->radius = val.u & 0x3FF;           //数值bit0~9
    Ui_Grap_Data->end_x = (val.u >> 10) & 0x7FF;    //数值bit10~20
    Ui_Grap_Data->end_y = (val.u >> 21) & 0x7FF;    //数值bit21~31
}

void Int_Draw(Graph_Data *Ui_Grap_Data, char *Graphname, uint32_t Graph_Operate_Tpye, uint32_t Graph_Layer, uint32_t Color, uint32_t Int_Wid, uint32_t Start_x, uint32_t Start_y, int32_t Int_Data)
{
    uint32_t val = (uint32_t)Int_Data;

    UI_graph_common(Ui_Grap_Data, Graphname, Graph_Operate_Tpye, Graph_Layer, Color, UI_Graph_Int);
    Ui_Grap_Data->width = Int_Wid; //整数位数
    Ui_Grap_Data->start_x = Start_x;
    Ui_Grap_Data->start_y = Start_y;
    Ui_Grap_Data->start_angle = 0;
    Ui_Grap_Data->radius = val & 0x3FF;
    Ui_Grap_Data->end_x = (val >> 10) & 0x7FF;
    Ui_Grap_Data->end_y = (val >> 21) & 0x7FF;
}

void Char_Draw(Graph_Data *Ui_Grap_Data, char *Graphname, uint32_t Graph_Operate_Tpye, uint32_t Graph_Layer, uint32_t Color, uint32_t Char_Size, uint32_t Start_x, uint32_t Start_y, char *Char_Data)
{
    uint32_t val;

    UI_graph_common(Ui_Grap_Data, Graphname, Graph_Operate_Tpye, Graph_Layer, Color, UI_Graph_Char);
    Ui_Grap_Data->width = Char_Size; //字符大小10~63
    Ui_Grap_Data->start_x = Start_x;
    Ui_Grap_Data->start_y = Start_y;
    val = ((uint32_t)(uint8_t)Char_Data[0])
        | ((uint32_t)(uint8_t)Char_Data[1] << 8)
        | ((uint32_t)(uint8_t)Char_Data[2] << 16);
    Ui_Grap_Data->start_angle = val & 0x1FF;
    Ui_Grap_Data->end_angle = (val >> 9) & 0x1FF;
    Ui_Grap_Data->radius = (val >> 18) & 0x3FF;
}

void Graph_Del(Graph_Data *Ui_Grap_Data, char *Graphname, uint32_t Graph_Layer, uint32_t Graph_Operate_Tpye)
{
    UI_graph_common(Ui_Grap_Data, Graphname, Graph_Operate_Tpye, Graph_Layer, 0, UI_Graph_Line);
}
