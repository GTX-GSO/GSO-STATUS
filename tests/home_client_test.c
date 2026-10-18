#include <windows.h>
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "data_source.h"
#include "home_client.h"
int main(int argc,char **argv){
 assert(argc==3);
 if(!strcmp(argv[2],"invalid")){assert(!desk_data_start(argv[1]));puts("PASS: invalid HOME credentials or snapshot URL rejected");return 0;}
 if(!strcmp(argv[2],"read-only")){assert(desk_data_start(argv[1]));assert(!desk_home_ready());desk_data_stop();puts("PASS: invalid optional HOME token preserves read-only telemetry");return 0;}
 assert(desk_data_start(argv[1]));assert(desk_home_ready());
 uint64_t until=GetTickCount64()+4000;gso_snapshot model;
 do{desk_data_latest(&model);if(desk_data_connected()&&model.home_count)break;Sleep(20);}while(GetTickCount64()<until);
 assert(desk_data_connected()&&model.home_count==4);
 const char *target="light.test";
 if(!strcmp(argv[2],"stop")){
  target="cover.left";assert(desk_home_command(target,GSO_HOME_OPEN,gso_metric_missing("not_used")));
  uint64_t sent=GetTickCount64()+2000;while(desk_home_latest().state!=DESK_HOME_SENT&&GetTickCount64()<sent)Sleep(5);Sleep(100);
  assert(desk_home_command(target,GSO_HOME_STOP,gso_metric_missing("not_used")));assert(!desk_home_command(target,GSO_HOME_STOP,gso_metric_missing("not_used")));
 }else{
  assert(!desk_home_command("light.unbound",GSO_HOME_ON,gso_metric_missing("not_used")));
  assert(desk_home_command(target,GSO_HOME_ON,gso_metric_missing("not_used")));
  assert(!desk_home_command(target,GSO_HOME_ON,gso_metric_missing("not_used")));
 }
 until=GetTickCount64()+15000;desk_home_result result;
 do{result=desk_home_latest();if(!result.busy)break;Sleep(20);}while(GetTickCount64()<until);
 assert(!result.busy);assert(!strcmp(result.entity_id,target));
 if(!strcmp(argv[2],"confirmed")||!strcmp(argv[2],"stop"))assert(result.state==DESK_HOME_CONFIRMED);
 else if(!strcmp(argv[2],"rejected"))assert(result.state==DESK_HOME_REJECTED);
 else assert(result.state==DESK_HOME_UNKNOWN);
 desk_data_stop();puts("PASS: native HOME command identity, busy protection and asynchronous result");return 0;
}
