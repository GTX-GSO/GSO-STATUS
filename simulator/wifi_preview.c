/* UI-only platform adapter. Never accesses WLAN, USB or operating-system profiles. */
#include "gss_wifi.h"
#include "gss_wifi_storage.h"
#include <string.h>
#include <stdio.h>
static gss_wifi_status state;
static unsigned char saved[256];static size_t saved_size;
bool gss_wifi_storage_read(void *data,size_t *size){if(!saved_size||*size<saved_size)return false;memcpy(data,saved,saved_size);*size=saved_size;return true;}
bool gss_wifi_storage_write(const void *data,size_t size){if(size>sizeof(saved))return false;memcpy(saved,data,size);saved_size=size;return true;}
bool gss_wifi_storage_forget(void){memset(saved,0,sizeof(saved));saved_size=0;return true;}
void gss_wifi_radio_start(void){memset(&state,0,sizeof(state));state.ready=true;state.message[0]=0;}
void gss_wifi_radio_stop(void){memset(saved,0,sizeof(saved));saved_size=0;}
const char *gss_wifi_transport_note(void){return "Wi-Fi 仅作界面预览 · 业务数据通过 Windows 网络直连 GSN";}
void gss_wifi_radio_status(gss_wifi_status *out){*out=state;}
bool gss_wifi_scan(void){state.scanning=false;state.count=2;strcpy(state.aps[0].ssid,"GSS Preview");state.aps[0].rssi=-45;state.aps[0].open=false;strcpy(state.aps[1].ssid,"Open Preview");state.aps[1].rssi=-65;state.aps[1].open=true;state.generation++;state.message[0]=0;return true;}
bool gss_wifi_connect_saved(void){if(!gss_wifi_ssid()[0])return false;state.connected=true;strcpy(state.ip,"192.0.2.2");state.message[0]=0;return true;}
bool gss_wifi_disconnect(void){state.connected=false;state.ip[0]=0;state.message[0]=0;return true;}
#ifdef GSO_TEST
void gss_wifi_test_mode(bool enabled){(void)enabled;memset(saved,0,sizeof(saved));saved_size=0;}
#endif
