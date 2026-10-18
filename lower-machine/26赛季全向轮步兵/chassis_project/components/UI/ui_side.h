//
// Created by RM UI Designer
// Static Edition
//

#ifndef UI_side_H
#define UI_side_H

#include "ui_interface.h"

extern ui_interface_round_t *ui_side_DynamicGroup_FricRound;
extern ui_interface_round_t *ui_side_DynamicGroup_AutoRound;
extern ui_interface_round_t *ui_side_DynamicGroup_RotateRound;
extern ui_interface_number_t *ui_side_DynamicGroup_M2006SpeedNumber;
extern ui_interface_arc_t *ui_side_DynamicGroup_DirectionArc;

void ui_init_side_DynamicGroup();
void ui_update_side_DynamicGroup();
void ui_remove_side_DynamicGroup();

extern ui_interface_string_t *ui_side_StaticTextGroup_FricText;
extern ui_interface_string_t *ui_side_StaticTextGroup_AutoText;
extern ui_interface_string_t *ui_side_StaticTextGroup_RotateText;
extern ui_interface_string_t *ui_side_StaticTextGroup_M2006Speed;
extern ui_interface_string_t *ui_side_StaticTextGroup_PowerText;

void ui_init_side_StaticTextGroup();
void ui_update_side_StaticTextGroup();
void ui_remove_side_StaticTextGroup();


#endif // UI_side_H
