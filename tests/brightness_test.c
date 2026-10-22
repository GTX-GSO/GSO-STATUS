#include "gso_brightness.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>
int main(void){
 gso_brightness b;gso_brightness_init(&b,true,80);gso_brightness_step(&b,gso_metric_number(31),true,0);assert(b.target>45&&b.target<46.5&&b.applied==80);
 for(unsigned i=1;i<=30;i++){double previous=b.applied;gso_brightness_step(&b,gso_metric_number(31),true,i*100);assert(fabs(b.applied-previous)<=0.801);}assert(b.applied>55&&b.applied<57);
 for(unsigned i=31;i<=100;i++)gso_brightness_step(&b,gso_metric_number(31),true,i*100);assert(gso_brightness_percent(&b)==46);
 double stable=b.target;for(unsigned i=101;i<=130;i++)gso_brightness_step(&b,gso_metric_number(i%2?30:32),true,i*100);assert(b.target==stable);
 gso_brightness_step(&b,gso_metric_number(0),false,13100);assert(!b.healthy&&b.target==80&&b.applied<48);
 for(unsigned i=132;i<=220;i++)gso_brightness_step(&b,gso_metric_missing("offline"),false,i*100);assert(gso_brightness_percent(&b)==80);
 gso_brightness_step(&b,gso_metric_number(0),true,22100);assert(b.healthy&&b.target==15);gso_brightness_step(&b,gso_metric_number(NAN),true,22200);assert(!b.healthy&&b.target==80);
 gso_brightness_step(&b,gso_metric_number(200001),true,22300);assert(!b.healthy);
 gso_brightness_manual(&b,42);assert(!b.automatic&&gso_brightness_percent(&b)==42);gso_brightness_step(&b,gso_metric_number(1000),true,22400);assert(gso_brightness_percent(&b)==42);
 gso_brightness_mode(&b,true);gso_brightness_step(&b,gso_metric_number(1000),true,22500);assert(b.target==100&&b.applied<43);
 double before=b.applied;gso_brightness_step(&b,gso_metric_number(1000),true,999999);assert(b.applied-before<=8.001);before=b.applied;gso_brightness_step(&b,gso_metric_number(0),true,1);assert(b.applied==before);
 puts("PASS: lux curve, zero, smoothing, hysteresis, slew, offline fallback, manual and clock bounds");return 0;
}
