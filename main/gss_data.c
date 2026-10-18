#include <stdio.h>
#include <string.h>
#include <time.h>
#include <math.h>
#include <sys/time.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "esp_timer.h"
#include "esp_random.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "cJSON.h"
#include "data_source.h"
#include "home_client.h"
#include "clash_client.h"
#include "gss_wifi.h"
#include "gso_home_delivery.h"
#include "gso_link.h"
static SemaphoreHandle_t gate;
static QueueHandle_t home_queue;
static gso_snapshot *latest,*incoming;
static uint64_t last_snapshot;
static desk_connection_status connection=DESK_CONNECTING;
static desk_home_result result;
static char command_id[37],base_url[256],read_auth[272],control_auth[272];
static uint8_t *thumb_incoming,*thumb_ready; /* model picture of the current job: being fetched / ready to hand out */
static char thumb_key[17],thumb_attempt_key[17];
static uint64_t thumb_attempt_ms;
static size_t last_body_size;
static bool configured;
typedef struct {char id[37],epoch[37],entity[161],mode[65];unsigned action;gso_metric value;uint64_t queued;} home_request;
typedef struct {char *body;size_t size,capacity;bool overflow;} response;
uint64_t desk_data_now_ms(void){return esp_timer_get_time()/1000;}
static const char *text(const cJSON *o,const char *k){const cJSON *v=cJSON_GetObjectItemCaseSensitive(o,k);return cJSON_IsString(v)?v->valuestring:"";}
static esp_err_t receive(esp_http_client_event_t *event){
 response *r=event->user_data;
 if(event->event_id==HTTP_EVENT_ON_DATA&&event->data_len>0){
  if(r->size+(size_t)event->data_len>r->capacity){r->overflow=true;return ESP_FAIL;}
  memcpy(r->body+r->size,event->data,event->data_len);r->size+=event->data_len;r->body[r->size]=0;
 }
 return ESP_OK;
}
/* Each invocation issues one request. No redirect or application POST replay. */
static int request(const char *path,bool post,const char *payload,char *body,size_t capacity){
 char url[384];if(snprintf(url,sizeof(url),"%s%s",base_url,path)>=(int)sizeof(url))return -1;
 response r={.body=body,.capacity=capacity};body[0]=0;
 esp_http_client_config_t config={.url=url,.timeout_ms=2000,.event_handler=receive,.user_data=&r,.disable_auto_redirect=true,.max_authorization_retries=-1,.buffer_size=2048,.buffer_size_tx=1024};
 esp_http_client_handle_t client=esp_http_client_init(&config);if(!client)return -1;
 esp_http_client_set_header(client,"Authorization",post?control_auth:read_auth);
 esp_http_client_set_header(client,"Accept","application/json");
 if(post){esp_http_client_set_method(client,HTTP_METHOD_POST);esp_http_client_set_header(client,"Content-Type","application/json");esp_http_client_set_post_field(client,payload,strlen(payload));}
 esp_err_t e=esp_http_client_perform(client);int code=esp_http_client_get_status_code(client);
 esp_http_client_cleanup(client);last_body_size=r.size;return e==ESP_OK&&!r.overflow?code:-1;
}
static bool load_config(void){
 if(nvs_flash_init_partition("gsn_config")!=ESP_OK)return false;
 nvs_handle_t h;if(nvs_open_from_partition("gsn_config","gso_net",NVS_READONLY,&h)!=ESP_OK)return false;
 char token[257];size_t n=sizeof(base_url);bool ok=nvs_get_str(h,"base_url",base_url,&n)==ESP_OK;
 n=sizeof(token);ok=ok&&nvs_get_str(h,"read_token",token,&n)==ESP_OK&&strlen(token)>=32;
 if(ok)snprintf(read_auth,sizeof(read_auth),"Bearer %s",token);
 n=sizeof(token);if(nvs_get_str(h,"control_token",token,&n)==ESP_OK&&strlen(token)>=32)snprintf(control_auth,sizeof(control_auth),"Bearer %s",token);
 char mihomo[256];n=sizeof(mihomo);bool has_mihomo=nvs_get_str(h,"mihomo_url",mihomo,&n)==ESP_OK;n=sizeof(token);if(has_mihomo&&nvs_get_str(h,"mihomo_token",token,&n)==ESP_OK)desk_clash_start(mihomo,token);
 memset(token,0,sizeof(token));nvs_close(h);
 return ok&&(!strncmp(base_url,"http://",7)||!strncmp(base_url,"https://",8));
}
static void set_connection(desk_connection_status s){
 xSemaphoreTake(gate,portMAX_DELAY);bool changed=connection!=s;connection=s;xSemaphoreGive(gate);
 if(changed)ESP_LOGI("gso_net","GSN_STATUS %d",s);
}
/* Follows the id the snapshot names for the current job: fetched once per id (a failed try waits ten seconds) and dropped
   when the job has no picture. The body is raw ARGB8888 of an exact size; anything else is a failure. */
static void update_thumbnail(void){
 if(!thumb_incoming||!thumb_ready)return;
 char key[17]={0};
 xSemaphoreTake(gate,portMAX_DELAY);if(latest->print.reported)snprintf(key,sizeof(key),"%s",latest->print.thumbnail);xSemaphoreGive(gate);
 if(!key[0]){if(thumb_key[0]){xSemaphoreTake(gate,portMAX_DELAY);thumb_key[0]=0;xSemaphoreGive(gate);}return;}
 if(!strcmp(key,thumb_key))return;
 uint64_t now=desk_data_now_ms();
 if(!strcmp(key,thumb_attempt_key)&&now-thumb_attempt_ms<10000)return;
 snprintf(thumb_attempt_key,sizeof(thumb_attempt_key),"%s",key);thumb_attempt_ms=now;
 char path[64];snprintf(path,sizeof(path),"/api/v1/printer/thumbnail?id=%s",key);
 int code=request(path,false,NULL,(char *)thumb_incoming,GSO_THUMBNAIL_BYTES);
 if(code!=200||last_body_size!=GSO_THUMBNAIL_BYTES)return;
 xSemaphoreTake(gate,portMAX_DELAY);memcpy(thumb_ready,thumb_incoming,GSO_THUMBNAIL_BYTES);snprintf(thumb_key,sizeof(thumb_key),"%s",key);xSemaphoreGive(gate);
}
bool desk_data_thumbnail(const char *key,uint8_t *pixels){
 if(!gate||!thumb_ready||!key||!key[0])return false;
 bool found=false;
 xSemaphoreTake(gate,portMAX_DELAY);
 if(!strcmp(key,thumb_key)){memcpy(pixels,thumb_ready,GSO_THUMBNAIL_BYTES);found=true;}
 xSemaphoreGive(gate);
 return found;
}
static void poll_snapshots(void *arg){
 (void)arg;char *body=heap_caps_malloc(GSO_SNAPSHOT_MAX_BYTES+1,MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);if(!body){set_connection(DESK_GSN_RESPONSE);vTaskDelete(NULL);return;}
 gso_link link;gso_link_init(&link,desk_data_now_ms(),DESK_CONNECTING);
 for(;;){
  gss_wifi_status wifi;gss_wifi_radio_status(&wifi);
  if(!wifi.connected){set_connection(DESK_NETWORK_OFFLINE);vTaskDelay(pdMS_TO_TICKS(500));continue;}
  if(!configured){set_connection(DESK_GSN_AUTH);vTaskDelay(pdMS_TO_TICKS(2000));continue;}
  int code=request("/api/v1/snapshot",false,NULL,body,GSO_SNAPSHOT_MAX_BYTES);
  if(code==200&&gso_snapshot_parse(body,strlen(body),desk_data_now_ms(),incoming)){
   xSemaphoreTake(gate,portMAX_DELAY);gso_snapshot *old=latest;latest=incoming;incoming=old;last_snapshot=desk_data_now_ms();int64_t stamp=latest->generated_at;xSemaphoreGive(gate);
   static uint64_t clock_at;if(!clock_at||desk_data_now_ms()-clock_at>60000){struct timeval tv={.tv_sec=stamp};settimeofday(&tv,NULL);clock_at=desk_data_now_ms();}
   set_connection(gso_link_update(&link,desk_data_now_ms(),true,0,false,DESK_ONLINE));update_thumbnail();static unsigned count;if(++count==1||count%60==0)ESP_LOGI("gso_net","SNAPSHOT_OK count=%u bytes=%u",count,(unsigned)strlen(body));
  }else{int failure=code==401||code==403?DESK_GSN_AUTH:code<0?DESK_GSN_OFFLINE:DESK_GSN_RESPONSE;
   set_connection(gso_link_update(&link,desk_data_now_ms(),false,failure,failure==DESK_GSN_AUTH,DESK_ONLINE));}
  vTaskDelay(pdMS_TO_TICKS(code==200?1000:2000));
 }
}
bool desk_data_latest(gso_snapshot *out){if(!gate||!latest)return false;xSemaphoreTake(gate,portMAX_DELAY);*out=*latest;xSemaphoreGive(gate);return out->generated_at!=0;}
desk_connection_status desk_data_status(void){
 if(!gate){return DESK_CONNECTING;}
 gss_wifi_status wifi;gss_wifi_radio_status(&wifi);if(!wifi.connected)return DESK_NETWORK_OFFLINE;
 xSemaphoreTake(gate,portMAX_DELAY);desk_connection_status s=connection;if(s==DESK_ONLINE&&desk_data_now_ms()-last_snapshot>GSO_LINK_GRACE_MS)s=DESK_GSN_OFFLINE;xSemaphoreGive(gate);return s;
}
bool desk_data_connected(void){return desk_data_status()==DESK_ONLINE;}
bool desk_home_ready(void){if(!gate||!configured||!control_auth[0]||!desk_data_connected())return false;xSemaphoreTake(gate,portMAX_DELAY);bool ready=latest->home_epoch[0]!=0;xSemaphoreGive(gate);return ready;}
static bool current(const home_request *r){xSemaphoreTake(gate,portMAX_DELAY);bool ok=!strcmp(r->id,command_id);xSemaphoreGive(gate);return ok;}
static void finish(const home_request *r,desk_home_state state,bool busy){xSemaphoreTake(gate,portMAX_DELAY);if(!strcmp(r->id,command_id)){result.state=state;result.busy=busy;result.updated_ms=desk_data_now_ms();}xSemaphoreGive(gate);}
static bool update_result(const home_request *r,const char *body){
 cJSON *o=cJSON_Parse(body);if(!o)return false;
 bool terminal=false;
 if(!strcmp(text(o,"command_id"),r->id)&&!strcmp(text(o,"entity_id"),r->entity)&&!strcmp(text(o,"action"),gso_home_action_name(r->action))){
  const char *s=text(o,"status");desk_home_state state=DESK_HOME_UNKNOWN;bool known=true;
  if(!strcmp(s,"confirmed"))state=DESK_HOME_CONFIRMED;else if(!strcmp(s,"rejected"))state=DESK_HOME_REJECTED;
  else if(!strcmp(s,"timed_out"))state=DESK_HOME_TIMED_OUT;else if(!strcmp(s,"superseded"))state=DESK_HOME_SUPERSEDED;
  else if(!strcmp(s,"unknown"))state=DESK_HOME_UNKNOWN;else if(!strcmp(s,"sent"))state=DESK_HOME_SENT;
  else if(!strcmp(s,"queued"))state=DESK_HOME_QUEUED;else known=false;
  if(known){terminal=state!=DESK_HOME_SENT&&state!=DESK_HOME_QUEUED;finish(r,state,!terminal);}
 }
 cJSON_Delete(o);return terminal;
}
static int deliver_request(void *context,bool post,const char *payload,char *body,size_t capacity){home_request *r=context;char path[96];snprintf(path,sizeof(path),"/api/v1/home/commands%s%s",post?"":"/",post?"":r->id);return request(path,post,payload,body,capacity);}
static bool deliver_apply(void *context,const char *body){return update_result(context,body);}
static bool deliver_current(void *context){return current(context);}
static uint64_t deliver_now(void *context){(void)context;return desk_data_now_ms();}
static void deliver_wait(void *context){(void)context;vTaskDelay(pdMS_TO_TICKS(500));}
static void home_worker(void *arg){
 (void)arg;home_request r;char response_body[4097];
 for(;;){
  if(!xQueueReceive(home_queue,&r,portMAX_DELAY))continue;
  if(!current(&r))continue;
  xSemaphoreTake(gate,portMAX_DELAY);bool epoch=!strcmp(r.epoch,latest->home_epoch);xSemaphoreGive(gate);
  if(desk_data_now_ms()-r.queued>2000||!desk_home_ready()||!epoch){finish(&r,DESK_HOME_REJECTED,false);continue;}
  bool cover=gso_home_action_slow(r.action);
  time_t expiry=time(NULL)+(cover?28:10);struct tm utc;gmtime_r(&expiry,&utc);char expires[32];strftime(expires,sizeof(expires),"%Y-%m-%dT%H:%M:%SZ",&utc);
  cJSON *o=cJSON_CreateObject();cJSON_AddStringToObject(o,"request_id",r.id);cJSON_AddStringToObject(o,"control_epoch",r.epoch);cJSON_AddStringToObject(o,"entity_id",r.entity);cJSON_AddStringToObject(o,"action",gso_home_action_name(r.action));cJSON_AddStringToObject(o,"expires_at",expires);
  const char *key=r.action==GSO_HOME_TEMPERATURE?"temperature_c":r.action==GSO_HOME_BRIGHTNESS?"brightness_percent":r.action==GSO_HOME_COLOR?"color_temperature_kelvin":r.action==GSO_HOME_SPEED?"percentage":r.action==GSO_HOME_HUMIDITY?"humidity_percent":NULL;
  if(key&&r.value.valid){cJSON_AddNumberToObject(o,key,r.value.value);}
  if(r.mode[0]){cJSON_AddStringToObject(o,"mode",r.mode);}
  char *payload=cJSON_PrintUnformatted(o);cJSON_Delete(o);if(!payload){finish(&r,DESK_HOME_REJECTED,false);continue;}
  gso_home_delivery io={.context=&r,.request=deliver_request,.apply=deliver_apply,.current=deliver_current,.now=deliver_now,.wait=deliver_wait};
  gso_delivery_result delivered=gso_home_deliver(&io,payload,r.queued+(cover?32000:12000),response_body,4096);cJSON_free(payload);
  if(delivered!=GSO_DELIVERY_DONE)finish(&r,delivered==GSO_DELIVERY_REJECTED?DESK_HOME_REJECTED:DESK_HOME_UNKNOWN,false);
 }
}
bool desk_data_start(const char *unused){
 (void)unused;gate=xSemaphoreCreateMutex();home_queue=xQueueCreate(4,sizeof(home_request));
 latest=heap_caps_calloc(1,sizeof(*latest),MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);incoming=heap_caps_malloc(sizeof(*incoming),MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
 if(!gate||!home_queue||!latest||!incoming){return false;}
 thumb_incoming=heap_caps_malloc(GSO_THUMBNAIL_BYTES+1,MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);thumb_ready=heap_caps_malloc(GSO_THUMBNAIL_BYTES,MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT); /* without them the picture is simply not shown */
 gso_snapshot_empty(latest);configured=load_config();
 bool a=xTaskCreatePinnedToCore(poll_snapshots,"gso_gsn",12288,NULL,3,NULL,0)==pdPASS;
 bool b=xTaskCreatePinnedToCore(home_worker,"gso_home",12288,NULL,3,NULL,0)==pdPASS;return a&&b;
}
void desk_data_stop(void){}
bool desk_home_start(const wchar_t *host,unsigned short port,bool secure,const wchar_t *route,const wchar_t *read,const wchar_t *token){(void)host;(void)port;(void)secure;(void)route;(void)read;(void)token;return configured;}
void desk_home_stop(void){}
desk_home_result desk_home_latest(void){if(!gate)return (desk_home_result){0};xSemaphoreTake(gate,portMAX_DELAY);desk_home_result copy=result;xSemaphoreGive(gate);return copy;}
bool desk_home_setting(const char *id,unsigned action,gso_metric value,const char *mode){
 if(!desk_home_ready()||!id||strlen(id)>160||(mode&&strlen(mode)>64)||(value.valid&&!isfinite(value.value)))return false;
 home_request r={.action=action,.value=value,.queued=desk_data_now_ms()};strcpy(r.entity,id);if(mode)strcpy(r.mode,mode);
 xSemaphoreTake(gate,portMAX_DELAY);bool allowed=gso_snapshot_action_allowed(latest,id,action,desk_data_now_ms());
 bool cover=action==GSO_HOME_OPEN||action==GSO_HOME_CLOSE||action==GSO_HOME_STOP;
 if(!allowed||(result.busy&&(!cover||strcmp(id,result.entity_id)))){xSemaphoreGive(gate);return false;}
 memcpy(r.epoch,latest->home_epoch,sizeof(r.epoch));uint32_t a=esp_random(),b=esp_random(),c=esp_random(),d=esp_random();snprintf(r.id,sizeof(r.id),"%08lx-%04lx-4%03lx-8%03lx-%04lx%08lx",(unsigned long)a,(unsigned long)(b>>16),(unsigned long)(b&0xfff),(unsigned long)(c&0xfff),(unsigned long)(c>>16),(unsigned long)d);
 /* Publish identity while holding the same lock as the result consumer. */
 bool ok=xQueueSend(home_queue,&r,0)==pdTRUE;if(ok){strcpy(command_id,r.id);result=(desk_home_result){.state=DESK_HOME_QUEUED,.busy=true,.action=action,.updated_ms=r.queued};strcpy(result.entity_id,id);}xSemaphoreGive(gate);return ok;
}
bool desk_home_command(const char *id,unsigned action,gso_metric value){return desk_home_setting(id,action,value,NULL);}
