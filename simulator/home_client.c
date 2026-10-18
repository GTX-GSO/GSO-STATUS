#define UNICODE
#define _UNICODE
#include <windows.h>
#include <winhttp.h>
#include <bcrypt.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <math.h>
#include "home_client.h"
#include "data_source.h"
#include "cJSON.h"

static SRWLOCK gate=SRWLOCK_INIT;
static HANDLE worker,stop_event,wake_event;
static HINTERNET session;
static wchar_t server[256],route[512],read_header[512],control_header[512];
static unsigned short server_port;
static bool tls;
static desk_home_result result;
typedef struct {char body[2048],id[37],epoch[37],entity[161];unsigned action;uint64_t queued;} home_request;
static home_request pending;
typedef struct {PVOID volatile request;} timed_request;
static VOID CALLBACK close_request(PVOID context,BOOLEAN fired){(void)fired;timed_request *r=context;HINTERNET handle=InterlockedExchangePointer(&r->request,NULL);if(handle)WinHttpCloseHandle(handle);}

static bool exchange(const wchar_t *method,const wchar_t *address,const wchar_t *header,const char *body,char *response,size_t capacity,DWORD *status){
 bool ok=false;HINTERNET connection=WinHttpConnect(session,server,server_port,0);if(!connection)return false;
 HINTERNET request=WinHttpOpenRequest(connection,method,address,NULL,WINHTTP_NO_REFERER,WINHTTP_DEFAULT_ACCEPT_TYPES,tls?WINHTTP_FLAG_SECURE:0);if(!request){WinHttpCloseHandle(connection);return false;}
 timed_request timed={request};HANDLE timer=NULL;if(!CreateTimerQueueTimer(&timer,NULL,close_request,&timed,800,0,WT_EXECUTEONLYONCE)){WinHttpCloseHandle(request);WinHttpCloseHandle(connection);return false;}
 DWORD redirect=WINHTTP_OPTION_REDIRECT_POLICY_NEVER;WinHttpSetOption(request,WINHTTP_OPTION_REDIRECT_POLICY,&redirect,sizeof(redirect));
 wchar_t headers[640];swprintf(headers,640,L"%lsContent-Type: application/json\r\n",header);
 DWORD size=body?(DWORD)strlen(body):0;
 if(!WinHttpSendRequest(request,headers,(DWORD)-1L,(LPVOID)body,size,size,0)||!WinHttpReceiveResponse(request,NULL))goto done;
 DWORD length=sizeof(*status);if(!WinHttpQueryHeaders(request,WINHTTP_QUERY_STATUS_CODE|WINHTTP_QUERY_FLAG_NUMBER,WINHTTP_HEADER_NAME_BY_INDEX,status,&length,WINHTTP_NO_HEADER_INDEX))goto done;
 size_t used=0;for(;;){DWORD n=0;if(used>=capacity-1||!WinHttpReadData(request,response+used,(DWORD)(capacity-1-used),&n))goto done;if(!n)break;used+=n;}
 response[used]=0;ok=true;
done:DeleteTimerQueueTimer(NULL,timer,INVALID_HANDLE_VALUE);close_request(&timed,false);WinHttpCloseHandle(connection);return ok;
}
static void update(desk_home_state state,bool busy,const char *id){AcquireSRWLockExclusive(&gate);if(!id||!strcmp(id,pending.id)){result.state=state;result.busy=busy;result.updated_ms=GetTickCount64();}ReleaseSRWLockExclusive(&gate);}
static bool replaced(const char *id){AcquireSRWLockShared(&gate);bool changed=strcmp(id,pending.id)!=0;ReleaseSRWLockShared(&gate);return changed;}
static bool decode_result(const char *body,const home_request *expected){
 cJSON *root=cJSON_ParseWithOpts(body,NULL,true);if(!root)return false;
 const cJSON *id=cJSON_GetObjectItemCaseSensitive(root,"command_id"),*entity=cJSON_GetObjectItemCaseSensitive(root,"entity_id"),*action=cJSON_GetObjectItemCaseSensitive(root,"action"),*state=cJSON_GetObjectItemCaseSensitive(root,"status");
 bool matched=cJSON_IsString(id)&&cJSON_IsString(entity)&&cJSON_IsString(action)&&cJSON_IsString(state)&&!strcmp(id->valuestring,expected->id)&&!strcmp(entity->valuestring,expected->entity)&&!strcmp(action->valuestring,gso_home_action_name(expected->action));
 bool terminal=false;if(matched){
  if(!strcmp(state->valuestring,"superseded")){update(DESK_HOME_SUPERSEDED,false,expected->id);terminal=true;}
  else if(!strcmp(state->valuestring,"confirmed")){update(DESK_HOME_CONFIRMED,false,expected->id);terminal=true;}
  else if(!strcmp(state->valuestring,"rejected")){update(DESK_HOME_REJECTED,false,expected->id);terminal=true;}
  else if(!strcmp(state->valuestring,"timed_out")){update(DESK_HOME_TIMED_OUT,false,expected->id);terminal=true;}
  else if(!strcmp(state->valuestring,"sent"))update(DESK_HOME_SENT,true,expected->id);
  else if(!strcmp(state->valuestring,"queued"))update(DESK_HOME_QUEUED,true,expected->id);
 }
 cJSON_Delete(root);return terminal;
}
static DWORD WINAPI command_thread(void *unused){
 (void)unused;HANDLE events[]={stop_event,wake_event};
 while(WaitForMultipleObjects(2,events,FALSE,INFINITE)==WAIT_OBJECT_0+1){
  home_request current;AcquireSRWLockShared(&gate);current=pending;ReleaseSRWLockShared(&gate);
  if(GetTickCount64()-current.queued>2000){update(DESK_HOME_TIMED_OUT,false,current.id);continue;}
  char response[4096];DWORD status=0;update(DESK_HOME_SENT,true,current.id);
  bool received=exchange(L"POST",route,control_header,current.body,response,sizeof(response),&status);
  if(received&&status>=300&&status<500){update(DESK_HOME_REJECTED,false,current.id);continue;}
  if(received&&(status==200||status==202)&&decode_result(response,&current))continue;
  if(!received||status>=500)update(DESK_HOME_UNKNOWN,true,current.id);
  wchar_t address[560],wide_id[40];MultiByteToWideChar(CP_UTF8,0,current.id,-1,wide_id,40);swprintf(address,560,L"%ls/%ls",route,wide_id);
  bool terminal=false;
  while(GetTickCount64()-current.queued<(gso_home_action_slow(current.action)?32000u:12000u)&&WaitForSingleObject(stop_event,500)==WAIT_TIMEOUT){
   if(replaced(current.id)){terminal=true;break;}
   gso_snapshot latest;desk_data_latest(&latest);
   if(latest.home_epoch[0]&&strcmp(latest.home_epoch,current.epoch)){update(DESK_HOME_UNKNOWN,false,current.id);terminal=true;break;}
   status=0;if(exchange(L"GET",address,read_header,NULL,response,sizeof(response),&status)&&status==200&&decode_result(response,&current)){terminal=true;break;}
  }
  if(!terminal)update(DESK_HOME_UNKNOWN,false,current.id);
 }return 0;
}
bool desk_home_start(const wchar_t *host,unsigned short port,bool secure,const wchar_t *path,const wchar_t *read,const wchar_t *control){
 if(worker)return false;
 if(wcslen(host)>=256||wcslen(path)>=512||wcslen(read)>=512||wcslen(control)>=512)return false;
 wcscpy(server,host);wcscpy(route,path);wcscpy(read_header,read);wcscpy(control_header,control);server_port=port;tls=secure;memset(&result,0,sizeof(result));
 stop_event=CreateEventW(NULL,TRUE,FALSE,NULL);wake_event=CreateEventW(NULL,FALSE,FALSE,NULL);
 session=WinHttpOpen(L"GSO STATUS HOME",WINHTTP_ACCESS_TYPE_NO_PROXY,WINHTTP_NO_PROXY_NAME,WINHTTP_NO_PROXY_BYPASS,0);
 if(!stop_event||!wake_event||!session)goto failed;WinHttpSetTimeouts(session,800,800,800,800);
 worker=CreateThread(NULL,0,command_thread,NULL,0,NULL);if(worker)return true;
failed:if(stop_event)CloseHandle(stop_event);if(wake_event)CloseHandle(wake_event);if(session)WinHttpCloseHandle(session);stop_event=wake_event=NULL;session=NULL;return false;
}
void desk_home_stop(void){if(!worker)return;SetEvent(stop_event);WaitForSingleObject(worker,INFINITE);CloseHandle(worker);CloseHandle(stop_event);CloseHandle(wake_event);WinHttpCloseHandle(session);worker=NULL;stop_event=wake_event=NULL;session=NULL;SecureZeroMemory(control_header,sizeof(control_header));SecureZeroMemory(read_header,sizeof(read_header));update(DESK_HOME_NONE,false,NULL);}
bool desk_home_ready(void){return worker!=NULL;}
#ifdef GSO_TEST
bool desk_home_test_isolated(void){return worker&&!wcscmp(server,L"127.0.0.1")&&!wcscmp(read_header,L"Authorization: Bearer rrrrrrrrrrrrrrrrrrrrrrrrrrrrrrrr\r\n")&&!wcscmp(control_header,L"Authorization: Bearer cccccccccccccccccccccccccccccccc\r\n");}
#endif
desk_home_result desk_home_latest(void){AcquireSRWLockShared(&gate);desk_home_result copy=result;ReleaseSRWLockShared(&gate);return copy;}
bool desk_home_command(const char *id,unsigned action,gso_metric temperature){return desk_home_setting(id,action,temperature,NULL);}
bool desk_home_setting(const char *id,unsigned action,gso_metric temperature,const char *mode){
 if(!worker||!desk_data_connected()||!id||strlen(id)>160||!gso_home_action_name(action))return false;
 if(action==GSO_HOME_TEMPERATURE&&(!temperature.valid||!isfinite(temperature.value)||temperature.value< -100||temperature.value>200))return false;
 gso_snapshot model;desk_data_latest(&model);bool allowed=gso_snapshot_action_allowed(&model,id,action,GetTickCount64());
 if(!allowed||!model.home_epoch[0]||model.generated_at<=0)return false;
 unsigned char random[16];if(BCryptGenRandom(NULL,random,sizeof(random),BCRYPT_USE_SYSTEM_PREFERRED_RNG)!=0)return false;random[6]=(random[6]&15)|64;random[8]=(random[8]&63)|128;
 home_request request={0};snprintf(request.id,sizeof(request.id),"%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",random[0],random[1],random[2],random[3],random[4],random[5],random[6],random[7],random[8],random[9],random[10],random[11],random[12],random[13],random[14],random[15]);
 strcpy(request.epoch,model.home_epoch);strcpy(request.entity,id);request.action=action;request.queued=GetTickCount64();
 time_t expires=(time_t)(model.generated_at+(gso_home_action_slow(action)?28:10)+(request.queued>=model.received_ms?(request.queued-model.received_ms)/1000:0));struct tm *utc=gmtime(&expires);if(!utc)return false;char expiration[32];if(!strftime(expiration,sizeof(expiration),"%Y-%m-%dT%H:%M:%SZ",utc))return false;
 cJSON *body=cJSON_CreateObject();if(!body)return false;cJSON_AddStringToObject(body,"request_id",request.id);cJSON_AddStringToObject(body,"control_epoch",request.epoch);cJSON_AddStringToObject(body,"entity_id",id);cJSON_AddStringToObject(body,"action",gso_home_action_name(action));cJSON_AddStringToObject(body,"expires_at",expiration);const char *parameter=action==GSO_HOME_TEMPERATURE?"temperature_c":action==GSO_HOME_BRIGHTNESS?"brightness_percent":action==GSO_HOME_COLOR?"color_temperature_kelvin":action==GSO_HOME_SPEED?"percentage":action==GSO_HOME_HUMIDITY?"humidity_percent":NULL;if(parameter){if(!temperature.valid||!isfinite(temperature.value)){cJSON_Delete(body);return false;}cJSON_AddNumberToObject(body,parameter,temperature.value);}if(action==GSO_HOME_HVAC||action==GSO_HOME_PRESET||action==GSO_HOME_MODE){if(!mode||!mode[0]||strlen(mode)>64){cJSON_Delete(body);return false;}cJSON_AddStringToObject(body,"mode",mode);}
 char *encoded=cJSON_PrintUnformatted(body);cJSON_Delete(body);if(!encoded)return false;bool fits=strlen(encoded)<sizeof(request.body);if(fits)strcpy(request.body,encoded);cJSON_free(encoded);if(!fits)return false;
 AcquireSRWLockExclusive(&gate);if(result.busy&&!(!strcmp(result.entity_id,id)&&(result.action==GSO_HOME_OPEN||result.action==GSO_HOME_CLOSE)&&(action==GSO_HOME_STOP||((action==GSO_HOME_OPEN||action==GSO_HOME_CLOSE)&&action!=result.action)))){ReleaseSRWLockExclusive(&gate);return false;}pending=request;strcpy(result.entity_id,id);result.state=DESK_HOME_QUEUED;result.busy=true;result.action=action;result.updated_ms=request.queued;ReleaseSRWLockExclusive(&gate);SetEvent(wake_event);return true;
}
