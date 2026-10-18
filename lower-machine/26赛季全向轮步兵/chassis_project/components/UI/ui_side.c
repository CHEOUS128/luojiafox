//
// Created by RM UI Designer
// Static Edition
//

#include <string.h>

#include "ui_interface.h"

ui_5_frame_t ui_side_DynamicGroup_0;

ui_interface_round_t *ui_side_DynamicGroup_FricRound = (ui_interface_round_t*)&(ui_side_DynamicGroup_0.data[0]);
ui_interface_round_t *ui_side_DynamicGroup_AutoRound = (ui_interface_round_t*)&(ui_side_DynamicGroup_0.data[1]);
ui_interface_round_t *ui_side_DynamicGroup_RotateRound = (ui_interface_round_t*)&(ui_side_DynamicGroup_0.data[2]);
ui_interface_number_t *ui_side_DynamicGroup_M2006SpeedNumber = (ui_interface_number_t*)&(ui_side_DynamicGroup_0.data[3]);
ui_interface_arc_t *ui_side_DynamicGroup_DirectionArc = (ui_interface_arc_t*)&(ui_side_DynamicGroup_0.data[4]);

void _ui_init_side_DynamicGroup_0() {
    for (int i = 0; i < 5; i++) {
        ui_side_DynamicGroup_0.data[i].figure_name[0] = 2;
        ui_side_DynamicGroup_0.data[i].figure_name[1] = 0;
        ui_side_DynamicGroup_0.data[i].figure_name[2] = i + 0;
        ui_side_DynamicGroup_0.data[i].operate_type = 1;
    }
    for (int i = 5; i < 5; i++) {
        ui_side_DynamicGroup_0.data[i].operate_type = 0;
    }

    ui_side_DynamicGroup_FricRound->figure_type = 2;
    ui_side_DynamicGroup_FricRound->operate_type = 1;
    ui_side_DynamicGroup_FricRound->layer = 0;
    ui_side_DynamicGroup_FricRound->color = 8;
    ui_side_DynamicGroup_FricRound->start_x = 810;
    ui_side_DynamicGroup_FricRound->start_y = 227;
    ui_side_DynamicGroup_FricRound->width = 15;
    ui_side_DynamicGroup_FricRound->r = 18;

    ui_side_DynamicGroup_AutoRound->figure_type = 2;
    ui_side_DynamicGroup_AutoRound->operate_type = 1;
    ui_side_DynamicGroup_AutoRound->layer = 0;
    ui_side_DynamicGroup_AutoRound->color = 8;
    ui_side_DynamicGroup_AutoRound->start_x = 960;
    ui_side_DynamicGroup_AutoRound->start_y = 227;
    ui_side_DynamicGroup_AutoRound->width = 15;
    ui_side_DynamicGroup_AutoRound->r = 18;

    ui_side_DynamicGroup_RotateRound->figure_type = 2;
    ui_side_DynamicGroup_RotateRound->operate_type = 1;
    ui_side_DynamicGroup_RotateRound->layer = 0;
    ui_side_DynamicGroup_RotateRound->color = 8;
    ui_side_DynamicGroup_RotateRound->start_x = 1110;
    ui_side_DynamicGroup_RotateRound->start_y = 227;
    ui_side_DynamicGroup_RotateRound->width = 15;
    ui_side_DynamicGroup_RotateRound->r = 18;

    ui_side_DynamicGroup_M2006SpeedNumber->figure_type = 6;
    ui_side_DynamicGroup_M2006SpeedNumber->operate_type = 1;
    ui_side_DynamicGroup_M2006SpeedNumber->layer = 0;
    ui_side_DynamicGroup_M2006SpeedNumber->color = 4;
    ui_side_DynamicGroup_M2006SpeedNumber->start_x = 402;
    ui_side_DynamicGroup_M2006SpeedNumber->start_y = 793;
    ui_side_DynamicGroup_M2006SpeedNumber->width = 2;
    ui_side_DynamicGroup_M2006SpeedNumber->font_size = 22;
    ui_side_DynamicGroup_M2006SpeedNumber->number = 12345;

    ui_side_DynamicGroup_DirectionArc->figure_type = 4;
    ui_side_DynamicGroup_DirectionArc->operate_type = 1;
    ui_side_DynamicGroup_DirectionArc->layer = 0;
    ui_side_DynamicGroup_DirectionArc->color = 2;
    ui_side_DynamicGroup_DirectionArc->start_x = 1526;
    ui_side_DynamicGroup_DirectionArc->start_y = 689;
    ui_side_DynamicGroup_DirectionArc->width = 10;
    ui_side_DynamicGroup_DirectionArc->start_angle = 30;
    ui_side_DynamicGroup_DirectionArc->end_angle = 330;
    ui_side_DynamicGroup_DirectionArc->rx = 50;
    ui_side_DynamicGroup_DirectionArc->ry = 50;


    ui_proc_5_frame(&ui_side_DynamicGroup_0);
    SEND_MESSAGE((uint8_t *) &ui_side_DynamicGroup_0, sizeof(ui_side_DynamicGroup_0));
}

void _ui_update_side_DynamicGroup_0() {
    for (int i = 0; i < 5; i++) {
        ui_side_DynamicGroup_0.data[i].operate_type = 2;
    }

    ui_proc_5_frame(&ui_side_DynamicGroup_0);
    SEND_MESSAGE((uint8_t *) &ui_side_DynamicGroup_0, sizeof(ui_side_DynamicGroup_0));
}

void _ui_remove_side_DynamicGroup_0() {
    for (int i = 0; i < 5; i++) {
        ui_side_DynamicGroup_0.data[i].operate_type = 3;
    }

    ui_proc_5_frame(&ui_side_DynamicGroup_0);
    SEND_MESSAGE((uint8_t *) &ui_side_DynamicGroup_0, sizeof(ui_side_DynamicGroup_0));
}


void ui_init_side_DynamicGroup() {
    _ui_init_side_DynamicGroup_0();
}

void ui_update_side_DynamicGroup() {
    _ui_update_side_DynamicGroup_0();
}

void ui_remove_side_DynamicGroup() {
    _ui_remove_side_DynamicGroup_0();
}


ui_string_frame_t ui_side_StaticTextGroup_0;
ui_interface_string_t* ui_side_StaticTextGroup_FricText = &(ui_side_StaticTextGroup_0.option);

void _ui_init_side_StaticTextGroup_0() {
    ui_side_StaticTextGroup_0.option.figure_name[0] = 2;
    ui_side_StaticTextGroup_0.option.figure_name[1] = 1;
    ui_side_StaticTextGroup_0.option.figure_name[2] = 0;
    ui_side_StaticTextGroup_0.option.operate_type = 1;

    ui_side_StaticTextGroup_FricText->figure_type = 7;
    ui_side_StaticTextGroup_FricText->operate_type = 1;
    ui_side_StaticTextGroup_FricText->layer = 0;
    ui_side_StaticTextGroup_FricText->color = 0;
    ui_side_StaticTextGroup_FricText->start_x = 810;
    ui_side_StaticTextGroup_FricText->start_y = 170;
    ui_side_StaticTextGroup_FricText->width = 2;
    ui_side_StaticTextGroup_FricText->font_size = 22;
    ui_side_StaticTextGroup_FricText->str_length = 1;
    strcpy(ui_side_StaticTextGroup_FricText->string, "F");


    ui_proc_string_frame(&ui_side_StaticTextGroup_0);
    SEND_MESSAGE((uint8_t *) &ui_side_StaticTextGroup_0, sizeof(ui_side_StaticTextGroup_0));
}

void _ui_update_side_StaticTextGroup_0() {
    ui_side_StaticTextGroup_0.option.operate_type = 2;

    ui_proc_string_frame(&ui_side_StaticTextGroup_0);
    SEND_MESSAGE((uint8_t *) &ui_side_StaticTextGroup_0, sizeof(ui_side_StaticTextGroup_0));
}

void _ui_remove_side_StaticTextGroup_0() {
    ui_side_StaticTextGroup_0.option.operate_type = 3;

    ui_proc_string_frame(&ui_side_StaticTextGroup_0);
    SEND_MESSAGE((uint8_t *) &ui_side_StaticTextGroup_0, sizeof(ui_side_StaticTextGroup_0));
}
ui_string_frame_t ui_side_StaticTextGroup_1;
ui_interface_string_t* ui_side_StaticTextGroup_AutoText = &(ui_side_StaticTextGroup_1.option);

void _ui_init_side_StaticTextGroup_1() {
    ui_side_StaticTextGroup_1.option.figure_name[0] = 2;
    ui_side_StaticTextGroup_1.option.figure_name[1] = 1;
    ui_side_StaticTextGroup_1.option.figure_name[2] = 1;
    ui_side_StaticTextGroup_1.option.operate_type = 1;

    ui_side_StaticTextGroup_AutoText->figure_type = 7;
    ui_side_StaticTextGroup_AutoText->operate_type = 1;
    ui_side_StaticTextGroup_AutoText->layer = 0;
    ui_side_StaticTextGroup_AutoText->color = 0;
    ui_side_StaticTextGroup_AutoText->start_x = 960;
    ui_side_StaticTextGroup_AutoText->start_y = 170;
    ui_side_StaticTextGroup_AutoText->width = 2;
    ui_side_StaticTextGroup_AutoText->font_size = 22;
    ui_side_StaticTextGroup_AutoText->str_length = 1;
    strcpy(ui_side_StaticTextGroup_AutoText->string, "A");


    ui_proc_string_frame(&ui_side_StaticTextGroup_1);
    SEND_MESSAGE((uint8_t *) &ui_side_StaticTextGroup_1, sizeof(ui_side_StaticTextGroup_1));
}

void _ui_update_side_StaticTextGroup_1() {
    ui_side_StaticTextGroup_1.option.operate_type = 2;

    ui_proc_string_frame(&ui_side_StaticTextGroup_1);
    SEND_MESSAGE((uint8_t *) &ui_side_StaticTextGroup_1, sizeof(ui_side_StaticTextGroup_1));
}

void _ui_remove_side_StaticTextGroup_1() {
    ui_side_StaticTextGroup_1.option.operate_type = 3;

    ui_proc_string_frame(&ui_side_StaticTextGroup_1);
    SEND_MESSAGE((uint8_t *) &ui_side_StaticTextGroup_1, sizeof(ui_side_StaticTextGroup_1));
}
ui_string_frame_t ui_side_StaticTextGroup_2;
ui_interface_string_t* ui_side_StaticTextGroup_RotateText = &(ui_side_StaticTextGroup_2.option);

void _ui_init_side_StaticTextGroup_2() {
    ui_side_StaticTextGroup_2.option.figure_name[0] = 2;
    ui_side_StaticTextGroup_2.option.figure_name[1] = 1;
    ui_side_StaticTextGroup_2.option.figure_name[2] = 2;
    ui_side_StaticTextGroup_2.option.operate_type = 1;

    ui_side_StaticTextGroup_RotateText->figure_type = 7;
    ui_side_StaticTextGroup_RotateText->operate_type = 1;
    ui_side_StaticTextGroup_RotateText->layer = 0;
    ui_side_StaticTextGroup_RotateText->color = 0;
    ui_side_StaticTextGroup_RotateText->start_x = 1110;
    ui_side_StaticTextGroup_RotateText->start_y = 170;
    ui_side_StaticTextGroup_RotateText->width = 2;
    ui_side_StaticTextGroup_RotateText->font_size = 22;
    ui_side_StaticTextGroup_RotateText->str_length = 1;
    strcpy(ui_side_StaticTextGroup_RotateText->string, "R");


    ui_proc_string_frame(&ui_side_StaticTextGroup_2);
    SEND_MESSAGE((uint8_t *) &ui_side_StaticTextGroup_2, sizeof(ui_side_StaticTextGroup_2));
}

void _ui_update_side_StaticTextGroup_2() {
    ui_side_StaticTextGroup_2.option.operate_type = 2;

    ui_proc_string_frame(&ui_side_StaticTextGroup_2);
    SEND_MESSAGE((uint8_t *) &ui_side_StaticTextGroup_2, sizeof(ui_side_StaticTextGroup_2));
}

void _ui_remove_side_StaticTextGroup_2() {
    ui_side_StaticTextGroup_2.option.operate_type = 3;

    ui_proc_string_frame(&ui_side_StaticTextGroup_2);
    SEND_MESSAGE((uint8_t *) &ui_side_StaticTextGroup_2, sizeof(ui_side_StaticTextGroup_2));
}
ui_string_frame_t ui_side_StaticTextGroup_3;
ui_interface_string_t* ui_side_StaticTextGroup_M2006Speed = &(ui_side_StaticTextGroup_3.option);

void _ui_init_side_StaticTextGroup_3() {
    ui_side_StaticTextGroup_3.option.figure_name[0] = 2;
    ui_side_StaticTextGroup_3.option.figure_name[1] = 1;
    ui_side_StaticTextGroup_3.option.figure_name[2] = 3;
    ui_side_StaticTextGroup_3.option.operate_type = 1;

    ui_side_StaticTextGroup_M2006Speed->figure_type = 7;
    ui_side_StaticTextGroup_M2006Speed->operate_type = 1;
    ui_side_StaticTextGroup_M2006Speed->layer = 0;
    ui_side_StaticTextGroup_M2006Speed->color = 5;
    ui_side_StaticTextGroup_M2006Speed->start_x = 369;
    ui_side_StaticTextGroup_M2006Speed->start_y = 845;
    ui_side_StaticTextGroup_M2006Speed->width = 2;
    ui_side_StaticTextGroup_M2006Speed->font_size = 20;
    ui_side_StaticTextGroup_M2006Speed->str_length = 10;
    strcpy(ui_side_StaticTextGroup_M2006Speed->string, "M2006Speed");


    ui_proc_string_frame(&ui_side_StaticTextGroup_3);
    SEND_MESSAGE((uint8_t *) &ui_side_StaticTextGroup_3, sizeof(ui_side_StaticTextGroup_3));
}

void _ui_update_side_StaticTextGroup_3() {
    ui_side_StaticTextGroup_3.option.operate_type = 2;

    ui_proc_string_frame(&ui_side_StaticTextGroup_3);
    SEND_MESSAGE((uint8_t *) &ui_side_StaticTextGroup_3, sizeof(ui_side_StaticTextGroup_3));
}

void _ui_remove_side_StaticTextGroup_3() {
    ui_side_StaticTextGroup_3.option.operate_type = 3;

    ui_proc_string_frame(&ui_side_StaticTextGroup_3);
    SEND_MESSAGE((uint8_t *) &ui_side_StaticTextGroup_3, sizeof(ui_side_StaticTextGroup_3));
}
ui_string_frame_t ui_side_StaticTextGroup_4;
ui_interface_string_t* ui_side_StaticTextGroup_PowerText = &(ui_side_StaticTextGroup_4.option);

void _ui_init_side_StaticTextGroup_4() {
    ui_side_StaticTextGroup_4.option.figure_name[0] = 2;
    ui_side_StaticTextGroup_4.option.figure_name[1] = 1;
    ui_side_StaticTextGroup_4.option.figure_name[2] = 4;
    ui_side_StaticTextGroup_4.option.operate_type = 1;

    ui_side_StaticTextGroup_PowerText->figure_type = 7;
    ui_side_StaticTextGroup_PowerText->operate_type = 1;
    ui_side_StaticTextGroup_PowerText->layer = 0;
    ui_side_StaticTextGroup_PowerText->color = 2;
    ui_side_StaticTextGroup_PowerText->start_x = 1439;
    ui_side_StaticTextGroup_PowerText->start_y = 811;
    ui_side_StaticTextGroup_PowerText->width = 2;
    ui_side_StaticTextGroup_PowerText->font_size = 20;
    ui_side_StaticTextGroup_PowerText->str_length = 9;
    strcpy(ui_side_StaticTextGroup_PowerText->string, "Direction");


    ui_proc_string_frame(&ui_side_StaticTextGroup_4);
    SEND_MESSAGE((uint8_t *) &ui_side_StaticTextGroup_4, sizeof(ui_side_StaticTextGroup_4));
}

void _ui_update_side_StaticTextGroup_4() {
    ui_side_StaticTextGroup_4.option.operate_type = 2;

    ui_proc_string_frame(&ui_side_StaticTextGroup_4);
    SEND_MESSAGE((uint8_t *) &ui_side_StaticTextGroup_4, sizeof(ui_side_StaticTextGroup_4));
}

void _ui_remove_side_StaticTextGroup_4() {
    ui_side_StaticTextGroup_4.option.operate_type = 3;

    ui_proc_string_frame(&ui_side_StaticTextGroup_4);
    SEND_MESSAGE((uint8_t *) &ui_side_StaticTextGroup_4, sizeof(ui_side_StaticTextGroup_4));
}

void ui_init_side_StaticTextGroup() {
    _ui_init_side_StaticTextGroup_0();
    _ui_init_side_StaticTextGroup_1();
    _ui_init_side_StaticTextGroup_2();
    _ui_init_side_StaticTextGroup_3();
    _ui_init_side_StaticTextGroup_4();
}

void ui_update_side_StaticTextGroup() {
    _ui_update_side_StaticTextGroup_0();
    _ui_update_side_StaticTextGroup_1();
    _ui_update_side_StaticTextGroup_2();
    _ui_update_side_StaticTextGroup_3();
    _ui_update_side_StaticTextGroup_4();
}

void ui_remove_side_StaticTextGroup() {
    _ui_remove_side_StaticTextGroup_0();
    _ui_remove_side_StaticTextGroup_1();
    _ui_remove_side_StaticTextGroup_2();
    _ui_remove_side_StaticTextGroup_3();
    _ui_remove_side_StaticTextGroup_4();
}

