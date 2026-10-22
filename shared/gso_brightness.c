#include "gso_brightness.h"
#include <math.h>
#include <string.h>
static unsigned clamp(unsigned n){return n<10?10:n>100?100:n;}
static double curve(double lux){
 /* This panel is not bright, so full backlight is reached at 100 lx, the study's light level (user choice 2026-10-20);
    the old curve stayed below 100% until 1000 lx. */
 static const double x[]={0,10,50,100},y[]={15,30,60,100};
 for(unsigned i=1;i<4;i++)if(lux<=x[i])return y[i-1]+(y[i]-y[i-1])*(lux-x[i-1])/(x[i]-x[i-1]);
 return 100;
}
void gso_brightness_init(gso_brightness *b,bool automatic,unsigned manual){memset(b,0,sizeof(*b));b->automatic=automatic;b->manual=clamp(manual);b->applied=b->target=b->manual;}
void gso_brightness_mode(gso_brightness *b,bool automatic){b->automatic=automatic;b->has_sample=false;if(!automatic)b->target=b->manual;}
void gso_brightness_manual(gso_brightness *b,unsigned percent){b->manual=clamp(percent);gso_brightness_mode(b,false);b->applied=b->target=b->manual;}
void gso_brightness_step(gso_brightness *b,gso_metric lux,bool source_ok,uint64_t now){
 double dt=b->started&&now>=b->last_ms?(double)(now-b->last_ms)/1000.0:0;if(dt>1)dt=1;
 b->last_ms=now;b->started=true;
 b->healthy=source_ok&&lux.valid&&isfinite(lux.value)&&lux.value>=0&&lux.value<=200000;
 if(b->automatic&&b->healthy){
  if(!b->has_sample){b->filtered_lux=lux.value;b->has_sample=true;b->target=curve(lux.value);}
  else{b->filtered_lux+=(lux.value-b->filtered_lux)*dt/(5+dt);double wanted=curve(b->filtered_lux);if(fabs(wanted-b->target)>=3)b->target=wanted;}
 }else{b->has_sample=false;b->target=b->manual;}
 double delta=b->target-b->applied,limit=8*dt;
 b->applied+=delta>limit?limit:delta< -limit?-limit:delta;
}
unsigned gso_brightness_percent(const gso_brightness *b){return (unsigned)(b->applied+0.5);}
