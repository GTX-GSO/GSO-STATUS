#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "gso_level.h"
/* What the board itself can tell about its own state, and its accelerometer. The firmware reads the chip (main/gss_system.c);
   the Windows simulator shows fixed sample values (simulator/system_preview.c) so the page can be previewed. */
typedef struct {
 bool cpu_valid;
 unsigned cpu_percent[2];     /* busy share of each core since the previous call */
 bool memory_valid;
 uint32_t internal_total,internal_free,psram_total,psram_free; /* bytes */
 bool storage_valid;
 uint32_t flash_total,app_total,app_used;                      /* bytes: flash chip, firmware partition, firmware image */
 bool temperature_valid;
 float temperature_c;
 uint32_t uptime_s;
} gss_system_info;
void gss_system_read(gss_system_info *info);
/* Accelerometer, powered only while the level page is open. gss_imu_read returns false when there is no sensor or no
   sample; the sample is in the screen frame of gso_level.h and already follows the flipped (rotated 180°) display. */
bool gss_imu_open(void);
void gss_imu_close(void);
bool gss_imu_read(gso_accel *sample);
