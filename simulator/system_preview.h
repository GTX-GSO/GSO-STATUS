#pragma once
#include <stdbool.h>
#include "gss_system.h"
/* Test hooks for the simulator's sample board state. */
void gss_preview_imu(bool present,gso_accel sample); /* fixed accelerometer reading, or no sensor */
void gss_preview_imu_sway(void);                     /* back to the slow demo movement */
void gss_preview_system(const gss_system_info *info); /* fixed board values; NULL for the built-in samples */
