#include "gso_snapshot.h"
#include "cJSON.h"
#include <string.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>

static const cJSON *get(const cJSON *o,const char *k){return cJSON_GetObjectItemCaseSensitive(o,k);}
static bool str(const cJSON *v,char *out,size_t cap){if(!cJSON_IsString(v)||strlen(v->valuestring)>=cap)return false;strcpy(out,v->valuestring);return true;}
static bool metric(const cJSON *o,gso_metric *out){
    if(!cJSON_IsObject(o))return false;
    const cJSON *v=get(o,"value"),*reason=get(o,"reason");
    if(cJSON_IsNull(v)&&cJSON_IsString(reason)&&reason->valuestring[0]){
        /* The UI tells a sleeping or still warming-up NAS metric from a broken one. */
        static const char *const known[]={"disk_standby","disk_idle","disk_unavailable","warming_up","not_configured"};
        const char *code="source_unavailable";
        for(unsigned i=0;i<sizeof(known)/sizeof(known[0]);i++)if(!strcmp(reason->valuestring,known[i]))code=known[i];
        *out=gso_metric_missing(code);return true;
    }
    if(!cJSON_IsNumber(v)||!isfinite(v->valuedouble)||!cJSON_IsNull(reason))return false;
    *out=gso_metric_number(v->valuedouble);return true;
}
static bool nonnegative(gso_metric m){return !m.valid||m.value>=0;}
static bool capacity(gso_metric m){return !m.valid||(m.value>=0&&m.value<=9007199254740991.0&&floor(m.value)==m.value);}
static bool source(const cJSON *o,uint64_t now,gso_freshness *out){
    if(!cJSON_IsObject(o))return false;
    const cJSON *q=get(o,"quality"),*age=get(o,"age_ms");if(!cJSON_IsString(q))return false;
    const char *names[]={"ok","stale","offline","unsupported","unconfigured","error"};int quality=-1;for(int i=0;i<6;i++)if(!strcmp(q->valuestring,names[i]))quality=i;if(quality<0)return false;
    if(!cJSON_IsNull(age)&&(!cJSON_IsNumber(age)||age->valuedouble<0||age->valuedouble>9007199254740991.0||floor(age->valuedouble)!=age->valuedouble))return false;
    if(quality<=GSO_OFFLINE&&(!cJSON_IsNumber(age)||!cJSON_IsString(get(o,"observed_at"))))return false;
    *out=(gso_freshness){(gso_quality)quality,cJSON_IsNumber(age),now,cJSON_IsNumber(age)?(uint64_t)age->valuedouble:0};return true;
}
static bool utf8_valid(const unsigned char *s,size_t n){
    for(size_t i=0;i<n;){unsigned char a=s[i++];if(a<128)continue;unsigned need;uint32_t code;if(a>=0xC2&&a<=0xDF){need=1;code=a&31;}else if(a>=0xE0&&a<=0xEF){need=2;code=a&15;}else if(a>=0xF0&&a<=0xF4){need=3;code=a&7;}else return false;if(i+need>n)return false;for(unsigned j=0;j<need;j++){unsigned char b=s[i++];if((b&0xC0)!=0x80)return false;code=(code<<6)|(b&63);}if((need==1&&code<128)||(need==2&&code<2048)||(need==3&&code<65536)||code>0x10FFFF||(code>=0xD800&&code<=0xDFFF))return false;}return true;
}
static int64_t timestamp(const char *s){
    int y,m,d,h,min,sec,n=0;if(sscanf(s,"%d-%d-%dT%d:%d:%d%n",&y,&m,&d,&h,&min,&sec,&n)!=6||y<1970||y>9999||m<1||m>12||d<1||d>31||h>23||min>59||sec>59)return 0;
    const char *zone=s+n;if(*zone=='.'){zone++;while(*zone>='0'&&*zone<='9')zone++;}int offset=0;if(*zone=='Z'&&zone[1]==0){}else{int oh,om,used=0;char sign=*zone;if((sign!='+'&&sign!='-')||sscanf(zone+1,"%d:%d%n",&oh,&om,&used)!=2||zone[used+1]!=0||oh>23||om>59)return 0;offset=(oh*60+om)*60*(sign=='+'?1:-1);}
    y-=m<=2;int era=y/400;unsigned year=(unsigned)(y-era*400),day=(153*(m+(m>2?-3:9))+2)/5+d-1,days=year*365+year/4-year/100+day;int64_t civil=(int64_t)era*146097+days-719468;return civil*86400+h*3600+min*60+sec-offset;
}
static bool hardware(const cJSON *o,gso_hardware *out){
    if(!cJSON_IsObject(o))return false;
    const cJSON *name=get(o,"model");if(cJSON_IsNull(name))strcpy(out->model,"Unavailable");else if(!str(name,out->model,sizeof(out->model)))return false;
    if(!metric(get(o,"load_percent"),&out->load)||!metric(get(o,"temperature_c"),&out->temperature)||!metric(get(o,"power_w"),&out->power)||!metric(get(o,"memory_used_bytes"),&out->used)||!metric(get(o,"memory_total_bytes"),&out->total))return false;
    const char *keys[]={"clock_mhz","memory_clock_mhz","memory_rate_mts"};gso_metric *clocks[]={&out->clock,&out->memory_clock,&out->memory_rate};for(int i=0;i<3;i++){const cJSON *m=get(o,keys[i]);*clocks[i]=gso_metric_missing("clock_unavailable");if(m&&(!metric(m,clocks[i])||!nonnegative(*clocks[i])||(clocks[i]->valid&&clocks[i]->value>100000)))return false;}
    const cJSON *clock_source=get(o,"memory_clock_source");out->memory_clock_configured=cJSON_IsString(clock_source)&&!strcmp(clock_source->valuestring,"configured_ddr");
    return (!out->load.valid||gso_metric_percent_valid(out->load))&&(!out->temperature.valid||(out->temperature.value>=-100&&out->temperature.value<=200))&&nonnegative(out->power)&&capacity(out->used)&&capacity(out->total)&&(!out->used.valid||!out->total.valid||out->used.value<=out->total.value);
}
static bool bounded_tree(const cJSON *o,unsigned depth,unsigned *nodes){
    if(depth>32||++*nodes>4096)return false;
    unsigned children=0;const cJSON *a;for(a=o->child;a;a=a->next){if(cJSON_IsObject(o)){if(++children>64||!a->string)return false;for(const cJSON *b=a->next;b;b=b->next)if(b->string&&!strcmp(a->string,b->string))return false;}if(!bounded_tree(a,depth+1,nodes))return false;}
    return true;
}
const char *gso_home_action_name(unsigned action){switch(action){case GSO_HOME_ON:return "turn_on";case GSO_HOME_OFF:return "turn_off";case GSO_HOME_TEMPERATURE:return "set_temperature";case GSO_HOME_OPEN:return "open_cover";case GSO_HOME_CLOSE:return "close_cover";case GSO_HOME_STOP:return "stop_cover";case GSO_HOME_HVAC:return "set_hvac_mode";case GSO_HOME_BRIGHTNESS:return "set_brightness";case GSO_HOME_COLOR:return "set_color_temperature";case GSO_HOME_SPEED:return "set_percentage";case GSO_HOME_PRESET:return "set_preset_mode";case GSO_HOME_MODE:return "set_mode";case GSO_HOME_HUMIDITY:return "set_humidity";case GSO_HOME_LOCK:return "lock";case GSO_HOME_UNLOCK:return "unlock";case GSO_HOME_PRESS:return "press";default:return NULL;}}
bool gso_home_action_slow(unsigned action){return action==GSO_HOME_OPEN||action==GSO_HOME_CLOSE||action==GSO_HOME_LOCK||action==GSO_HOME_UNLOCK;}
bool gso_home_action_allowed(const gso_home_entity *entity,unsigned action,uint64_t now){return entity&&gso_home_action_name(action)&&(entity->actions&action)&&gso_source_quality(entity->source,now,45000,120000)==GSO_OK;}
bool gso_snapshot_action_allowed(const gso_snapshot *m,const char *id,unsigned action,uint64_t now){
    if(!m||!id)return false;
    for(unsigned i=0;i<m->home_count;i++)if(!strcmp(m->home_entities[i].id,id))return gso_home_action_allowed(&m->home_entities[i],action,now);
    const gso_car *c=&m->car;
    if(!c->reported||!(c->actions&action)||gso_source_quality(m->car_source,now,45000,120000)!=GSO_OK)return false;
    const char *target=action==GSO_HOME_PRESS?c->refresh_id:(action==GSO_HOME_LOCK||action==GSO_HOME_UNLOCK)?c->lock_id:"";
    return target[0]&&!strcmp(id,target);
}
static bool word(const cJSON *v,const char *const *names,unsigned count,unsigned *out){
    if(!cJSON_IsString(v))return false;
    for(unsigned i=0;i<count;i++)if(!strcmp(v->valuestring,names[i])){*out=i;return true;}
    return false;
}
static bool epoch_valid(const char *s){if(strlen(s)!=36)return false;for(int i=0;i<36;i++){if(i==8||i==13||i==18||i==23){if(s[i]!='-')return false;}else if(!((s[i]>='0'&&s[i]<='9')||(s[i]>='a'&&s[i]<='f')||(s[i]>='A'&&s[i]<='F')))return false;}return true;}
static bool home_model(const cJSON *o,uint64_t now,gso_snapshot *m){
    if(!source(get(o,"source"),now,&m->home))return false;
    const cJSON *epoch=get(o,"control_epoch");if(epoch&&!cJSON_IsNull(epoch)&&(!str(epoch,m->home_epoch,sizeof(m->home_epoch))||!epoch_valid(m->home_epoch)))return false;
    const cJSON *entities=get(o,"entities"),*item;if(!cJSON_IsArray(entities)||cJSON_GetArraySize(entities)>(int)GSO_MAX_HOME_ENTITIES)return false;
    cJSON_ArrayForEach(item,entities){gso_home_entity *e=&m->home_entities[m->home_count];
        if(!str(get(item,"entity_id"),e->id,sizeof(e->id))||!e->id[0]||!str(get(item,"name"),e->name,sizeof(e->name))||!source(get(item,"source"),now,&e->source))return false;
        for(unsigned i=0;i<m->home_count;i++)if(!strcmp(e->id,m->home_entities[i].id))return false;
        const cJSON *state=get(item,"state");if(!cJSON_IsNull(state)&&!str(state,e->state,sizeof(e->state)))return false;
        if(!metric(get(item,"current_temperature_c"),&e->current_temperature)||!metric(get(item,"target_temperature_c"),&e->target_temperature))return false;
        if((e->current_temperature.valid&&(e->current_temperature.value< -100||e->current_temperature.value>200))||(e->target_temperature.valid&&(e->target_temperature.value< -100||e->target_temperature.value>200)))return false;
        e->min_temperature=e->max_temperature=e->temperature_step=gso_metric_missing("temperature_limits_unavailable");
        const cJSON *minimum=get(item,"min_temperature_c"),*maximum=get(item,"max_temperature_c"),*step=get(item,"temperature_step_c");
        if((minimum&&!metric(minimum,&e->min_temperature))||(maximum&&!metric(maximum,&e->max_temperature))||(step&&!metric(step,&e->temperature_step)))return false;
        if((e->min_temperature.valid&&(e->min_temperature.value< -100||e->min_temperature.value>200))||(e->max_temperature.valid&&(e->max_temperature.value< -100||e->max_temperature.value>200))||(e->temperature_step.valid&&(e->temperature_step.value<=0||e->temperature_step.value>100))||(e->min_temperature.valid&&e->max_temperature.valid&&e->min_temperature.value>e->max_temperature.value))return false;
        const cJSON *actions=get(item,"actions"),*a;if(!cJSON_IsArray(actions)||cJSON_GetArraySize(actions)>13)return false;
        cJSON_ArrayForEach(a,actions){if(!cJSON_IsString(a))return false;unsigned flag=0;for(unsigned bit=1;bit<=4096;bit<<=1)if(!strcmp(a->valuestring,gso_home_action_name(bit)))flag=bit;if(!flag||(e->actions&flag))return false;e->actions|=flag;}
        const char *keys[]={"brightness_percent","color_temperature_kelvin","min_color_temperature_kelvin","max_color_temperature_kelvin","percentage","percentage_step","humidity_percent","min_humidity_percent","max_humidity_percent","remaining_minutes"};gso_metric *values[]={&e->brightness,&e->kelvin,&e->min_kelvin,&e->max_kelvin,&e->percentage,&e->percentage_step,&e->humidity,&e->min_humidity,&e->max_humidity,&e->remaining};
        for(int k=0;k<10;k++){const cJSON *v=get(item,keys[k]);*values[k]=gso_metric_missing("not_supported");if(v&&(!metric(v,values[k])||!nonnegative(*values[k])||(values[k]->valid&&values[k]->value>(k>=1&&k<=3?20000:k==9?100000:100))))return false;}
        const cJSON *mode=get(item,"mode"),*modes=get(item,"modes"),*extra=get(item,"detail_state");if(mode&&!cJSON_IsNull(mode)&&!str(mode,e->mode,sizeof(e->mode)))return false;if(extra&&!cJSON_IsNull(extra)&&!str(extra,e->detail_state,sizeof(e->detail_state)))return false;
        if(modes){if(!cJSON_IsArray(modes)||cJSON_GetArraySize(modes)>8)return false;const cJSON *entry;cJSON_ArrayForEach(entry,modes){if(!str(entry,e->modes[e->mode_count++],sizeof(e->modes[0])))return false;}}
        m->home_count++;
    }return true;
}
const char *gso_vibe_provider_name(unsigned provider){return provider==GSO_VIBE_CLAUDE?"Claude":provider==GSO_VIBE_CODEX?"Codex":"";}
unsigned gso_vibe_session_provider(const char *id){return id&&!strncmp(id,"claude-",7)?GSO_VIBE_CLAUDE:GSO_VIBE_CODEX;}
void gso_snapshot_empty(gso_snapshot *m){
    memset(m,0,sizeof(*m));m->windows.quality=m->wan.quality=m->codex.quality=m->game.quality=m->home.quality=m->storage.quality=m->printer.quality=m->nas_source.quality=GSO_UNCONFIGURED;
    gso_hardware *h[]={&m->cpu,&m->gpu};for(int i=0;i<2;i++){strcpy(h[i]->model,"Unavailable");h[i]->load=h[i]->temperature=h[i]->power=h[i]->used=h[i]->total=gso_metric_missing("not_connected");}
    m->download=m->upload=m->fps=m->latency=gso_metric_missing("not_connected");
    m->ambient.quality=GSO_UNCONFIGURED;m->illuminance=gso_metric_missing("not_connected");
    m->vpn.quality=m->weather.quality=GSO_UNCONFIGURED;m->vpn_remaining=m->vpn_total=m->weather_temperature=gso_metric_missing("not_connected");for(int i=0;i<4;i++)m->services[i].source.quality=GSO_UNCONFIGURED;
    for(int p=0;p<GSO_VIBE_PROVIDERS;p++)for(int i=0;i<2;i++)m->quota_remaining[p][i]=gso_metric_missing("quota_not_connected");
    for(int p=0;p<GSO_VIBE_PROVIDERS;p++)m->clients[p].tokens_today=gso_metric_missing("not_reported");
    gso_metric *printer_metrics[]={&m->print.progress,&m->print.remaining_minutes,&m->print.layer,&m->print.layer_total,&m->print.nozzle,&m->print.nozzle_target,&m->print.bed,&m->print.bed_target,&m->print.error_code};
    for(unsigned i=0;i<sizeof(printer_metrics)/sizeof(printer_metrics[0]);i++)*printer_metrics[i]=gso_metric_missing("not_reported");
    gso_metric *car_metrics[]={&m->car.battery,&m->car.range,&m->car.remaining_minutes,&m->car.cabin,&m->car.odometer,&m->car.week,&m->car.consumption};
    for(unsigned i=0;i<sizeof(car_metrics)/sizeof(car_metrics[0]);i++)*car_metrics[i]=gso_metric_missing("not_reported");
    for(unsigned i=0;i<4;i++)m->car.tyre[i]=gso_metric_missing("not_reported");
    m->car.charging_power=gso_metric_missing("not_reported");
    m->car_source.quality=GSO_UNCONFIGURED;
    strcpy(m->task_state,"unknown");m->task_duration_ms=gso_metric_missing("not_connected");m->vibe_provider=m->task_provider=GSO_VIBE_NONE;
}
bool gso_snapshot_parse(const char *json,size_t length,uint64_t now,gso_snapshot *out){
    if(!json||!out||!gso_snapshot_size_allowed(length)||memchr(json,0,length)||!utf8_valid((const unsigned char *)json,length))return false;
    for(size_t i=0;i<length;i++)if(json[i]=='\\'){if(i+5<length&&!memcmp(json+i,"\\u0000",6))return false;i++;}
    const char *end=NULL;cJSON *root=cJSON_ParseWithLengthOpts(json,length,&end,false);if(!root)return false;
    bool valid=false;gso_snapshot *m=malloc(sizeof(*m));if(!m){cJSON_Delete(root);return false;}gso_snapshot_empty(m);
    while(end<json+length&&(*end==' '||*end=='\r'||*end=='\n'||*end=='\t'))end++;
    unsigned nodes=0;if(end!=json+length||!bounded_tree(root,0,&nodes))goto done;
    const cJSON *version=get(root,"schema_version");if(!cJSON_IsString(version)||strcmp(version->valuestring,"1.0"))goto done;
    const cJSON *win=get(root,"windows"),*net=get(root,"net"),*wan=get(net,"wan"),*game=get(root,"game");
    if(!cJSON_IsObject(get(root,"home"))||!cJSON_IsObject(get(root,"weather"))||!cJSON_IsObject(get(root,"sun"))||!cJSON_IsArray(get(root,"alerts")))goto done;
    if(!source(get(win,"source"),now,&m->windows)||!source(get(wan,"source"),now,&m->wan)||!source(get(game,"source"),now,&m->game)||!source(get(get(root,"codex"),"source"),now,&m->codex))goto done;
    if(!hardware(get(win,"cpu"),&m->cpu)||!hardware(get(win,"gpu"),&m->gpu))goto done;
    const cJSON *disks=get(win,"disks");if(!cJSON_IsArray(disks)||cJSON_GetArraySize(disks)>3)goto done;
    const cJSON *d;cJSON_ArrayForEach(d,disks){gso_disk *disk=&m->disks[m->disk_count++];if(!str(get(d,"id"),disk->id,sizeof(disk->id)))goto done;for(unsigned i=0;i+1<m->disk_count;i++)if(!strcmp(m->disks[i].id,disk->id))goto done;
        if(cJSON_IsNull(get(d,"model")))strcpy(disk->model,"Unavailable");else if(!str(get(d,"model"),disk->model,sizeof(disk->model)))goto done;
        if(!metric(get(d,"total_bytes"),&disk->total)||!metric(get(d,"free_bytes"),&disk->free)||!capacity(disk->total)||!capacity(disk->free)||(disk->free.valid&&disk->total.valid&&disk->free.value>disk->total.value))goto done;
        /* Optional activity metrics (GSW 0.1.17+); an older GSW leaves them out and the page shows "-". */
        disk->busy=disk->read_ms=disk->write_ms=disk->response_ms=disk->read_rate=disk->write_rate=gso_metric_missing("source_unavailable");
        if(get(d,"busy_percent")&&(!metric(get(d,"busy_percent"),&disk->busy)||(disk->busy.valid&&!gso_metric_percent_valid(disk->busy))))goto done;
        if(get(d,"read_ms")&&!metric(get(d,"read_ms"),&disk->read_ms))goto done;
        if(get(d,"write_ms")&&!metric(get(d,"write_ms"),&disk->write_ms))goto done;
        /* GSW 0.1.18+: overall response time and the byte rates of the last interval. */
        if(get(d,"response_ms")&&(!metric(get(d,"response_ms"),&disk->response_ms)||!nonnegative(disk->response_ms)))goto done;
        if(get(d,"read_bytes_per_second")&&(!metric(get(d,"read_bytes_per_second"),&disk->read_rate)||!nonnegative(disk->read_rate)))goto done;
        if(get(d,"write_bytes_per_second")&&(!metric(get(d,"write_bytes_per_second"),&disk->write_rate)||!nonnegative(disk->write_rate)))goto done;
    }
    const cJSON *wan_error=get(get(wan,"source"),"last_error_code"),*game_error=get(get(game,"source"),"last_error_code");
    if(!cJSON_IsNull(wan_error)&&!str(wan_error,m->wan_error,sizeof(m->wan_error)))goto done;
    if(!cJSON_IsNull(game_error)&&!str(game_error,m->game_error,sizeof(m->game_error)))goto done;
    if(!metric(get(wan,"download_bytes_per_second"),&m->download)||!metric(get(wan,"upload_bytes_per_second"),&m->upload)||!nonnegative(m->download)||!nonnegative(m->upload)||!metric(get(game,"fps"),&m->fps)||!metric(get(game,"render_latency_ms"),&m->latency)||!nonnegative(m->fps)||!nonnegative(m->latency))goto done;
    if(!cJSON_IsNull(get(game,"process_name"))&&!str(get(game,"process_name"),m->game_process,sizeof(m->game_process)))goto done;
    const cJSON *icon=get(game,"icon_argb8888");
    if(icon&&!cJSON_IsNull(icon)){
        if(!cJSON_IsString(icon)||strlen(icon->valuestring)!=3072)goto done;
        const char *alphabet="ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";unsigned acc=0,bits=0,n=0;
        for(unsigned i=0;i<3072;i++){const char *at=strchr(alphabet,icon->valuestring[i]);if(!at)goto done;acc=(acc<<6)|(unsigned)(at-alphabet);bits+=6;if(bits>=8){bits-=8;m->game_icon[n++]=(uint8_t)(acc>>bits);}}
        m->game_icon_valid=n==sizeof(m->game_icon);if(!m->game_icon_valid)goto done;
    }
    const cJSON *render=get(game,"rendering");if(!cJSON_IsBool(render)&&!cJSON_IsNull(render))goto done;m->rendering=cJSON_IsTrue(render);
    const cJSON *history=get(game,"fps_history");if(!cJSON_IsArray(history)||cJSON_GetArraySize(history)>(int)GSO_GAME_HISTORY_POINTS)goto done;const cJSON *point;cJSON_ArrayForEach(point,history){if(!metric(point,&m->fps_history[m->fps_history_count])||!nonnegative(m->fps_history[m->fps_history_count]))goto done;m->fps_history_count++;}
    const char *roles[]={"proxy","dhcp","clash","antiad"};const cJSON *services=get(net,"services");for(int i=0;i<4;i++){const cJSON *service=get(services,roles[i]);if(!source(get(service,"source"),now,&m->services[i].source))goto done;const cJSON *raw=get(service,"raw_state");if(!cJSON_IsNull(raw)&&!str(raw,m->services[i].raw_state,sizeof(m->services[i].raw_state)))goto done;}
    const cJSON *details=get(net,"detail_sources");const char *detail_roles[]={"subscription_updated","antiad_rules_updated","antiad_rule_count","dhcp_active_leases"};
    for(int i=0;i<4;i++){m->net_details[i]=i==0?m->vpn:i==3?m->services[1].source:m->services[3].source;if(details&&!source(get(details,detail_roles[i]),now,&m->net_details[i]))goto done;if(!details&&i==0&&!source(get(get(net,"vpn"),"source"),now,&m->net_details[i]))goto done;}
    const char *counter_keys[]={"antiad_rule_count","dhcp_active_leases"};gso_metric *counters[]={&m->antiad_count,&m->dhcp_leases};
    for(int i=0;i<2;i++){const cJSON *v=get(net,counter_keys[i]);*counters[i]=gso_metric_missing("not_connected");if(v&&(!metric(v,counters[i])||!capacity(*counters[i])||(counters[i]->valid&&floor(counters[i]->value)!=counters[i]->value)))goto done;}
    const char *date_keys[]={"subscription_updated_at","antiad_rules_updated_at"};int64_t *date_values[]={&m->subscription_updated,&m->antiad_updated};
    for(int i=0;i<2;i++){const cJSON *v=get(net,date_keys[i]);if(v&&!cJSON_IsNull(v)&&(!cJSON_IsString(v)||!(*date_values[i]=timestamp(v->valuestring))))goto done;}
    const cJSON *ambient=get(root,"ambient");if(ambient&&(!source(get(ambient,"source"),now,&m->ambient)||!metric(get(ambient,"illuminance_lux"),&m->illuminance)||!nonnegative(m->illuminance)||(m->illuminance.valid&&m->illuminance.value>200000)))goto done;
    const cJSON *vpn=get(net,"vpn"),*weather=get(root,"weather");if(!source(get(vpn,"source"),now,&m->vpn)||!metric(get(vpn,"remaining_bytes"),&m->vpn_remaining)||!metric(get(vpn,"total_bytes"),&m->vpn_total)||!capacity(m->vpn_remaining)||!capacity(m->vpn_total))goto done;
    if(!cJSON_IsNull(get(vpn,"total_origin"))&&!str(get(vpn,"total_origin"),m->vpn_total_origin,sizeof(m->vpn_total_origin)))goto done;
    if(!source(get(weather,"source"),now,&m->weather)||!metric(get(weather,"temperature_c"),&m->weather_temperature))goto done;
    const char *weather_keys[]={"humidity_percent","pressure_hpa","wind_speed_kmh","wind_bearing_degrees"};gso_metric *weather_metrics[]={&m->humidity,&m->pressure,&m->wind,&m->wind_bearing};for(int i=0;i<4;i++){const cJSON *v=get(weather,weather_keys[i]);*weather_metrics[i]=gso_metric_missing("weather_metric_unavailable");if(v&&(!metric(v,weather_metrics[i])||!nonnegative(*weather_metrics[i])))goto done;}
    if((m->humidity.valid&&m->humidity.value>100)||(m->pressure.valid&&m->pressure.value>2000)||(m->wind.valid&&m->wind.value>1000)||(m->wind_bearing.valid&&m->wind_bearing.value>360))goto done;
    const cJSON *forecast=get(weather,"forecast");if(forecast){if(!cJSON_IsArray(forecast)||cJSON_GetArraySize(forecast)>6)goto done;const cJSON *day;cJSON_ArrayForEach(day,forecast){gso_forecast *f=&m->forecast[m->forecast_count++];const cJSON *date=get(day,"datetime"),*cond=get(day,"condition");if(!cJSON_IsString(date)||!(f->date=timestamp(date->valuestring))||(!cJSON_IsNull(cond)&&!str(cond,f->condition,sizeof(f->condition)))||!metric(get(day,"high_c"),&f->high)||!metric(get(day,"low_c"),&f->low)||(f->high.valid&&(f->high.value< -100||f->high.value>200))||(f->low.valid&&(f->low.value< -100||f->low.value>200)))goto done;}}
    const cJSON *hourly=get(weather,"hourly_forecast");if(hourly){if(!cJSON_IsArray(hourly)||cJSON_GetArraySize(hourly)>12)goto done;const cJSON *day;cJSON_ArrayForEach(day,hourly){gso_forecast *f=&m->hourly_forecast[m->hourly_count++];const cJSON *date=get(day,"datetime"),*cond=get(day,"condition");if(!cJSON_IsString(date)||!(f->date=timestamp(date->valuestring))||(!cJSON_IsNull(cond)&&!str(cond,f->condition,sizeof(f->condition)))||!metric(get(day,"temperature_c"),&f->high)||(f->high.valid&&(f->high.value< -100||f->high.value>200))||(f->low.valid&&(f->low.value< -100||f->low.value>200)))goto done;}}
    const cJSON *condition=get(weather,"condition"),*location=get(weather,"location");if(!cJSON_IsNull(condition)&&!str(condition,m->weather_condition,sizeof(m->weather_condition)))goto done;if(!cJSON_IsNull(location)&&!str(location,m->weather_location,sizeof(m->weather_location)))goto done;
    const cJSON *quotas=get(get(root,"codex"),"quotas");if(!cJSON_IsArray(quotas)||cJSON_GetArraySize(quotas)>8)goto done;const cJSON *q;cJSON_ArrayForEach(q,quotas){const cJSON *bucket=get(q,"bucket"),*window=get(q,"window_duration_mins");if(!cJSON_IsString(bucket)||!cJSON_IsNumber(window))goto done;int provider=!strcmp(bucket->valuestring,"codex")?GSO_VIBE_CODEX:!strcmp(bucket->valuestring,"claude")?GSO_VIBE_CLAUDE:-1;if(provider<0)continue;int index=window->valuedouble==300?0:window->valuedouble==10080?1:-1;if(index<0)continue;gso_metric *remaining=&m->quota_remaining[provider][index];if(!metric(get(q,"remaining_percent"),remaining)||(remaining->valid&&!gso_metric_percent_valid(*remaining)))goto done;const cJSON *reset=get(q,"resets_at");if(cJSON_IsString(reset)){m->quota_resets[provider][index]=timestamp(reset->valuestring);if(!m->quota_resets[provider][index])goto done;}else if(!cJSON_IsNull(reset))goto done;}
    const cJSON *clients=get(get(root,"codex"),"clients");
    if(clients){
        if(!cJSON_IsArray(clients)||cJSON_GetArraySize(clients)>2)goto done;
        const cJSON *client;cJSON_ArrayForEach(client,clients){
            const cJSON *id=get(client,"id"),*online=get(client,"online"),*tokens=get(client,"tokens_today");
            if(!cJSON_IsString(id)||!cJSON_IsBool(online))goto done;
            int provider=!strcmp(id->valuestring,"codex")?GSO_VIBE_CODEX:!strcmp(id->valuestring,"claude")?GSO_VIBE_CLAUDE:-1;if(provider<0||m->clients[provider].reported)goto done;
            gso_client *c=&m->clients[provider];c->reported=true;c->online=cJSON_IsTrue(online);
            if(cJSON_IsNumber(tokens)){if(!isfinite(tokens->valuedouble)||tokens->valuedouble<0||tokens->valuedouble>9007199254740991.0||floor(tokens->valuedouble)!=tokens->valuedouble)goto done;c->tokens_today=gso_metric_number(tokens->valuedouble);}
            else if(tokens&&!cJSON_IsNull(tokens))goto done;
            else c->tokens_today=gso_metric_missing("tokens_unavailable");
        }
    }
    const cJSON *storage=get(root,"storage");
    if(storage){
        if(!source(get(storage,"source"),now,&m->storage))goto done;
        const cJSON *volumes=get(storage,"volumes");if(!cJSON_IsArray(volumes)||cJSON_GetArraySize(volumes)>(int)GSO_MAX_VOLUMES)goto done;
        const cJSON *volume;cJSON_ArrayForEach(volume,volumes){
            gso_volume *v=&m->volumes[m->volume_count];char id[33];
            if(!str(get(volume,"id"),id,sizeof(id))||!str(get(volume,"label"),v->label,sizeof(v->label))||!v->label[0])goto done;
            if(!metric(get(volume,"total_bytes"),&v->total)||!metric(get(volume,"free_bytes"),&v->free)||!capacity(v->total)||!capacity(v->free)||(v->total.valid&&v->free.valid&&v->free.value>v->total.value))goto done;
            m->volume_count++;
        }
    }
    const cJSON *nas=get(root,"nas");
    if(nas){
        gso_nas *n=&m->nas;
        if(!source(get(nas,"source"),now,&m->nas_source)||!metric(get(nas,"cpu_percent"),&n->cpu)||!metric(get(nas,"memory_percent"),&n->memory)||!metric(get(nas,"memory_total_bytes"),&n->memory_total))goto done;
        if(!metric(get(nas,"soc_temperature_c"),&n->soc)||!metric(get(nas,"gpu_temperature_c"),&n->gpu)||!metric(get(nas,"disk_temperature_c"),&n->disk))goto done;
        gso_metric *temperatures[]={&n->soc,&n->gpu,&n->disk};
        for(unsigned i=0;i<3;i++)if(temperatures[i]->valid&&(temperatures[i]->value<-100||temperatures[i]->value>200))goto done;
        if((n->cpu.valid&&!gso_metric_percent_valid(n->cpu))||(n->memory.valid&&!gso_metric_percent_valid(n->memory))||!capacity(n->memory_total))goto done;
        /* Optional HDD activity (GSN 0.1.29+): busy percent and throughput of the last sample. */
        n->disk_busy=n->disk_read_rate=n->disk_write_rate=gso_metric_missing("source_unavailable");
        if(get(nas,"disk_busy_percent")&&(!metric(get(nas,"disk_busy_percent"),&n->disk_busy)||(n->disk_busy.valid&&!gso_metric_percent_valid(n->disk_busy))))goto done;
        if(get(nas,"disk_read_bytes_per_second")&&(!metric(get(nas,"disk_read_bytes_per_second"),&n->disk_read_rate)||!nonnegative(n->disk_read_rate)))goto done;
        if(get(nas,"disk_write_bytes_per_second")&&(!metric(get(nas,"disk_write_bytes_per_second"),&n->disk_write_rate)||!nonnegative(n->disk_write_rate)))goto done;
        /* Optional eMMC activity (GSN 0.1.30+), same shape as the disk's. */
        n->emmc_busy=n->emmc_read_rate=n->emmc_write_rate=gso_metric_missing("source_unavailable");
        if(get(nas,"emmc_busy_percent")&&(!metric(get(nas,"emmc_busy_percent"),&n->emmc_busy)||(n->emmc_busy.valid&&!gso_metric_percent_valid(n->emmc_busy))))goto done;
        if(get(nas,"emmc_read_bytes_per_second")&&(!metric(get(nas,"emmc_read_bytes_per_second"),&n->emmc_read_rate)||!nonnegative(n->emmc_read_rate)))goto done;
        if(get(nas,"emmc_write_bytes_per_second")&&(!metric(get(nas,"emmc_write_bytes_per_second"),&n->emmc_write_rate)||!nonnegative(n->emmc_write_rate)))goto done;
        /* Optional wake-ups of the hard disk since local midnight (GSN 0.1.30+). */
        n->disk_wakes=gso_metric_missing("source_unavailable");
        if(get(nas,"disk_wake_count_today")&&(!metric(get(nas,"disk_wake_count_today"),&n->disk_wakes)||!nonnegative(n->disk_wakes)))goto done;
        n->disk_state=GSO_DISK_UNKNOWN;
        const cJSON *disk_state=get(nas,"disk_state");
        if(disk_state){
            const char *names[]={"active","idle","standby","quiet"};int found=-1;
            if(!cJSON_IsString(disk_state))goto done;
            for(int i=0;i<4;i++)if(!strcmp(disk_state->valuestring,names[i]))found=i;
            if(found<0)goto done;
            n->disk_state=(gso_disk_state)(found+1);
        }
        /* Optional SMART health of the hard disk (GSN 0.1.33+): a verdict and six counters. */
        {
            static const char *keys[6]={"disk_power_on_hours","disk_reallocated_sectors","disk_pending_sectors","disk_crc_errors","disk_power_cycles","disk_load_cycles"};
            gso_metric *counters[6]={&n->disk_hours,&n->disk_reallocated,&n->disk_pending,&n->disk_crc,&n->disk_power_cycles,&n->disk_load_cycles};
            for(unsigned i=0;i<6;i++){
                *counters[i]=gso_metric_missing("source_unavailable");
                if(get(nas,keys[i])&&(!metric(get(nas,keys[i]),counters[i])||!nonnegative(*counters[i])))goto done;
            }
            n->disk_health=GSO_DISK_HEALTH_UNKNOWN;
            const cJSON *health=get(nas,"disk_health");
            if(health){
                const char *names[]={"unknown","good","watch","bad"};int found=-1;
                if(!cJSON_IsString(health))goto done;
                for(int i=0;i<4;i++)if(!strcmp(health->valuestring,names[i]))found=i;
                if(found<0)goto done;
                n->disk_health=(gso_disk_health)found;
            }
        }
        n->reported=true;
    }
    const cJSON *printer=get(root,"printer");
    if(printer){
        gso_printer *p=&m->print;char state[16];
        if(!source(get(printer,"source"),now,&m->printer)||!str(get(printer,"state"),state,sizeof(state)))goto done;
        const char *states[]={"unknown","idle","preparing","running","paused","finished","failed"};int found=-1;
        for(int i=0;i<7;i++)if(!strcmp(state,states[i]))found=i;
        if(found<0)goto done;
        p->state=(unsigned)found;
        const cJSON *picture=get(printer,"thumbnail_id");
        if(cJSON_IsString(picture)){
            size_t length=strlen(picture->valuestring);
            if(length!=16)goto done;
            for(size_t i=0;i<length;i++){char ch=picture->valuestring[i];if(!((ch>='0'&&ch<='9')||(ch>='a'&&ch<='f')))goto done;}
            strcpy(p->thumbnail,picture->valuestring);
        }else if(picture&&!cJSON_IsNull(picture))goto done;
        const cJSON *job=get(printer,"job_name");
        if(cJSON_IsString(job)){if(!str(job,p->job,sizeof(p->job)))goto done;}
        else if(!cJSON_IsNull(job))goto done;
        if(!metric(get(printer,"progress_percent"),&p->progress)||!metric(get(printer,"remaining_minutes"),&p->remaining_minutes)||!metric(get(printer,"layer"),&p->layer)||!metric(get(printer,"layer_total"),&p->layer_total))goto done;
        if(!metric(get(printer,"nozzle_c"),&p->nozzle)||!metric(get(printer,"nozzle_target_c"),&p->nozzle_target)||!metric(get(printer,"bed_c"),&p->bed)||!metric(get(printer,"bed_target_c"),&p->bed_target)||!metric(get(printer,"error_code"),&p->error_code))goto done;
        gso_metric *temperatures[]={&p->nozzle,&p->nozzle_target,&p->bed,&p->bed_target};
        for(unsigned i=0;i<4;i++)if(!temperatures[i]->valid?false:(temperatures[i]->value<0||temperatures[i]->value>400))goto done;
        gso_metric *counts[]={&p->remaining_minutes,&p->layer,&p->layer_total,&p->error_code};
        for(unsigned i=0;i<4;i++)if(!capacity(*counts[i]))goto done;
        if(p->progress.valid&&!gso_metric_percent_valid(p->progress))goto done;
        p->reported=true;
    }
    const cJSON *car=get(root,"car");
    if(car){
        gso_car *c=&m->car;
        static const char *const vehicle_names[]={"unknown","driving","parked"},*const charge_names[]={"unknown","charging","plugged","unplugged"},*const lock_names[]={"unknown","locked","unlocked","locking","unlocking","jammed"};
        if(!source(get(car,"source"),now,&m->car_source)||!word(get(car,"vehicle_state"),vehicle_names,3,&c->vehicle)||!word(get(car,"charge_state"),charge_names,4,&c->charge)||!word(get(car,"lock_state"),lock_names,6,&c->lock))goto done;
        const cJSON *label=get(car,"name"),*lock_id=get(car,"lock_entity_id"),*refresh_id=get(car,"refresh_entity_id"),*updated=get(car,"updated_at");
        if(cJSON_IsString(label)){if(!str(label,c->name,sizeof(c->name)))goto done;}else if(!cJSON_IsNull(label))goto done;
        if(cJSON_IsString(lock_id)){if(!str(lock_id,c->lock_id,sizeof(c->lock_id)))goto done;}else if(!cJSON_IsNull(lock_id))goto done;
        if(cJSON_IsString(refresh_id)){if(!str(refresh_id,c->refresh_id,sizeof(c->refresh_id)))goto done;}else if(!cJSON_IsNull(refresh_id))goto done;
        if(cJSON_IsString(updated)){if(!(c->updated_at=timestamp(updated->valuestring)))goto done;}else if(!cJSON_IsNull(updated))goto done;
        if(!metric(get(car,"battery_percent"),&c->battery)||!metric(get(car,"range_km"),&c->range)||!metric(get(car,"remaining_charge_minutes"),&c->remaining_minutes)||!metric(get(car,"cabin_temperature_c"),&c->cabin)||!metric(get(car,"odometer_km"),&c->odometer)||!metric(get(car,"week_distance_km"),&c->week)||!metric(get(car,"energy_kwh_per_100km"),&c->consumption))goto done;
        static const char *const tyre_keys[4]={"tyre_fl_bar","tyre_fr_bar","tyre_rl_bar","tyre_rr_bar"},*const tyre_names[]={"unknown","ok","alarm"},*const open_names[]={"unknown","closed","open"};
        for(unsigned i=0;i<4;i++)if(get(car,tyre_keys[i])&&(!metric(get(car,tyre_keys[i]),&c->tyre[i])||!nonnegative(c->tyre[i])||(c->tyre[i].valid&&c->tyre[i].value>10)))goto done;
        if((get(car,"charging_power_kw")&&(!metric(get(car,"charging_power_kw"),&c->charging_power)||!nonnegative(c->charging_power)||(c->charging_power.valid&&c->charging_power.value>500)))||(get(car,"tyre_alarm")&&!word(get(car,"tyre_alarm"),tyre_names,3,&c->tyre_alarm))||(get(car,"windows_state")&&!word(get(car,"windows_state"),open_names,3,&c->windows))||(get(car,"trunk_state")&&!word(get(car,"trunk_state"),open_names,3,&c->trunk)))goto done;
        if((c->battery.valid&&!gso_metric_percent_valid(c->battery))||(c->cabin.valid&&(c->cabin.value< -100||c->cabin.value>200))||!nonnegative(c->range)||!nonnegative(c->remaining_minutes)||!nonnegative(c->odometer)||!nonnegative(c->week)||!nonnegative(c->consumption)||(c->range.valid&&c->range.value>5000)||(c->remaining_minutes.valid&&c->remaining_minutes.value>100000)||(c->odometer.valid&&c->odometer.value>9000000)||(c->week.valid&&c->week.value>100000)||(c->consumption.valid&&c->consumption.value>1000))goto done;
        const cJSON *offered=get(car,"actions"),*act;if(!cJSON_IsArray(offered)||cJSON_GetArraySize(offered)>3)goto done;
        cJSON_ArrayForEach(act,offered){unsigned flag=0;if(!cJSON_IsString(act))goto done;for(unsigned bit=GSO_HOME_LOCK;bit<=GSO_HOME_PRESS;bit<<=1)if(!strcmp(act->valuestring,gso_home_action_name(bit)))flag=bit;if(!flag||(c->actions&flag))goto done;c->actions|=flag;}
        c->reported=true;
    }
    if(!str(get(get(root,"codex"),"state"),m->task_state,sizeof(m->task_state))||(strcmp(m->task_state,"running")&&strcmp(m->task_state,"idle")&&strcmp(m->task_state,"unknown")))goto done;
    const cJSON *sessions=get(get(root,"codex"),"sessions");if(!cJSON_IsArray(sessions)||cJSON_GetArraySize(sessions)>4)goto done;
    const cJSON *session;bool selected=false,first=true;
    cJSON_ArrayForEach(session,sessions){
        const cJSON *title=get(session,"title"),*state=get(session,"state"),*duration=get(session,"running_duration_ms"),*start=get(session,"started_at");
        char checked[641];if(!str(get(session,"id"),checked,sizeof(checked))||!str(state,checked,sizeof(checked))||(strcmp(checked,"running")&&strcmp(checked,"idle")&&strcmp(checked,"unknown")))goto done;
        if(!cJSON_IsNull(title)&&!str(title,checked,sizeof(checked)))goto done;
        if(duration&&!cJSON_IsNull(duration)&&(!cJSON_IsNumber(duration)||!isfinite(duration->valuedouble)||duration->valuedouble<0||duration->valuedouble>9007199254740991.0||floor(duration->valuedouble)!=duration->valuedouble))goto done;
        if(start&&!cJSON_IsNull(start)&&(!cJSON_IsString(start)||!timestamp(start->valuestring)))goto done;
        const cJSON *duration_reason=get(session,"duration_reason");unsigned provider=gso_vibe_session_provider(get(session,"id")->valuestring);
        if(first){m->vibe_provider=provider;first=false;}
        if(!selected&&!strcmp(m->task_state,"unknown")&&!strcmp(state->valuestring,"unknown")){
            selected=true;m->task_provider=m->vibe_provider=provider;
            if(cJSON_IsString(duration_reason)&&!strcmp(duration_reason->valuestring,"task_event_timeout"))m->task_duration_ms=gso_metric_missing("task_event_timeout");
        }
        if(!selected&&!strcmp(m->task_state,"running")&&!strcmp(state->valuestring,"running")){
            selected=true;m->task_provider=m->vibe_provider=provider;if(cJSON_IsString(title))strcpy(m->task_title,title->valuestring);
            if(cJSON_IsNumber(duration)&&cJSON_IsString(start)){m->task_duration_ms=gso_metric_number(duration->valuedouble);m->task_started_at=timestamp(start->valuestring);}
        }
    }
    /* Without a selected task, never focus a provider that has no quota while the other one has data. */
    /* Without a selected task, never focus a provider that has no quota while the other one has data; with no
       sessions at all the provider is whichever alone has quota data, otherwise none (no default provider). */
    if(!selected){
        bool have[GSO_VIBE_PROVIDERS];for(unsigned p=0;p<GSO_VIBE_PROVIDERS;p++)have[p]=m->quota_remaining[p][0].valid||m->quota_remaining[p][1].valid;
        if(m->vibe_provider<GSO_VIBE_PROVIDERS){unsigned other=1u-m->vibe_provider;if(!have[m->vibe_provider]&&have[other])m->vibe_provider=other;}
        else m->vibe_provider=have[GSO_VIBE_CLAUDE]!=have[GSO_VIBE_CODEX]?(have[GSO_VIBE_CLAUDE]?GSO_VIBE_CLAUDE:GSO_VIBE_CODEX):GSO_VIBE_NONE;
    }
    const cJSON *generated=get(root,"generated_at");if(!cJSON_IsString(generated)||!(m->generated_at=timestamp(generated->valuestring))||!home_model(get(root,"home"),now,m))goto done;m->received_ms=now;
    valid=true;*out=*m;
done:free(m);cJSON_Delete(root);return valid;
}
