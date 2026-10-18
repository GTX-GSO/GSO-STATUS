#pragma once
#include "gso_model.h"
/* Hardware-independent controller. Output is a requested percentage, not proof of backlight actuation. */
typedef struct {
 bool automatic,healthy,has_sample,started;
 unsigned manual;
 double applied,target,filtered_lux;
 uint64_t last_ms;
} gso_brightness;
void gso_brightness_init(gso_brightness *b,bool automatic,unsigned manual);
void gso_brightness_mode(gso_brightness *b,bool automatic);
void gso_brightness_manual(gso_brightness *b,unsigned percent);
void gso_brightness_step(gso_brightness *b,gso_metric lux,bool source_ok,uint64_t now_ms);
unsigned gso_brightness_percent(const gso_brightness *b);
