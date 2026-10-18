#include "gss_wifi.h"
#include "gss_board.h"
#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "driver/gpio.h"
#include "esp_hosted.h"
#include "esp_wifi_remote.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "ping/ping_sock.h"
enum {SCAN=1,SCAN_DONE,CONNECT,DISCONNECT,RETRY,CHECK_NET};
typedef struct {int kind;} command;
static QueueHandle_t commands;
static portMUX_TYPE lock=portMUX_INITIALIZER_UNLOCKED;
static gss_wifi_status state;
static bool desired;
static bool automatic;
static unsigned retries;
static int64_t connect_deadline;
static int64_t retry_at;
static void ping_end(esp_ping_handle_t ping,void *arg){
 (void)arg;uint32_t sent=0,received=0;
 esp_ping_get_profile(ping,ESP_PING_PROF_REQUEST,&sent,sizeof(sent));esp_ping_get_profile(ping,ESP_PING_PROF_REPLY,&received,sizeof(received));
 ESP_LOGI("gso_wifi","GATEWAY_CHECK sent=%lu received=%lu",(unsigned long)sent,(unsigned long)received);esp_ping_delete_session(ping);
}
static void check_network(void){
 wifi_ap_record_t ap={0};if(esp_wifi_remote_sta_get_ap_info(&ap)==ESP_OK){
  portENTER_CRITICAL(&lock);state.rssi=ap.rssi;portEXIT_CRITICAL(&lock);
  ESP_LOGI("gso_wifi","LINK rssi=%d channel=%u auth=%u",ap.rssi,ap.primary,(unsigned)ap.authmode);
 }
 esp_netif_ip_info_t ip;esp_netif_t *net=esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
 if(!net||esp_netif_get_ip_info(net,&ip)!=ESP_OK||!ip.gw.addr)return;
 esp_ping_config_t config=ESP_PING_DEFAULT_CONFIG();config.count=3;config.interval_ms=500;config.timeout_ms=1000;
 ip_addr_set_ip4_u32(&config.target_addr,ip.gw.addr);
 esp_ping_callbacks_t callbacks={.on_ping_end=ping_end};esp_ping_handle_t ping;
 if(esp_ping_new_session(&config,&callbacks,&ping)==ESP_OK&&esp_ping_start(ping)!=ESP_OK)esp_ping_delete_session(ping);
}
static void message(const char *s){portENTER_CRITICAL(&lock);snprintf(state.message,sizeof(state.message),"%s",s);portEXIT_CRITICAL(&lock);ESP_LOGI("gso_wifi","%s",s);}
void gss_wifi_radio_status(gss_wifi_status *out){portENTER_CRITICAL(&lock);*out=state;portEXIT_CRITICAL(&lock);}
static bool enqueue(int kind){command c={.kind=kind};portENTER_CRITICAL(&lock);bool ready=state.ready;portEXIT_CRITICAL(&lock);return ready&&commands&&xQueueSend(commands,&c,0)==pdTRUE;}
bool gss_wifi_scan(void){return enqueue(SCAN);}
bool gss_wifi_connect_saved(void){return enqueue(CONNECT);}
bool gss_wifi_disconnect(void){return enqueue(DISCONNECT);}
static void event(void *arg,esp_event_base_t base,int32_t id,void *data){
 (void)arg;
 if(base==WIFI_EVENT&&id==WIFI_EVENT_SCAN_DONE){enqueue(SCAN_DONE);}
 if(base==WIFI_EVENT&&id==WIFI_EVENT_STA_DISCONNECTED){
  wifi_event_sta_disconnected_t *d=data;
  ESP_LOGW("gso_wifi","DISCONNECTED reason=%u",(unsigned)d->reason);
  portENTER_CRITICAL(&lock);state.connected=false;state.ip[0]=0;state.reason=d->reason;portEXIT_CRITICAL(&lock);
  enqueue(RETRY);
 }
 if(base==IP_EVENT&&id==IP_EVENT_STA_GOT_IP){
  ip_event_got_ip_t *ip=data;
  portENTER_CRITICAL(&lock);state.connected=true;state.reason=0;snprintf(state.ip,sizeof(state.ip),IPSTR,IP2STR(&ip->ip_info.ip));portEXIT_CRITICAL(&lock);
  /* No credentials or SSID in serial output. */
  ESP_LOGI("gso_wifi","GOT_IP " IPSTR,IP2STR(&ip->ip_info.ip));message("已连接 · 已获取 IP");
  enqueue(CHECK_NET);
 }
}
static esp_err_t initialize(void){
 gpio_config_t boot={.pin_bit_mask=1ULL<<GSO_C5_BOOT_GPIO,.mode=GPIO_MODE_OUTPUT};
 esp_err_t e=gpio_config(&boot);if(e!=ESP_OK)return e;gpio_set_level(GSO_C5_BOOT_GPIO,1);
 if((e=esp_hosted_init())!=ESP_OK)return e;
 if((e=esp_netif_init())!=ESP_OK)return e;
 e=esp_event_loop_create_default();if(e!=ESP_OK&&e!=ESP_ERR_INVALID_STATE)return e;
 if(!esp_netif_create_default_wifi_sta())return ESP_FAIL;
 wifi_init_config_t cfg=WIFI_INIT_CONFIG_DEFAULT();
 if((e=esp_wifi_remote_init(&cfg))!=ESP_OK)return e;
 if((e=esp_event_handler_register(WIFI_EVENT,ESP_EVENT_ANY_ID,event,NULL))!=ESP_OK)return e;
 if((e=esp_event_handler_register(IP_EVENT,IP_EVENT_STA_GOT_IP,event,NULL))!=ESP_OK)return e;
 if((e=esp_wifi_remote_set_storage(WIFI_STORAGE_RAM))!=ESP_OK)return e;
 if((e=esp_wifi_remote_set_mode(WIFI_MODE_STA))!=ESP_OK)return e;
 e=esp_wifi_remote_start();if(e!=ESP_OK)return e;
 e=esp_wifi_remote_set_ps(WIFI_PS_NONE);if(e!=ESP_OK)ESP_LOGW("gso_wifi","Power-save setting: %s",esp_err_to_name(e));
 return ESP_OK;
}
static esp_err_t connect_profile(void){
 char ssid[33],password[65];bool open;
 if(!gss_wifi_copy(ssid,password,&open))return ESP_ERR_INVALID_ARG;
 wifi_config_t cfg={0};memcpy(cfg.sta.ssid,ssid,strlen(ssid));memcpy(cfg.sta.password,password,strlen(password));
 cfg.sta.threshold.authmode=open?WIFI_AUTH_OPEN:WIFI_AUTH_WPA2_PSK;cfg.sta.pmf_cfg.capable=true;
 esp_err_t e=esp_wifi_remote_set_config(WIFI_IF_STA,&cfg);
 memset(password,0,sizeof(password));memset(&cfg,0,sizeof(cfg));
 if(e==ESP_OK)e=esp_wifi_remote_connect();
 connect_deadline=esp_timer_get_time()+30000000;
 return e;
}
static void worker(void *arg){
 (void)arg;message("正在初始化 C5 无线模块");
 esp_err_t e=initialize();
 if(e!=ESP_OK){char s[96];snprintf(s,sizeof(s),"C5 无线模块未就绪 · %s",esp_err_to_name(e));message(s);vTaskDelete(NULL);return;}
 portENTER_CRITICAL(&lock);state.ready=true;portEXIT_CRITICAL(&lock);message("Wi-Fi 就绪");
 if(gss_wifi_ssid()[0])enqueue(CONNECT);else enqueue(SCAN);
 command cmd;
 for(;;){
  if(!xQueueReceive(commands,&cmd,pdMS_TO_TICKS(1000))){
   gss_wifi_status copy;gss_wifi_radio_status(&copy);
   if(copy.connected){connect_deadline=0;retries=0;}
   else if(desired&&connect_deadline&&esp_timer_get_time()>connect_deadline){desired=false;connect_deadline=0;retry_at=esp_timer_get_time()+60000000;esp_wifi_remote_disconnect();message("连接超时 · 稍后自动重连");}
   else if(automatic&&!desired&&retry_at&&esp_timer_get_time()>=retry_at){retry_at=0;enqueue(CONNECT);}
   continue;
  }
  e=ESP_OK;
  if(cmd.kind==CONNECT){desired=false;esp_wifi_remote_disconnect();automatic=desired=true;retry_at=0;retries=0;message("正在连接 Wi-Fi");e=connect_profile();}
  else if(cmd.kind==DISCONNECT){automatic=desired=false;retry_at=connect_deadline=0;e=esp_wifi_remote_disconnect();message("已断开 Wi-Fi");}
  else if(cmd.kind==RETRY){
   gss_wifi_status copy;gss_wifi_radio_status(&copy);
   if(desired&&!copy.connected){if(retries++<3){message("连接中断 · 正在重试");vTaskDelay(pdMS_TO_TICKS(1500*retries));connect_deadline=esp_timer_get_time()+30000000;e=esp_wifi_remote_connect();}
    else {desired=false;connect_deadline=0;retry_at=esp_timer_get_time()+60000000;char s[96];snprintf(s,sizeof(s),"连接失败 · 原因 %d · 稍后自动重连",copy.reason);message(s);}}
  }else if(cmd.kind==CHECK_NET){check_network();
  }else if(cmd.kind==SCAN){
   gss_wifi_status copy;gss_wifi_radio_status(&copy);if(copy.scanning)continue;
   wifi_scan_config_t scan={.show_hidden=false,.scan_type=WIFI_SCAN_TYPE_ACTIVE};
   portENTER_CRITICAL(&lock);state.scanning=true;portEXIT_CRITICAL(&lock);
   e=esp_wifi_remote_scan_start(&scan,false);
   if(e!=ESP_OK){portENTER_CRITICAL(&lock);state.scanning=false;portEXIT_CRITICAL(&lock);}
  }else if(cmd.kind==SCAN_DONE){
   wifi_ap_record_t records[GSS_WIFI_AP_LIMIT];uint16_t count=GSS_WIFI_AP_LIMIT;
   e=esp_wifi_remote_scan_get_ap_records(&count,records);
   portENTER_CRITICAL(&lock);state.scanning=false;state.count=0;
   if(e==ESP_OK)for(unsigned i=0;i<count;i++){
    bool duplicate=false;for(unsigned j=0;j<state.count;j++)if(!strncmp(state.aps[j].ssid,(char*)records[i].ssid,32))duplicate=true;
    if(duplicate||!records[i].ssid[0])continue;
    gss_wifi_ap *ap=&state.aps[state.count++];memcpy(ap->ssid,records[i].ssid,32);ap->ssid[32]=0;ap->rssi=records[i].rssi;ap->open=records[i].authmode==WIFI_AUTH_OPEN;
   }
   state.generation++;portEXIT_CRITICAL(&lock);ESP_LOGI("gso_wifi","SCAN_DONE count=%u",count);
  }
  if(e!=ESP_OK){char s[96];snprintf(s,sizeof(s),"Wi-Fi 操作失败 · %s",esp_err_to_name(e));message(s);}
  memset(&cmd,0,sizeof(cmd));
 }
}
void gss_wifi_radio_start(void){if(commands)return;commands=xQueueCreate(6,sizeof(command));if(commands)xTaskCreatePinnedToCore(worker,"gso_wifi",12288,NULL,3,NULL,0);}
void gss_wifi_radio_stop(void){}
const char *gss_wifi_transport_note(void){return "开机自动连接已存网络 · 实屏通过 Wi-Fi 直连 GSN";}
