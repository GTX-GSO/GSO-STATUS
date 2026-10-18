#include "gso_level.h"
#include <math.h>
#define SMOOTHING 0.25f
#define STILL_MIN 0.90f
#define STILL_MAX 1.10f
#define FLAT_IN_PLANE 0.30f
#define RAD_TO_DEG 57.29577951f
void gso_level_reset(gso_level *l){
 l->state=GSO_LEVEL_NO_SENSOR;
 l->seeded=false;
 l->filtered=(gso_accel){0,0,0};
 l->roll_deg=0;
 l->tilt_deg=0;
}
void gso_level_no_sensor(gso_level *l){
 gso_level_reset(l);
}
void gso_level_update(gso_level *l,gso_accel s){
 float magnitude=sqrtf(s.x*s.x+s.y*s.y+s.z*s.z);
 if(!(magnitude>=STILL_MIN&&magnitude<=STILL_MAX)){
  l->state=GSO_LEVEL_MOVING;
  return;
 }
 if(!l->seeded){
  l->filtered=s;
  l->seeded=true;
 }
 else{
  l->filtered.x+=(s.x-l->filtered.x)*SMOOTHING;
  l->filtered.y+=(s.y-l->filtered.y)*SMOOTHING;
  l->filtered.z+=(s.z-l->filtered.z)*SMOOTHING;
 }
 gso_accel f=l->filtered;
 float in_plane=sqrtf(f.x*f.x+f.y*f.y);
 float whole=sqrtf(in_plane*in_plane+f.z*f.z);
 l->tilt_deg=whole>0?asinf(f.z/whole>1?1:f.z/whole<-1?-1:f.z/whole)*RAD_TO_DEG:0;
 if(in_plane<FLAT_IN_PLANE){
  l->state=GSO_LEVEL_FLAT;
  return;
 }
 l->roll_deg=atan2f(-f.x,f.y)*RAD_TO_DEG;
 l->state=GSO_LEVEL_OK;
}
gso_level_band gso_level_band_of(float roll_deg){
 float a=roll_deg<0?-roll_deg:roll_deg;
 if(a<0.5f)return GSO_LEVEL_FLATNESS_LEVEL;
 if(a<2.0f)return GSO_LEVEL_FLATNESS_NEAR;
 return GSO_LEVEL_FLATNESS_OFF;
}
