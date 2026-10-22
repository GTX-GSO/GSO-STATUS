/* Direct Mihomo control: one PATCH, then read the actual mode; no PATCH replay. */
#include "clash_client.h"
#include "data_source.h"
#include "cJSON.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef ESP_PLATFORM
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_http_client.h"
#include "gss_wifi.h"
static SemaphoreHandle_t gate;static TaskHandle_t worker;
static void lock(void){xSemaphoreTake(gate,portMAX_DELAY);}static void unlock(void){xSemaphoreGive(gate);}
#else
#define UNICODE
#define _UNICODE
#include <windows.h>
#include <winhttp.h>
static SRWLOCK gate=SRWLOCK_INIT;static HANDLE worker,wake,stop;
static void lock(void){AcquireSRWLockExclusive(&gate);}static void unlock(void){ReleaseSRWLockExclusive(&gate);}
typedef struct {PVOID volatile request;} clash_deadline;
static VOID CALLBACK deadline_close(PVOID context,BOOLEAN fired){(void)fired;clash_deadline *d=context;HINTERNET h=InterlockedExchangePointer(&d->request,NULL);if(h)WinHttpCloseHandle(h);}
#endif
static desk_clash_result result;
static char endpoint[288],secret[257],pending[8];
static bool valid_mode(const char *s){return s&&(!strcmp(s,"rule")||!strcmp(s,"global")||!strcmp(s,"direct"));}
#ifdef ESP_PLATFORM
typedef struct {char *text;size_t used,capacity;bool overflow;} clash_response;
static esp_err_t receive(esp_http_client_event_t *event){
 clash_response *r=event->user_data;if(event->event_id==HTTP_EVENT_ON_DATA&&event->data_len>0){if(r->used+(size_t)event->data_len>=r->capacity){r->overflow=true;return ESP_FAIL;}memcpy(r->text+r->used,event->data,event->data_len);r->used+=event->data_len;r->text[r->used]=0;}return ESP_OK;
}
static int exchange(bool patch,const char *body,char *response,size_t capacity){
 clash_response r={.text=response,.capacity=capacity};response[0]=0;
 esp_http_client_config_t config={.url=endpoint,.timeout_ms=2000,.disable_auto_redirect=true,.max_authorization_retries=-1,.event_handler=receive,.user_data=&r};
 esp_http_client_handle_t client=esp_http_client_init(&config);if(!client)return -1;
 char authorization[272];snprintf(authorization,sizeof(authorization),"Bearer %s",secret);esp_http_client_set_header(client,"Authorization",authorization);
 if(patch){esp_http_client_set_method(client,HTTP_METHOD_PATCH);esp_http_client_set_header(client,"Content-Type","application/json");esp_http_client_set_post_field(client,body,strlen(body));}
 esp_err_t e=esp_http_client_perform(client);int status=esp_http_client_get_status_code(client);esp_http_client_cleanup(client);return e==ESP_OK&&!r.overflow?status:-1;
}
#else
static int exchange(bool patch,const char *body,char *response,size_t capacity){
 wchar_t url[320],host[256],path[64],authorization[320];int code=-1;
 clash_deadline deadline={0};HANDLE timer=NULL;
 if(!MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,endpoint,-1,url,320))return -1;
 URL_COMPONENTS parts={.dwStructSize=sizeof(parts),.lpszHostName=host,.dwHostNameLength=256,.lpszUrlPath=path,.dwUrlPathLength=64};
 if(!WinHttpCrackUrl(url,0,0,&parts))return -1;
 HINTERNET session=WinHttpOpen(L"GSS Mihomo",WINHTTP_ACCESS_TYPE_NO_PROXY,WINHTTP_NO_PROXY_NAME,WINHTTP_NO_PROXY_BYPASS,0);if(!session)return -1;
 WinHttpSetTimeouts(session,2000,2000,2000,2000);
 HINTERNET connection=WinHttpConnect(session,host,parts.nPort,0),request=NULL;if(!connection)goto done;
 request=WinHttpOpenRequest(connection,patch?L"PATCH":L"GET",path,NULL,WINHTTP_NO_REFERER,WINHTTP_DEFAULT_ACCEPT_TYPES,parts.nScheme==INTERNET_SCHEME_HTTPS?WINHTTP_FLAG_SECURE:0);if(!request)goto done;
 deadline.request=request;if(!CreateTimerQueueTimer(&timer,NULL,deadline_close,&deadline,2500,0,WT_EXECUTEONLYONCE))goto done;
 DWORD redirects=WINHTTP_OPTION_REDIRECT_POLICY_NEVER,disable=WINHTTP_DISABLE_AUTHENTICATION;WinHttpSetOption(request,WINHTTP_OPTION_REDIRECT_POLICY,&redirects,sizeof(redirects));WinHttpSetOption(request,WINHTTP_OPTION_DISABLE_FEATURE,&disable,sizeof(disable));
 wchar_t token[257];if(!MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,secret,-1,token,257))goto done;
 swprintf(authorization,320,L"Authorization: Bearer %ls\r\nContent-Type: application/json\r\n",token);DWORD size=body?(DWORD)strlen(body):0;
 if(!WinHttpSendRequest(request,authorization,(DWORD)-1L,(LPVOID)body,size,size,0)||!WinHttpReceiveResponse(request,NULL))goto done;
 DWORD status=0,n=sizeof(status);if(!WinHttpQueryHeaders(request,WINHTTP_QUERY_STATUS_CODE|WINHTTP_QUERY_FLAG_NUMBER,WINHTTP_HEADER_NAME_BY_INDEX,&status,&n,WINHTTP_NO_HEADER_INDEX))goto done;
 size_t used=0;for(;;){DWORD count=0;if(used>=capacity-1||!WinHttpReadData(request,response+used,(DWORD)(capacity-1-used),&count))goto done;if(!count)break;used+=count;}response[used]=0;code=(int)status;
done:if(timer)DeleteTimerQueueTimer(NULL,timer,INVALID_HANDLE_VALUE);deadline_close(&deadline,false);if(connection)WinHttpCloseHandle(connection);WinHttpCloseHandle(session);return code;
}
#endif
static void perform(void){
 char wanted[8],body[32];lock();strcpy(wanted,pending);pending[0]=0;unlock();
 char *response=malloc(16384);if(!response){lock();result.busy=false;result.state=CLASH_UNAVAILABLE;unlock();return;}
 int patched=0;if(wanted[0]){snprintf(body,sizeof(body),"{\"mode\":\"%s\"}",wanted);patched=exchange(true,body,response,16384);}
 int status=exchange(false,NULL,response,16384);char actual[8]={0};
 if(status==200){cJSON *json=cJSON_ParseWithOpts(response,NULL,true);const cJSON *mode=cJSON_GetObjectItemCaseSensitive(json,"mode");if(cJSON_IsString(mode)&&valid_mode(mode->valuestring))strcpy(actual,mode->valuestring);cJSON_Delete(json);}
 free(response);lock();result.busy=pending[0]!=0;
 if(actual[0]){strcpy(result.mode,actual);result.observed_ms=desk_data_now_ms();result.state=wanted[0]&&strcmp(wanted,actual)?CLASH_UNCONFIRMED:CLASH_CONFIRMED;}
 else{result.mode[0]=0;result.state=status==401||status==403||patched==401||patched==403?CLASH_AUTH:wanted[0]?CLASH_UNCONFIRMED:CLASH_UNAVAILABLE;}
 if(status==401||status==403||patched==401||patched==403)result.state=CLASH_AUTH;
 if(result.busy)result.state=CLASH_SWITCHING;
 unlock();
}
#ifdef ESP_PLATFORM
/* The TCP/IP stack only exists once the radio is up: wait for a connection instead of resolving the host at boot. */
static void run(void *unused){
 (void)unused;
 for(;;){
  gss_wifi_status wifi;gss_wifi_radio_status(&wifi);
  if(!wifi.connected){ulTaskNotifyTake(pdTRUE,pdMS_TO_TICKS(500));continue;}
  perform();ulTaskNotifyTake(pdTRUE,pdMS_TO_TICKS(10000));
 }
}
#else
static DWORD WINAPI run(void *unused){(void)unused;HANDLE events[]={stop,wake};for(;;){perform();if(WaitForMultipleObjects(2,events,FALSE,10000)==WAIT_OBJECT_0)break;}return 0;}
#endif
bool desk_clash_start(const char *url,const char *token){
 if(worker||!url||!token||strlen(url)>255||strlen(token)>256||strpbrk(token,"\r\n"))return false;
 const char *host=!strncmp(url,"http://",7)?url+7:!strncmp(url,"https://",8)?url+8:NULL;if(!host||!*host)return false;
 size_t n=strcspn(host,"/");if(!n||(host[n]&&strcmp(host+n,"/"))||strpbrk(host,"@?#\\\r\n \t"))return false;
 snprintf(endpoint,sizeof(endpoint),"%.*s/configs",(int)((host-url)+n),url);strcpy(secret,token);
#ifdef ESP_PLATFORM
 if(!gate){gate=xSemaphoreCreateMutex();}if(!gate)return false;
#endif
 lock();result=(desk_clash_result){.configured=true};pending[0]=0;unlock();
#ifdef ESP_PLATFORM
 if(xTaskCreate(run,"gss_clash",6144,NULL,2,&worker)==pdPASS)return true;
#else
 wake=CreateEventW(NULL,FALSE,FALSE,NULL);stop=CreateEventW(NULL,TRUE,FALSE,NULL);if(wake&&stop){worker=CreateThread(NULL,0,run,NULL,0,NULL);if(worker)return true;}if(wake)CloseHandle(wake);if(stop)CloseHandle(stop);wake=stop=NULL;
#endif
 lock();result.configured=false;unlock();memset(secret,0,sizeof(secret));return false;
}
desk_clash_result desk_clash_latest(void){
#ifdef ESP_PLATFORM
 if(!gate)return (desk_clash_result){0};
#endif
 lock();desk_clash_result copy=result;unlock();return copy;
}
bool desk_clash_mode(const char *mode){
 if(!worker||!valid_mode(mode)){return false;}lock();if(result.busy){unlock();return false;}strcpy(pending,mode);result.busy=true;result.state=CLASH_SWITCHING;unlock();
#ifdef ESP_PLATFORM
 xTaskNotifyGive(worker);
#else
 SetEvent(wake);
#endif
 return true;
}
void desk_clash_stop(void){
#ifndef ESP_PLATFORM
 if(!worker)return;SetEvent(stop);WaitForSingleObject(worker,INFINITE);CloseHandle(worker);CloseHandle(wake);CloseHandle(stop);worker=wake=stop=NULL;lock();memset(&result,0,sizeof(result));memset(secret,0,sizeof(secret));unlock();
#endif
}
