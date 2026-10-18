/* Dragging the card strip would redraw every visible card on every frame. While a finger drags it, and while it snaps to
   the next page, the visible cards are replaced by PSRAM snapshots (the same mechanism the boot transition uses), so a frame
   is only a blit. Cards that do not get a snapshot (no memory, or outside the drag direction) simply stay live. */
#define STRIP_CARDS 7
static lv_obj_t *strip_images[STRIP_CARDS];
static bool strip_hidden[STRIP_CARDS],strip_frozen;
static bool strip_dragging(void){
 for(lv_indev_t *input=lv_indev_get_next(NULL);input;input=lv_indev_get_next(input))if(lv_indev_get_scroll_obj(input)==viewport)return true;
 return false;
}
unsigned desk_ui_strip_frozen_cards(void){
 unsigned count=0;
 for(int m=0;m<STRIP_CARDS;m++)if(strip_images[m])count++;
 return count;
}
static void strip_thaw(void){
 if(!strip_frozen)return;
 strip_frozen=false;
 for(int m=0;m<STRIP_CARDS;m++){
  if(strip_images[m]){lv_obj_delete(strip_images[m]);strip_images[m]=NULL;}
  if(strip_hidden[m]){strip_hidden[m]=false;if(main_cards[m])lv_obj_remove_flag(main_cards[m],LV_OBJ_FLAG_HIDDEN);}
 }
 telemetry_pending=true; /* re-run the card layout: PRINT or CAR may have come or gone meanwhile */
}
/* A tap that stopped the snap animation never sends SCROLL_END, so the poll timer also thaws an idle strip. */
static void strip_thaw_if_idle(void){
 if(strip_frozen&&!strip_dragging()&&!lv_anim_get(viewport,NULL))strip_thaw();
}
/* direction: <0 content moves left (finger drags left, next cards appear on the right), >0 right, 0 unknown */
static void strip_freeze_now(int direction){
 if(strip_frozen||overlay||!viewport)return;
 int scroll=lv_obj_get_scroll_x(viewport);
 int low=scroll-(direction>=0?350:0),high=scroll+1032+(direction<=0?350:0);
 strip_frozen=true;
 for(int m=0;m<STRIP_CARDS;m++){
  lv_obj_t *card=main_cards[m];
  if(!card||lv_obj_has_flag(card,LV_OBJ_FLAG_HIDDEN))continue;
  int left=lv_obj_get_x(card);
  if(left+334<=low||left>=high)continue;
  lv_obj_t *image=transition_snapshot(card);
  if(!image)continue;
  strip_images[m]=image;strip_hidden[m]=true;
  lv_obj_add_flag(card,LV_OBJ_FLAG_HIDDEN);
 }
}
static void strip_scroll_begin(lv_event_t *event){
 (void)event;
 if(!strip_dragging())return; /* programmatic page jumps are instant and need no snapshot */
 lv_point_t vector={0,0};
 for(lv_indev_t *input=lv_indev_get_next(NULL);input;input=lv_indev_get_next(input))if(lv_indev_get_scroll_obj(input)==viewport)lv_indev_get_vect(input,&vector);
 strip_freeze_now(vector.x<0?-1:vector.x>0?1:0);
}
/* The release sends one SCROLL_END as LVGL starts its own snap animation and another when an animation finishes. LVGL's
   throw prediction ignores the momentum flag and its snap target ignores the content range, so the first one is replaced
   by a snap to the nearest page inside the strip; only the end of that animation thaws the cards. */
static bool strip_snapping;
static void strip_scroll_end(void){
 if(lv_anim_get(viewport,NULL)){
  if(strip_snapping)return;
  int target=(lv_obj_get_scroll_x(viewport)+175)/350;
  if(target>page_limit())target=page_limit();
  lv_anim_delete(viewport,NULL);
  strip_snapping=true;
  lv_obj_scroll_to_x(viewport,target*350,LV_ANIM_ON);
  return;
 }
 strip_snapping=false;
 strip_thaw();
}
void desk_ui_test_strip_freeze(int direction){strip_freeze_now(direction);}
void desk_ui_test_strip_thaw(void){strip_thaw();}
int desk_ui_strip_scroll_x(void){return viewport?lv_obj_get_scroll_x(viewport):0;}
