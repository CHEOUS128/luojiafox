//
// Created by RM UI Designer
// Static Edition
//

#ifndef UI_H
#define UI_H
#ifdef __cplusplus
extern "C" {
#endif

#include "ui_interface.h"

#include "ui_default.h"

#include "ui_middle.h"









// 图形配置参数：图形颜色
#define UI_Color_Main 0         // 红蓝主色
#define UI_Color_Yellow 1
#define UI_Color_Green 2
#define UI_Color_Orange 3
#define UI_Color_Purplish_red 4 // 紫红色
#define UI_Color_Pink 5
#define UI_Color_Cyan 6         // 青色
#define UI_Color_Black 7
#define UI_Color_White 8






void ui_init_middle_StaticGraphicGroup();
void ui_update_middle_StaticGraphicGroup();
void ui_remove_middle_StaticGraphicGroup();
#include "ui_side.h"

void ui_init_side_DynamicGroup();
void ui_update_side_DynamicGroup();
void ui_remove_side_DynamicGroup();
void ui_init_side_StaticTextGroup();
void ui_update_side_StaticTextGroup();
void ui_remove_side_StaticTextGroup();

#ifdef __cplusplus
}
#endif

#endif // UI_H
