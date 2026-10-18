#include <windows.h>
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "data_source.h"
int main(int argc,char **argv){
 assert(argc==3);uint64_t start=GetTickCount64();assert(desk_data_start(argv[1]));Sleep(5500);
 gso_snapshot snapshot;desk_data_latest(&snapshot);
 if(!strcmp(argv[2],"retention")){assert(snapshot.windows.received);assert(snapshot.cpu.load.valid&&snapshot.cpu.load.value==0);assert(!desk_data_connected());}
 else{assert(!snapshot.windows.received);assert(!desk_data_connected());}
 desk_data_stop();assert(GetTickCount64()-start<7000);
 puts("PASS: native HTTP client bounded response / last valid retention / total deadline");return 0;
}
