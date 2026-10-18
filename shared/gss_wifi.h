#pragma once
#include <stdbool.h>
/* Credentials remain on-device. No USB command or log exports them. */
void gss_wifi_start(void);
const char *gss_wifi_ssid(void);
bool gss_wifi_has_password(void);
bool gss_wifi_is_open(void);
/* NULL on successful save; otherwise a user-facing validation/storage error. */
const char *gss_wifi_save(const char *ssid,const char *password,bool open_network);
bool gss_wifi_forget(void);
bool gss_wifi_copy(char ssid[33],char password[65],bool *open_network);
#define GSS_WIFI_AP_LIMIT 12
typedef struct {char ssid[33];int rssi;bool open;} gss_wifi_ap;
typedef struct {bool ready,scanning,connected;int reason,rssi;char message[96],ip[16];unsigned count,generation;gss_wifi_ap aps[GSS_WIFI_AP_LIMIT];} gss_wifi_status;
void gss_wifi_radio_start(void);
void gss_wifi_radio_stop(void);
const char *gss_wifi_transport_note(void);
#ifdef GSO_TEST
void gss_wifi_test_mode(bool enable);
#endif
void gss_wifi_radio_status(gss_wifi_status *out);
bool gss_wifi_scan(void);
bool gss_wifi_connect_saved(void);
bool gss_wifi_disconnect(void);
