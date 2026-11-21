#pragma once

#include "main_window.h"

void ShowHeaderPanel(const State &state, bool local_trial);

std::string CupPhaseText(const GameStatus *game);
std::string CupTimeText(const GameStatus *game);

bool ShowAllianceHeader(const State &state, bool local_trial, bool blue, bool heavy);
