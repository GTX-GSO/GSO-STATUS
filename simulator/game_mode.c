#include "ui.h"
#include <string.h>

/* GSW supplies verified game rendering samples; the display controls view lifecycle. */
static bool view_open,suppressed,manual_open;
static unsigned int consecutive,offline_samples;
static uint32_t last_rendering_ms;
static char process[641];

bool desk_ui_game_mode_active(void){return view_open;}
void desk_ui_game_view_closed(void){if(view_open)suppressed=true;view_open=false;manual_open=false;}
void desk_ui_game_sample(const char *name,bool rendering){
    uint32_t now=lv_tick_get();
    if(rendering&&name&&name[0]&&strlen(name)<sizeof(process)){
        if(strcmp(process,name)!=0){strcpy(process,name);consecutive=0;suppressed=false;}
        last_rendering_ms=now;if(consecutive<3)consecutive++;
        if(consecutive>=3&&!view_open&&!suppressed&&!desk_ui_detail_is_open()){
            desk_ui_detail(16);view_open=true;
        }
    }else{
        consecutive=0;
        if((uint32_t)(now-last_rendering_ms)>=5000){
            if(view_open&&!manual_open){desk_ui_close_detail();view_open=false;}
            suppressed=false;process[0]=0;
        }
    }
}

void desk_ui_game_manual_open(void){desk_ui_detail(16);view_open=true;manual_open=true;offline_samples=0;}

/* Three consecutive offline/error polls also dismiss a manually opened game view. */
void desk_ui_game_source(bool offline){
    if(!offline){offline_samples=0;return;}
    if(offline_samples<3)offline_samples++;
    if(offline_samples>=3){if(view_open){desk_ui_close_detail();view_open=false;}manual_open=false;consecutive=0;suppressed=false;process[0]=0;}
}
