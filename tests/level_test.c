#include "gso_level.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#define CHECK(c) do{if(!(c)){printf("FAIL line %d: %s\n",__LINE__,#c);exit(1);}}while(0)
#define NEAR(a,b,t) (fabsf((a)-(b))<(t))
/* The reading of a display standing upright and turned clockwise by deg (seen from the front), tilted back by tilt. */
static gso_accel reading(float roll_deg,float tilt_deg){
 float r=roll_deg*3.14159265f/180,t=tilt_deg*3.14159265f/180;
 return (gso_accel){-sinf(r)*cosf(t),cosf(r)*cosf(t),sinf(t)};
}
static void settle(gso_level *l,gso_accel s){
 for(int i=0;i<60;i++)gso_level_update(l,s);
}
int main(void){
 gso_level l;gso_level_reset(&l);
 CHECK(l.state==GSO_LEVEL_NO_SENSOR);
 settle(&l,reading(0,0));
 CHECK(l.state==GSO_LEVEL_OK&&NEAR(l.roll_deg,0,0.01f)&&NEAR(l.tilt_deg,0,0.01f));
 CHECK(gso_level_band_of(l.roll_deg)==GSO_LEVEL_FLATNESS_LEVEL);
 /* clockwise turn: the right side is lower, positive roll */
 settle(&l,reading(1.5f,0));
 CHECK(NEAR(l.roll_deg,1.5f,0.02f));
 CHECK(gso_level_band_of(l.roll_deg)==GSO_LEVEL_FLATNESS_NEAR);
 settle(&l,reading(-4,0));
 CHECK(NEAR(l.roll_deg,-4,0.02f)&&gso_level_band_of(l.roll_deg)==GSO_LEVEL_FLATNESS_OFF);
 /* leaning back does not change the left/right angle */
 settle(&l,reading(2,8));
 CHECK(NEAR(l.roll_deg,2,0.05f)&&NEAR(l.tilt_deg,8,0.05f));
 settle(&l,reading(0,-6));
 CHECK(NEAR(l.tilt_deg,-6,0.05f));
 /* flat on a desk: no left/right angle, the last one is kept but the state says so */
 gso_level_reset(&l);
 settle(&l,(gso_accel){0.01f,0.02f,0.999f});
 CHECK(l.state==GSO_LEVEL_FLAT&&l.tilt_deg>85);
 settle(&l,(gso_accel){0.01f,0.02f,-0.999f});
 CHECK(l.state==GSO_LEVEL_FLAT&&l.tilt_deg<-85);
 /* shaking or moving: the reading is not 1 g, the angles hold */
 gso_level_reset(&l);
 settle(&l,reading(1,0));
 float held=l.roll_deg;
 gso_level_update(&l,(gso_accel){0.2f,1.9f,0.1f});
 CHECK(l.state==GSO_LEVEL_MOVING&&l.roll_deg==held);
 gso_level_update(&l,(gso_accel){0,0.1f,0});
 CHECK(l.state==GSO_LEVEL_MOVING&&l.roll_deg==held);
 settle(&l,reading(1,0));
 CHECK(l.state==GSO_LEVEL_OK);
 /* a first reading that is not at rest is not shown as an angle */
 gso_level_reset(&l);
 gso_level_update(&l,(gso_accel){0,3,0});
 CHECK(l.state==GSO_LEVEL_MOVING&&!l.seeded);
 /* the smoothing follows a step within about a second at 20 Hz without overshooting */
 gso_level_reset(&l);
 settle(&l,reading(0,0));
 for(int i=0;i<20;i++){
  gso_level_update(&l,reading(3,0));
  CHECK(l.roll_deg<=3.001f);
 }
 CHECK(l.roll_deg>2.9f);
 gso_level_no_sensor(&l);
 CHECK(l.state==GSO_LEVEL_NO_SENSOR&&!l.seeded);
 puts("level tests passed");
 return 0;
}
