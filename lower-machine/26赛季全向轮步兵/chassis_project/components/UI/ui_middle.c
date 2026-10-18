//
// Created by RM UI Designer
// Static Edition
//

#include <string.h>

#include "ui_interface.h"

ui_7_frame_t ui_middle_StaticGraphicGroup_0;

ui_interface_rect_t *ui_middle_StaticGraphicGroup_SelfaimRect = (ui_interface_rect_t*)&(ui_middle_StaticGraphicGroup_0.data[0]);
ui_interface_line_t *ui_middle_StaticGraphicGroup_CrosshairLine = (ui_interface_line_t*)&(ui_middle_StaticGraphicGroup_0.data[1]);
ui_interface_line_t *ui_middle_StaticGraphicGroup_GuideLine2 = (ui_interface_line_t*)&(ui_middle_StaticGraphicGroup_0.data[2]);
ui_interface_line_t *ui_middle_StaticGraphicGroup_GuideLine1 = (ui_interface_line_t*)&(ui_middle_StaticGraphicGroup_0.data[3]);
ui_interface_line_t *ui_middle_StaticGraphicGroup_CrosshairLine2 = (ui_interface_line_t*)&(ui_middle_StaticGraphicGroup_0.data[4]);
ui_interface_line_t *ui_middle_StaticGraphicGroup_CrosshairLine3 = (ui_interface_line_t*)&(ui_middle_StaticGraphicGroup_0.data[5]);
ui_interface_rect_t *ui_middle_StaticGraphicGroup_HeatRect = (ui_interface_rect_t*)&(ui_middle_StaticGraphicGroup_0.data[6]);

void _ui_init_middle_StaticGraphicGroup_0() {
    for (int i = 0; i < 7; i++) {
        ui_middle_StaticGraphicGroup_0.data[i].figure_name[0] = 1;
        ui_middle_StaticGraphicGroup_0.data[i].figure_name[1] = 0;
        ui_middle_StaticGraphicGroup_0.data[i].figure_name[2] = i + 0;
        ui_middle_StaticGraphicGroup_0.data[i].operate_type = 1;
    }
    for (int i = 7; i < 7; i++) {
        ui_middle_StaticGraphicGroup_0.data[i].operate_type = 0;
    }

    ui_middle_StaticGraphicGroup_SelfaimRect->figure_type = 1;
    ui_middle_StaticGraphicGroup_SelfaimRect->operate_type = 1;
    ui_middle_StaticGraphicGroup_SelfaimRect->layer = 0;
    ui_middle_StaticGraphicGroup_SelfaimRect->color = 6;
    ui_middle_StaticGraphicGroup_SelfaimRect->start_x = 710;
    ui_middle_StaticGraphicGroup_SelfaimRect->start_y = 320;
    ui_middle_StaticGraphicGroup_SelfaimRect->width = 3;
    ui_middle_StaticGraphicGroup_SelfaimRect->end_x = 1208;
    ui_middle_StaticGraphicGroup_SelfaimRect->end_y = 750;

    ui_middle_StaticGraphicGroup_CrosshairLine->figure_type = 0;
    ui_middle_StaticGraphicGroup_CrosshairLine->operate_type = 1;
    ui_middle_StaticGraphicGroup_CrosshairLine->layer = 0;
    ui_middle_StaticGraphicGroup_CrosshairLine->color = 1;
    ui_middle_StaticGraphicGroup_CrosshairLine->start_x = 760;
    ui_middle_StaticGraphicGroup_CrosshairLine->start_y = 540;
    ui_middle_StaticGraphicGroup_CrosshairLine->width = 3;
    ui_middle_StaticGraphicGroup_CrosshairLine->end_x = 1162;
    ui_middle_StaticGraphicGroup_CrosshairLine->end_y = 540;

    ui_middle_StaticGraphicGroup_GuideLine2->figure_type = 0;
    ui_middle_StaticGraphicGroup_GuideLine2->operate_type = 1;
    ui_middle_StaticGraphicGroup_GuideLine2->layer = 0;
    ui_middle_StaticGraphicGroup_GuideLine2->color = 3;
    ui_middle_StaticGraphicGroup_GuideLine2->start_x = 1359;
    ui_middle_StaticGraphicGroup_GuideLine2->start_y = 60;
    ui_middle_StaticGraphicGroup_GuideLine2->width = 3;
    ui_middle_StaticGraphicGroup_GuideLine2->end_x = 1260;
    ui_middle_StaticGraphicGroup_GuideLine2->end_y = 239;

    ui_middle_StaticGraphicGroup_GuideLine1->figure_type = 0;
    ui_middle_StaticGraphicGroup_GuideLine1->operate_type = 1;
    ui_middle_StaticGraphicGroup_GuideLine1->layer = 0;
    ui_middle_StaticGraphicGroup_GuideLine1->color = 3;
    ui_middle_StaticGraphicGroup_GuideLine1->start_x = 560;
    ui_middle_StaticGraphicGroup_GuideLine1->start_y = 60;
    ui_middle_StaticGraphicGroup_GuideLine1->width = 3;
    ui_middle_StaticGraphicGroup_GuideLine1->end_x = 659;
    ui_middle_StaticGraphicGroup_GuideLine1->end_y = 239;

    ui_middle_StaticGraphicGroup_CrosshairLine2->figure_type = 0;
    ui_middle_StaticGraphicGroup_CrosshairLine2->operate_type = 1;
    ui_middle_StaticGraphicGroup_CrosshairLine2->layer = 0;
    ui_middle_StaticGraphicGroup_CrosshairLine2->color = 1;
    ui_middle_StaticGraphicGroup_CrosshairLine2->start_x = 860;
    ui_middle_StaticGraphicGroup_CrosshairLine2->start_y = 417;
    ui_middle_StaticGraphicGroup_CrosshairLine2->width = 2;
    ui_middle_StaticGraphicGroup_CrosshairLine2->end_x = 1060;
    ui_middle_StaticGraphicGroup_CrosshairLine2->end_y = 417;

    ui_middle_StaticGraphicGroup_CrosshairLine3->figure_type = 0;
    ui_middle_StaticGraphicGroup_CrosshairLine3->operate_type = 1;
    ui_middle_StaticGraphicGroup_CrosshairLine3->layer = 0;
    ui_middle_StaticGraphicGroup_CrosshairLine3->color = 1;
    ui_middle_StaticGraphicGroup_CrosshairLine3->start_x = 960;
    ui_middle_StaticGraphicGroup_CrosshairLine3->start_y = 742;
    ui_middle_StaticGraphicGroup_CrosshairLine3->width = 3;
    ui_middle_StaticGraphicGroup_CrosshairLine3->end_x = 960;
    ui_middle_StaticGraphicGroup_CrosshairLine3->end_y = 340;

    ui_middle_StaticGraphicGroup_HeatRect->figure_type = 1;
    ui_middle_StaticGraphicGroup_HeatRect->operate_type = 1;
    ui_middle_StaticGraphicGroup_HeatRect->layer = 0;
    ui_middle_StaticGraphicGroup_HeatRect->color = 5;
    ui_middle_StaticGraphicGroup_HeatRect->start_x = 355;
    ui_middle_StaticGraphicGroup_HeatRect->start_y = 751;
    ui_middle_StaticGraphicGroup_HeatRect->width = 3;
    ui_middle_StaticGraphicGroup_HeatRect->end_x = 569;
    ui_middle_StaticGraphicGroup_HeatRect->end_y = 861;


    ui_proc_7_frame(&ui_middle_StaticGraphicGroup_0);
    SEND_MESSAGE((uint8_t *) &ui_middle_StaticGraphicGroup_0, sizeof(ui_middle_StaticGraphicGroup_0));
}

void _ui_update_middle_StaticGraphicGroup_0() {
    for (int i = 0; i < 7; i++) {
        ui_middle_StaticGraphicGroup_0.data[i].operate_type = 2;
    }

    ui_proc_7_frame(&ui_middle_StaticGraphicGroup_0);
    SEND_MESSAGE((uint8_t *) &ui_middle_StaticGraphicGroup_0, sizeof(ui_middle_StaticGraphicGroup_0));
}

void _ui_remove_middle_StaticGraphicGroup_0() {
    for (int i = 0; i < 7; i++) {
        ui_middle_StaticGraphicGroup_0.data[i].operate_type = 3;
    }

    ui_proc_7_frame(&ui_middle_StaticGraphicGroup_0);
    SEND_MESSAGE((uint8_t *) &ui_middle_StaticGraphicGroup_0, sizeof(ui_middle_StaticGraphicGroup_0));
}


void ui_init_middle_StaticGraphicGroup() {
    _ui_init_middle_StaticGraphicGroup_0();
}

void ui_update_middle_StaticGraphicGroup() {
    _ui_update_middle_StaticGraphicGroup_0();
}

void ui_remove_middle_StaticGraphicGroup() {
    _ui_remove_middle_StaticGraphicGroup_0();
}

