//
// Created by RM UI Designer
// Static Edition
//

#ifndef UI_middle_H
#define UI_middle_H

#include "ui_interface.h"

extern ui_interface_rect_t *ui_middle_StaticGraphicGroup_SelfaimRect;
extern ui_interface_line_t *ui_middle_StaticGraphicGroup_CrosshairLine;
extern ui_interface_line_t *ui_middle_StaticGraphicGroup_GuideLine2;
extern ui_interface_line_t *ui_middle_StaticGraphicGroup_GuideLine1;
extern ui_interface_line_t *ui_middle_StaticGraphicGroup_CrosshairLine2;
extern ui_interface_line_t *ui_middle_StaticGraphicGroup_CrosshairLine3;
extern ui_interface_rect_t *ui_middle_StaticGraphicGroup_HeatRect;

void ui_init_middle_StaticGraphicGroup();
void ui_update_middle_StaticGraphicGroup();
void ui_remove_middle_StaticGraphicGroup();


#endif // UI_middle_H
