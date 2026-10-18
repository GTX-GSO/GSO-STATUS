#pragma once
#include "gso_brightness.h"
void desk_brightness_start(const char *config_path);
gso_brightness *desk_brightness_state(void);
bool desk_brightness_save(void);
