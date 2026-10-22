/* Dragging the card strip would redraw every visible card on every frame. While a finger drags it, and while it snaps to
   the next page, the visible cards are replaced by PSRAM snapshots (the same mechanism the boot transition uses), so a frame
   is only a blit. Cards that do not get a snapshot (no memory, or outside the drag direction) simply stay live. */
#define STRIP_CARDS 7
static lv_obj_t *strip_images[STRIP_CARDS];
static bool strip_hidden[STRIP_CARDS],strip_snapping,strip_interactive;
static int strip_pending_sides[2],strip_pending_count; /* neighbours still to be frozen, see strip_freeze_later */
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
 strip_snapping=false;
 strip_interactive=false;
 if(!strip_frozen)return;
 strip_frozen=false;
 strip_pending_count=0;
 for(int m=0;m<STRIP_CARDS;m++){
  if(strip_images[m]){lv_obj_delete(strip_images[m]);strip_images[m]=NULL;}
  if(strip_hidden[m]){strip_hidden[m]=false;if(main_cards[m])lv_obj_remove_flag(main_cards[m],LV_OBJ_FLAG_HIDDEN);}
 }
 telemetry_pending=true; /* re-run the card layout: PRINT or CAR may have come or gone meanwhile */
}
/* A tap that stopped the snap animation never sends SCROLL_END, so the poll timer also thaws an idle strip. */
static void strip_thaw_if_idle(void){
 if(strip_frozen&&strip_interactive&&!strip_dragging()&&!lv_anim_get(viewport,NULL))strip_thaw();
}
/* direction: <0 content moves left (finger drags left, next cards appear on the right), >0 right, 0 unknown */
static uint32_t strip_freeze_time;
uint32_t desk_ui_strip_freeze_ms(void){return strip_freeze_time;}
/* Snapshot of one card of the strip in place of the card itself. */
static void strip_freeze_card(int m){
 lv_obj_t *card=main_cards[m];
 if(!card||strip_images[m]||lv_obj_has_flag(card,LV_OBJ_FLAG_HIDDEN))return;
 lv_obj_t *image=transition_snapshot_as(card,true);
 if(!image)return;
 strip_images[m]=image;strip_hidden[m]=true;
 lv_obj_add_flag(card,LV_OBJ_FLAG_HIDDEN);
}
/* side <0: the card just left of the screen at freeze time, >0: just right of it, 0: the cards on screen. */
static int strip_freeze_scroll;
static void strip_freeze_side(int side){
 for(int m=0;m<STRIP_CARDS;m++){
  lv_obj_t *card=main_cards[m];
  if(!card)continue;
  int left=lv_obj_get_x(card),scroll=strip_freeze_scroll;
  bool visible=left+334>scroll&&left<scroll+1032;
  bool wanted=side==0?visible:side<0?left+334<=scroll&&left+334>scroll-350:left>=scroll+1032&&left<scroll+1032+350;
  if(wanted)strip_freeze_card(m);
 }
}
/* The neighbours are taken on later timer ticks, one per tick, so the first frames of a drag are not held up by them: first
   the one the finger is pulling into view, then the other, since a finger can reverse before release. Until then they are
   drawn live. */
static void strip_freeze_later(void *unused){
 (void)unused;
 if(!strip_frozen||!strip_pending_count){strip_pending_count=0;return;}
 int side=strip_pending_sides[0];
 strip_pending_sides[0]=strip_pending_sides[1];strip_pending_count--;
 strip_freeze_side(side);
 if(strip_pending_count)lv_async_call(strip_freeze_later,NULL);
}
static void strip_freeze_now(int direction){
 if(strip_frozen||overlay||!viewport)return;
 uint32_t started=lv_tick_get();
 strip_freeze_scroll=lv_obj_get_scroll_x(viewport);
 strip_frozen=true;
 strip_interactive=strip_dragging();
 strip_freeze_side(0); /* scarce cache space goes to what is on screen first */
 strip_freeze_time=lv_tick_elaps(started);
 /* direction <0: content moves left, so the card on the right comes into view */
 int first=direction<0?1:-1;
 strip_pending_sides[0]=first;strip_pending_sides[1]=-first;
 if(!strip_pending_count)lv_async_call(strip_freeze_later,NULL);
 strip_pending_count=2;
}
static void strip_scroll_begin(lv_event_t *event){
 (void)event;
 if(!strip_dragging())return; /* programmatic page jumps are instant and need no snapshot */
 lv_point_t vector={0,0};
 for(lv_indev_t *input=lv_indev_get_next(NULL);input;input=lv_indev_get_next(input))if(lv_indev_get_scroll_obj(input)==viewport)lv_indev_get_vect(input,&vector);
 strip_freeze_now(vector.x<0?-1:vector.x>0?1:0);
}
/* Our bounded snap is an object animation, so LVGL's pointer throw cancellation
   alone does not stop it. Consume this tap: restoring the real cards must not
   turn a tap intended to stop motion into a HOME/CAR action or detail click. */
static void strip_pointer_pressed(lv_event_t *event){
 if(!strip_frozen||!strip_snapping)return;
 lv_indev_t *input=lv_event_get_target(event);
 lv_point_t point;lv_indev_get_point(input,&point);
 lv_area_t area;lv_obj_get_coords(viewport,&area);
 if(point.x<area.x1||point.x>area.x2||point.y<area.y1||point.y>area.y2)return;
 lv_obj_stop_scroll_anim(viewport);
 strip_thaw();
 page=(lv_obj_get_scroll_x(viewport)+175)/350;
 lv_indev_wait_release(input);
}
/* The release sends one SCROLL_END as LVGL starts its own snap animation and another when an animation finishes. LVGL's
   throw prediction ignores the momentum flag and its snap target ignores the content range, so the first one is replaced
   by a snap to the nearest page inside the strip; only the end of that animation thaws the cards. */
static void strip_scroll_end(void){
 if(lv_anim_get(viewport,NULL)){
  if(strip_snapping)return;
  int target=(lv_obj_get_scroll_x(viewport)+175)/350;
  if(target<0)target=0;
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
