#include "gso_snapshot.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static char body[GSO_SNAPSHOT_MAX_BYTES+1];
int main(int argc,char **argv){
 assert(argc==2);FILE *file=fopen(argv[1],"rb");assert(file);size_t n=fread(body,1,sizeof(body)-1,file);fclose(file);body[n]=0;
 gso_snapshot model;gso_snapshot_empty(&model);assert(gso_snapshot_parse(body,n,1000,&model));
 if(strstr(argv[1],"vibe-claude")){assert(model.clients[GSO_VIBE_CODEX].reported&&!model.clients[GSO_VIBE_CODEX].online&&!model.clients[GSO_VIBE_CODEX].tokens_today.valid);assert(model.clients[GSO_VIBE_CLAUDE].online&&model.clients[GSO_VIBE_CLAUDE].tokens_today.value==1250000);
  char *negative=malloc(n+1);memcpy(negative,body,n+1);char *count=strstr(negative,"1250000");assert(count);memcpy(count,"-1     ",7);gso_snapshot rejected;gso_snapshot_empty(&rejected);assert(!gso_snapshot_parse(negative,n,1000,&rejected));free(negative);}
 else assert(!model.clients[GSO_VIBE_CODEX].reported&&!model.clients[GSO_VIBE_CLAUDE].reported);
 if(strstr(argv[1],"net-details")){assert(model.storage.quality==GSO_OK&&model.volume_count==2&&!strcmp(model.volumes[0].label,"hdd0")&&model.volumes[1].free.value==812345678901.0&&model.volumes[0].total.value==4000787030016.0);
  char *oversized=malloc(n+1);memcpy(oversized,body,n+1);char *total=strstr(oversized,"2000398934016");assert(total);memcpy(total,"100000000000 ",13);gso_snapshot rejected;gso_snapshot_empty(&rejected);assert(!gso_snapshot_parse(oversized,n,1000,&rejected));free(oversized);}
 else assert(model.volume_count==0&&model.storage.quality==GSO_UNCONFIGURED);
 if(strstr(argv[1],"vibe-running")){assert(model.task_title[0]);assert(model.task_duration_ms.valid&&model.task_duration_ms.value==3723000);assert(model.task_started_at>0);}
 if(strstr(argv[1],"vibe-claude")){assert(!strcmp(model.task_state,"running")&&model.task_provider==GSO_VIBE_CLAUDE&&model.vibe_provider==GSO_VIBE_CLAUDE);assert(!strcmp(model.task_title,"VIBE接入Claude")&&model.task_duration_ms.value==1800000);assert(model.quota_remaining[GSO_VIBE_CODEX][0].value==67&&model.quota_remaining[GSO_VIBE_CODEX][1].value==95);assert(model.quota_remaining[GSO_VIBE_CLAUDE][0].valid&&model.quota_remaining[GSO_VIBE_CLAUDE][0].value==76.5&&model.quota_resets[GSO_VIBE_CLAUDE][0]>0);assert(!model.quota_remaining[GSO_VIBE_CLAUDE][1].valid&&model.quota_resets[GSO_VIBE_CLAUDE][1]>0);assert(!strcmp(gso_vibe_provider_name(GSO_VIBE_CLAUDE),"Claude")&&gso_vibe_session_provider("89abcdef01234567")==GSO_VIBE_CODEX);
  /* Idle with no Codex quota focuses the provider that has quota data. */
  char *copy=malloc(n+1);memcpy(copy,body,n+1);char *state=strstr(copy,"\"state\": \"running\"");assert(state);memcpy(state+9,"\"idle\"   ",9);state=strstr(state+1,"\"state\": \"running\"");assert(state);memcpy(state+9,"\"idle\"   ",9);char *bucket;while((bucket=strstr(copy,"\"bucket\": \"codex\"")))memcpy(bucket+11,"other",5);
  gso_snapshot idle;gso_snapshot_empty(&idle);assert(gso_snapshot_parse(copy,n,1000,&idle));assert(!strcmp(idle.task_state,"idle")&&idle.vibe_provider==GSO_VIBE_CLAUDE&&!idle.quota_remaining[GSO_VIBE_CODEX][0].valid);free(copy);}
 if(strstr(argv[1],"vibe-timeout")){assert(!strcmp(model.task_state,"unknown"));assert(!model.task_duration_ms.valid);assert(!strcmp(model.task_duration_ms.reason,"task_event_timeout"));}
 if(strstr(argv[1],"snapshot-home")){assert(model.home_count==4);assert(!model.home_entities[1].current_temperature.valid);assert(model.home_entities[1].target_temperature.value==25);assert(model.home_entities[1].temperature_step.value==1);assert(gso_home_action_allowed(&model.home_entities[0],GSO_HOME_ON,1000));assert(!gso_home_action_allowed(&model.home_entities[0],GSO_HOME_STOP,1000));assert(!gso_home_action_allowed(&model.home_entities[0],GSO_HOME_ON,50000));assert(strlen(model.home_epoch)==36);}
 if(strstr(argv[1],"weather-clocks")){assert(model.cpu.clock.valid&&model.cpu.clock.value==4200);assert(model.cpu.memory_clock_configured&&model.cpu.memory_clock.value==1800);assert(model.gpu.memory_clock.value==10701);assert(model.humidity.value==59&&model.forecast_count==6&&model.hourly_count==12&&model.hourly_forecast[0].high.value==30.3);}
 if(strstr(argv[1],"game-icon")){assert(model.game_icon_valid&&model.game_icon[0]==255&&model.game_icon[1]==128&&model.game_icon[2]==64&&model.game_icon[3]==255);assert(model.latency.valid&&model.latency.value==0.25);char *icon=strstr(body,"icon_argb8888");assert(icon);char *start=strchr(icon,':');assert(start);start=strchr(start,'"')+1;char saved=*start;*start='!';assert(!gso_snapshot_parse(body,n,1001,&model));*start=saved;}
 if(strstr(argv[1],"net-details")){assert(model.subscription_updated==1792118941&&model.antiad_count.value==102535&&model.dhcp_leases.value==4);assert(model.net_details[0].quality==GSO_OK);}
 if(strstr(argv[1],"home-rich")){assert(model.home_entities[0].brightness.valid&&model.home_entities[0].brightness.value==80);assert(model.home_entities[0].actions&GSO_HOME_COLOR);assert(model.home_entities[1].mode_count==2&&!strcmp(model.home_entities[1].modes[1],"off"));}
 if(strstr(argv[1],"snapshot-printer")){assert(model.print.reported&&model.printer.quality==GSO_OK&&model.print.state==GSO_PRINT_RUNNING&&!strcmp(model.print.job,"Benchy"));
  assert(model.print.progress.value==42&&model.print.remaining_minutes.value==75&&model.print.layer.value==10&&model.print.layer_total.value==100);
  assert(model.print.nozzle.value==219.6&&model.print.nozzle_target.value==220&&model.print.bed.value==59.8&&model.print.bed_target.value==60&&model.print.error_code.valid&&model.print.error_code.value==0);
  assert(!strcmp(model.print.thumbnail,"0123456789abcdef"));
  const char *bad[]={"\"value\": 42,","\"value\": 219.6,","\"state\": \"running\",","\"thumbnail_id\": \"0123456789abcdef\""};const char *replacement[]={"\"value\": 142,","\"value\": 999.6,","\"state\": \"melting\",","\"thumbnail_id\": \"0123456789abcdeg\""};
  for(unsigned i=0;i<4;i++){char *copy=malloc(n+16);memcpy(copy,body,n+1);char *at=strstr(copy,bad[i]);assert(at);size_t old_length=strlen(bad[i]),new_length=strlen(replacement[i]);memmove(at+new_length,at+old_length,strlen(at+old_length)+1);memcpy(at,replacement[i],new_length);
   gso_snapshot rejected;gso_snapshot_empty(&rejected);assert(!gso_snapshot_parse(copy,strlen(copy),1000,&rejected));free(copy);}}
 else assert(!model.print.reported&&model.printer.quality==GSO_UNCONFIGURED&&model.print.state==GSO_PRINT_UNKNOWN&&!model.print.progress.valid);
 if(strstr(argv[1],"snapshot-car")){assert(model.car.reported&&model.car_source.quality==GSO_OK&&!strcmp(model.car.name,"零跑C11")&&model.car.vehicle==GSO_CAR_PARKED&&model.car.charge==GSO_CAR_CHARGING&&model.car.lock==GSO_CAR_LOCKED);
  assert(model.car.battery.value==76&&model.car.range.value==403.5&&model.car.remaining_minutes.value==95&&model.car.cabin.value==28&&model.car.odometer.value==20610&&model.car.week.value==132.4&&model.car.consumption.value==14.8&&model.car.updated_at>0);
  assert(model.car.tyre[0].value==2.33&&model.car.tyre[1].value==2.35&&model.car.tyre[2].value==2.31&&model.car.tyre[3].value==2.34&&model.car.tyre_alarm==GSO_CAR_TYRE_OK&&model.car.windows==GSO_CAR_CLOSED&&model.car.trunk==GSO_CAR_CLOSED&&model.car.charging_power.valid&&model.car.charging_power.value==6.8);
  assert(model.car.actions==(GSO_HOME_UNLOCK|GSO_HOME_PRESS)&&!strcmp(model.car.lock_id,"lock.ling_pao_c11_che_men_suo")&&!strcmp(model.car.refresh_id,"button.ling_pao_c11_shua_xin_che_kuang"));
  assert(gso_snapshot_action_allowed(&model,model.car.lock_id,GSO_HOME_UNLOCK,1000)&&gso_snapshot_action_allowed(&model,model.car.refresh_id,GSO_HOME_PRESS,1000));
  assert(!gso_snapshot_action_allowed(&model,model.car.lock_id,GSO_HOME_LOCK,1000)&&!gso_snapshot_action_allowed(&model,model.car.refresh_id,GSO_HOME_UNLOCK,1000)&&!gso_snapshot_action_allowed(&model,"lock.other",GSO_HOME_UNLOCK,1000));
  assert(!gso_snapshot_action_allowed(&model,model.car.lock_id,GSO_HOME_UNLOCK,1000+130000));
  assert(gso_home_action_slow(GSO_HOME_UNLOCK)&&!gso_home_action_slow(GSO_HOME_PRESS)&&!strcmp(gso_home_action_name(GSO_HOME_LOCK),"lock"));
  const char *bad[]={"\"value\": 76,","\"lock_state\": \"locked\",","\"unlock\",","\"updated_at\": \"2026-10-17T13:20:00+00:00\""};const char *replacement[]={"\"value\": 176,","\"lock_state\": \"ajar\",","\"pry\",","\"updated_at\": \"yesterday\""};
  for(unsigned i=0;i<4;i++){char *copy=malloc(n+32);memcpy(copy,body,n+1);char *at=strstr(copy,bad[i]);assert(at);size_t old_length=strlen(bad[i]),new_length=strlen(replacement[i]);memmove(at+new_length,at+old_length,strlen(at+old_length)+1);memcpy(at,replacement[i],new_length);
   gso_snapshot rejected;gso_snapshot_empty(&rejected);assert(!gso_snapshot_parse(copy,strlen(copy),1000,&rejected));free(copy);}}
 else assert(!model.car.reported&&model.car_source.quality==GSO_UNCONFIGURED&&model.car.lock==GSO_CAR_LOCK_UNKNOWN&&!model.car.tyre[0].valid&&model.car.windows==GSO_CAR_OPEN_UNKNOWN&&!model.car.battery.valid&&!model.car.actions);
 if(strstr(argv[1],"snapshot-nas")){assert(model.nas.reported&&model.nas_source.quality==GSO_OK&&model.nas.cpu.value==22.5&&model.nas.memory.value==72.3&&model.nas.memory_total.value==2056081408.0&&model.nas.soc.value==44.4&&model.nas.gpu.value==43.8&&model.nas.disk.valid&&model.nas.disk.value==41&&model.nas.disk_state==GSO_DISK_ACTIVE&&model.nas.disk_busy.value==12.5&&model.nas.disk_read_rate.value==5242880&&model.nas.disk_write_rate.valid&&model.nas.disk_wakes.value==3&&model.nas.emmc_busy.value==3.5&&model.nas.emmc_read_rate.value==1048576&&model.nas.emmc_write_rate.value==2048&&model.nas.disk_health==GSO_DISK_HEALTH_GOOD&&model.nas.disk_hours.value==11234&&model.nas.disk_reallocated.valid&&model.nas.disk_reallocated.value==0&&model.nas.disk_pending.value==0&&model.nas.disk_crc.value==2&&model.nas.disk_power_cycles.value==56&&model.nas.disk_load_cycles.value==3000);
  assert(model.disk_count==1&&model.disks[0].busy.valid&&model.disks[0].busy.value==12.5&&model.disks[0].read_ms.valid&&model.disks[0].read_ms.value==0.4&&!model.disks[0].write_ms.valid&&model.disks[0].response_ms.value==0.4&&model.disks[0].read_rate.value==5242880&&model.disks[0].write_rate.valid&&model.disks[0].write_rate.value==0);
  const char *bad[]={"\"value\": 22.5,","\"value\": 72.3,","\"value\": 44.4,","\"value\": 41,"};const char *replacement[]={"\"value\": 122.5,","\"value\": -2.3,","\"value\": 244.4,","\"value\": 241,"};
  for(unsigned i=0;i<4;i++){char *copy=malloc(n+32);memcpy(copy,body,n+1);char *at=strstr(copy,bad[i]);assert(at);size_t old_length=strlen(bad[i]),new_length=strlen(replacement[i]);memmove(at+new_length,at+old_length,strlen(at+old_length)+1);memcpy(at,replacement[i],new_length);
   gso_snapshot rejected;gso_snapshot_empty(&rejected);assert(!gso_snapshot_parse(copy,strlen(copy),1000,&rejected));free(copy);}}
 else assert(!model.nas.reported&&model.nas_source.quality==GSO_UNCONFIGURED&&!model.nas.cpu.valid&&!model.nas.disk.valid);
 if(strstr(argv[1],"snapshot-ambient")){assert(model.ambient.quality==GSO_OK&&model.illuminance.valid&&model.illuminance.value==31);}
 gso_snapshot old=model;assert(!gso_snapshot_parse("{",1,1001,&model));assert(memcmp(&old,&model,sizeof(model))==0);
 assert(!gso_snapshot_parse(body,32769,1001,&model));
 const char *duplicate="{\"schema_version\":\"1.0\",\"schema_version\":\"1.0\"}";
 assert(!gso_snapshot_parse(duplicate,strlen(duplicate),1001,&model));
 puts("PASS: bounded snapshot parse and invalid input retention");return 0;
}
