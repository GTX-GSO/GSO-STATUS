#include "display_brightness.h"
#include "nvs.h"
static gso_brightness state;
gso_brightness *desk_brightness_state(void){return &state;}
void desk_brightness_start(const char *unused){(void)unused;uint8_t automatic=1,manual=80;nvs_handle_t h;if(nvs_open("gso_display",NVS_READONLY,&h)==ESP_OK){nvs_get_u8(h,"automatic",&automatic);nvs_get_u8(h,"manual",&manual);nvs_close(h);}gso_brightness_init(&state,automatic!=0,manual);}
bool desk_brightness_save(void){nvs_handle_t h;if(nvs_open("gso_display",NVS_READWRITE,&h)!=ESP_OK)return false;esp_err_t e=nvs_set_u8(h,"automatic",state.automatic);if(e==ESP_OK)e=nvs_set_u8(h,"manual",state.manual);if(e==ESP_OK)e=nvs_commit(h);nvs_close(h);return e==ESP_OK;}
