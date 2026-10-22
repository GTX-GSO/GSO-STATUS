#define UNICODE
#define _UNICODE
#include <windows.h>
#include <winhttp.h>
#include <iphlpapi.h>
#include <stdio.h>
#include <string.h>
#include <wchar.h>
#include "data_source.h"
#include "home_client.h"
#include "clash_client.h"
#include "cJSON.h"
#include "gso_link.h"
#define GSO_REQUEST_TIMEOUT_MS 2000 /* LAN GETs take ~20 ms; 2 s tolerates a busy NAS without hiding a real outage */

static SRWLOCK lock=SRWLOCK_INIT;
static gso_snapshot latest;
static HANDLE worker,stop_event;
static volatile LONG connected,connection_status;
static desk_connection_status transport_failure(void){
 ULONG size=0;GetIfTable(NULL,&size,FALSE);if(!size||size>65536)return DESK_GSN_OFFLINE;
 PMIB_IFTABLE table=HeapAlloc(GetProcessHeap(),0,size);if(!table)return DESK_GSN_OFFLINE;
 bool up=false;DWORD result=GetIfTable(table,&size,FALSE);if(result==NO_ERROR)for(DWORD i=0;i<table->dwNumEntries;i++)if(table->table[i].dwType!=MIB_IF_TYPE_LOOPBACK&&table->table[i].dwOperStatus==MIB_IF_OPER_STATUS_OPERATIONAL){up=true;break;}
 HeapFree(GetProcessHeap(),0,table);return result==NO_ERROR&&!up?DESK_NETWORK_OFFLINE:DESK_GSN_OFFLINE;
}
static HINTERNET session;
static wchar_t thumb_prefix[640];   /* ".../api/v1/printer/thumbnail?id=" for the configured GSN */
static char thumb_key[17],thumb_attempt_key[17];
static uint8_t thumb_pixels[GSO_THUMBNAIL_BYTES];
static uint64_t thumb_attempt_ms;
static wchar_t host[256],path[512],authorization[512];
static INTERNET_PORT port;
static bool secure;
typedef struct {PVOID volatile handle;} deadline_request;
static VOID CALLBACK deadline_close(PVOID context,BOOLEAN fired){(void)fired;deadline_request *d=context;HINTERNET h=InterlockedExchangePointer(&d->handle,NULL);if(h)WinHttpCloseHandle(h);}

static bool fetch(gso_snapshot *model,int *failure){
    *failure=DESK_GSN_OFFLINE;
    bool ok=false;HINTERNET connection=WinHttpConnect(session,host,port,0);if(!connection)return false;
    HINTERNET request=WinHttpOpenRequest(connection,L"GET",path,NULL,WINHTTP_NO_REFERER,WINHTTP_DEFAULT_ACCEPT_TYPES,secure?WINHTTP_FLAG_SECURE:0);if(!request){WinHttpCloseHandle(connection);return false;}
    deadline_request deadline={request};HANDLE timer=NULL;if(!CreateTimerQueueTimer(&timer,NULL,deadline_close,&deadline,GSO_REQUEST_TIMEOUT_MS,0,WT_EXECUTEONLYONCE)){WinHttpCloseHandle(request);WinHttpCloseHandle(connection);return false;}
    DWORD redirect=WINHTTP_OPTION_REDIRECT_POLICY_NEVER;WinHttpSetOption(request,WINHTTP_OPTION_REDIRECT_POLICY,&redirect,sizeof(redirect));
    if(!WinHttpSendRequest(request,authorization,(DWORD)-1L,WINHTTP_NO_REQUEST_DATA,0,0,0)||!WinHttpReceiveResponse(request,NULL))goto done;
    DWORD status=0,size=sizeof(status);if(!WinHttpQueryHeaders(request,WINHTTP_QUERY_STATUS_CODE|WINHTTP_QUERY_FLAG_NUMBER,WINHTTP_HEADER_NAME_BY_INDEX,&status,&size,WINHTTP_NO_HEADER_INDEX) )goto done;
    if(status!=200){*failure=status==401||status==403?DESK_GSN_AUTH:DESK_GSN_RESPONSE;goto done;}
    *failure=DESK_GSN_RESPONSE;
    char buffer[GSO_SNAPSHOT_MAX_BYTES+1];size_t used=0;
    for(;;){DWORD n=0;if(!WinHttpReadData(request,buffer+used,(DWORD)(sizeof(buffer)-used),&n))goto done;if(!n)break;used+=n;if(used>GSO_SNAPSHOT_MAX_BYTES)goto done;}
    buffer[used]=0;ok=gso_snapshot_parse(buffer,used,GetTickCount64(),model);
done:DeleteTimerQueueTimer(NULL,timer,INVALID_HANDLE_VALUE);deadline_close(&deadline,false);WinHttpCloseHandle(connection);return ok;
}
/* The picture is raw bytes of an exact size; anything else (a 404, a short or long body) is a failure. */
static bool fetch_thumbnail(const char *key,uint8_t *out){
    wchar_t request_path[700];
    if(!thumb_prefix[0]||swprintf(request_path,700,L"%ls%hs",thumb_prefix,key)<=0)return false;
    bool ok=false;HINTERNET connection=WinHttpConnect(session,host,port,0);if(!connection)return false;
    HINTERNET request=WinHttpOpenRequest(connection,L"GET",request_path,NULL,WINHTTP_NO_REFERER,WINHTTP_DEFAULT_ACCEPT_TYPES,secure?WINHTTP_FLAG_SECURE:0);if(!request){WinHttpCloseHandle(connection);return false;}
    DWORD redirect=WINHTTP_OPTION_REDIRECT_POLICY_NEVER;WinHttpSetOption(request,WINHTTP_OPTION_REDIRECT_POLICY,&redirect,sizeof(redirect));
    if(!WinHttpSendRequest(request,authorization,(DWORD)-1L,WINHTTP_NO_REQUEST_DATA,0,0,0)||!WinHttpReceiveResponse(request,NULL))goto done;
    DWORD status=0,size=sizeof(status);if(!WinHttpQueryHeaders(request,WINHTTP_QUERY_STATUS_CODE|WINHTTP_QUERY_FLAG_NUMBER,WINHTTP_HEADER_NAME_BY_INDEX,&status,&size,WINHTTP_NO_HEADER_INDEX)||status!=200)goto done;
    size_t used=0;
    for(;;){DWORD n=0;if(!WinHttpReadData(request,out+used,(DWORD)(GSO_THUMBNAIL_BYTES+1-used),&n))goto done;if(!n)break;used+=n;if(used>GSO_THUMBNAIL_BYTES)goto done;}
    ok=used==GSO_THUMBNAIL_BYTES;
done:WinHttpCloseHandle(request);WinHttpCloseHandle(connection);return ok;
}
/* Follows the id the snapshot names for the current job: fetched once per id (a failed try waits ten seconds), dropped when the job has no picture. */
static void update_thumbnail(const gso_snapshot *m){
    const char *key=m->print.reported?m->print.thumbnail:"";
    if(!key[0]){if(thumb_key[0]){AcquireSRWLockExclusive(&lock);thumb_key[0]=0;ReleaseSRWLockExclusive(&lock);}return;}
    if(!strcmp(key,thumb_key))return;
    uint64_t now=GetTickCount64();
    if(!strcmp(key,thumb_attempt_key)&&now-thumb_attempt_ms<10000)return;
    snprintf(thumb_attempt_key,sizeof(thumb_attempt_key),"%s",key);thumb_attempt_ms=now;
    static uint8_t incoming[GSO_THUMBNAIL_BYTES+1];
    if(!fetch_thumbnail(key,incoming))return;
    AcquireSRWLockExclusive(&lock);memcpy(thumb_pixels,incoming,GSO_THUMBNAIL_BYTES);snprintf(thumb_key,sizeof(thumb_key),"%s",key);ReleaseSRWLockExclusive(&lock);
}
bool desk_data_thumbnail(const char *key,uint8_t *pixels){
    bool found=false;
    AcquireSRWLockShared(&lock);
    if(key&&key[0]&&!strcmp(key,thumb_key)){memcpy(pixels,thumb_pixels,GSO_THUMBNAIL_BYTES);found=true;}
    ReleaseSRWLockShared(&lock);
    return found;
}
static DWORD WINAPI poll_thread(void *arg){
    (void)arg;DWORD delay=1000;gso_link link;gso_link_init(&link,GetTickCount64(),DESK_CONNECTING);
    while(WaitForSingleObject(stop_event,0)!=WAIT_OBJECT_0){
        gso_snapshot model;uint64_t start=GetTickCount64();int failure=DESK_GSN_OFFLINE;bool ok=fetch(&model,&failure);
        if(ok){AcquireSRWLockExclusive(&lock);latest=model;ReleaseSRWLockExclusive(&lock);update_thumbnail(&model);InterlockedExchange(&connection_status,gso_link_update(&link,GetTickCount64(),true,0,false,DESK_ONLINE));InterlockedExchange(&connected,1);delay=1000;}
        else{
            /* Debounced: a single slow or lost poll keeps the last status; only certain failures show at once. */
            if(failure==DESK_GSN_OFFLINE)failure=transport_failure();
            InterlockedExchange(&connection_status,gso_link_update(&link,GetTickCount64(),false,failure,failure==DESK_GSN_AUTH||failure==DESK_NETWORK_OFFLINE,DESK_ONLINE));
            InterlockedExchange(&connected,0); /* controls follow the real link at once; only the status label is debounced */
        }
        uint64_t elapsed=GetTickCount64()-start;DWORD wait=elapsed<delay?delay-(DWORD)elapsed:1;if(WaitForSingleObject(stop_event,wait)==WAIT_OBJECT_0)break;if(!ok&&delay<4000)delay*=2;
    }return 0;
}
bool desk_data_start(const char *config){
    if(worker)return false;
    gso_snapshot_empty(&latest);
    FILE *f=fopen(config,"rb");if(!f)return false;char raw[2049];size_t size=fread(raw,1,2049,f);fclose(f);if(size>=2049)return false;raw[size]=0;
    cJSON *json=cJSON_Parse(raw);if(!json)return false;
    const cJSON *url=cJSON_GetObjectItemCaseSensitive(json,"url"),*token=cJSON_GetObjectItemCaseSensitive(json,"read_token"),*control=cJSON_GetObjectItemCaseSensitive(json,"home_control_token");bool valid=cJSON_IsString(url)&&cJSON_IsString(token)&&strlen(token->valuestring)>=32&&strlen(token->valuestring)<300&&strpbrk(token->valuestring,"\r\n")==NULL;
    wchar_t home_authorization[512]={0};
    if(valid&&cJSON_IsString(control)&&control->valuestring[0]){bool home_valid=strlen(control->valuestring)>=32&&strlen(control->valuestring)<=256&&!strpbrk(control->valuestring,"\r\n")&&strcmp(control->valuestring,token->valuestring);if(home_valid){wchar_t secret[320];if(MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,control->valuestring,-1,secret,320)>0)swprintf(home_authorization,512,L"Authorization: Bearer %ls\r\n",secret);}}
    wchar_t wide[1024];if(valid)valid=MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,url->valuestring,-1,wide,1024)>0;
    URL_COMPONENTS parts={0};parts.dwStructSize=sizeof(parts);parts.lpszHostName=host;parts.dwHostNameLength=256;parts.lpszUrlPath=path;parts.dwUrlPathLength=512;parts.dwUserNameLength=(DWORD)-1;parts.dwPasswordLength=(DWORD)-1;parts.dwExtraInfoLength=(DWORD)-1;
    if(valid)valid=WinHttpCrackUrl(wide,0,0,&parts)&&parts.dwHostNameLength>0&&parts.dwUserNameLength==0&&parts.dwPasswordLength==0&&parts.dwExtraInfoLength==0&&(parts.nScheme==INTERNET_SCHEME_HTTP||parts.nScheme==INTERNET_SCHEME_HTTPS);
    if(valid){wchar_t secret[320];valid=MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,token->valuestring,-1,secret,320)>0;if(valid)swprintf(authorization,512,L"Authorization: Bearer %ls\r\n",secret);port=parts.nPort;secure=parts.nScheme==INTERNET_SCHEME_HTTPS;}
    if(valid){const cJSON *mihomo=cJSON_GetObjectItemCaseSensitive(json,"mihomo_url"),*secret=cJSON_GetObjectItemCaseSensitive(json,"mihomo_token");if(cJSON_IsString(mihomo)&&cJSON_IsString(secret))desk_clash_start(mihomo->valuestring,secret->valuestring);}
    cJSON_Delete(json);if(!valid)return false;
    stop_event=CreateEventW(NULL,TRUE,FALSE,NULL);if(!stop_event)return false;
    session=WinHttpOpen(L"GSO STATUS/0.5",WINHTTP_ACCESS_TYPE_NO_PROXY,WINHTTP_NO_PROXY_NAME,WINHTTP_NO_PROXY_BYPASS,0);if(!session){CloseHandle(stop_event);stop_event=NULL;return false;}
    WinHttpSetTimeouts(session,GSO_REQUEST_TIMEOUT_MS,GSO_REQUEST_TIMEOUT_MS,GSO_REQUEST_TIMEOUT_MS,GSO_REQUEST_TIMEOUT_MS);
    worker=CreateThread(NULL,0,poll_thread,NULL,0,NULL);if(!worker){WinHttpCloseHandle(session);CloseHandle(stop_event);return false;}
    const wchar_t *suffix=L"/api/v1/snapshot";size_t count=wcslen(path),tail=wcslen(suffix);if(home_authorization[0]&&count>=tail&&!wcscmp(path+count-tail,suffix)){wchar_t home_route[512];int n=swprintf(home_route,512,L"%.*ls/api/v1/home/commands",(int)(count-tail),path);if(n>0&&n<512)desk_home_start(host,port,secure,home_route,authorization,home_authorization);}
    {const wchar_t *tail_text=L"/api/v1/snapshot";size_t full=wcslen(path),tail_length=wcslen(tail_text);if(full>=tail_length&&!wcscmp(path+full-tail_length,tail_text))swprintf(thumb_prefix,640,L"%.*ls/api/v1/printer/thumbnail?id=",(int)(full-tail_length),path);}
    SecureZeroMemory(home_authorization,sizeof(home_authorization));return true;
}
void desk_data_stop(void){desk_clash_stop();desk_home_stop();if(!worker)return;SetEvent(stop_event);WaitForSingleObject(worker,INFINITE);CloseHandle(worker);CloseHandle(stop_event);WinHttpCloseHandle(session);worker=NULL;}
bool desk_data_latest(gso_snapshot *out){AcquireSRWLockShared(&lock);*out=latest;ReleaseSRWLockShared(&lock);return out->windows.received||out->wan.received||out->home.received;}
bool desk_data_connected(void){return InterlockedCompareExchange(&connected,0,0)!=0;}
uint64_t desk_data_now_ms(void){return GetTickCount64();}

desk_connection_status desk_data_status(void){return (desk_connection_status)InterlockedCompareExchange(&connection_status,0,0);}

#ifdef GSO_TEST
void desk_data_test_snapshot(const gso_snapshot *snapshot){AcquireSRWLockExclusive(&lock);latest=*snapshot;ReleaseSRWLockExclusive(&lock);desk_data_test_status(DESK_ONLINE);}
void desk_data_test_thumbnail(const char *key,const uint8_t *pixels){AcquireSRWLockExclusive(&lock);memcpy(thumb_pixels,pixels,GSO_THUMBNAIL_BYTES);snprintf(thumb_key,sizeof(thumb_key),"%s",key);ReleaseSRWLockExclusive(&lock);}
void desk_data_test_status(desk_connection_status status){InterlockedExchange(&connection_status,status);InterlockedExchange(&connected,status==DESK_ONLINE);}
#endif
