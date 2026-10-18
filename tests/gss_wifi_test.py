"""Exercise production credential validation and NVS failures with a host NVS shim."""
from pathlib import Path
import subprocess
root=Path(__file__).resolve().parents[1]
out=root/'work/gss-wifi-test';out.mkdir(exist_ok=True)
(out/'test.c').write_text(r'''
#include "gss_wifi.h"
#include "gss_wifi_storage.h"
#include <stdlib.h>
#define assert(x) do {if(!(x)){fprintf(stderr,"FAIL line %d\n",__LINE__);exit(1);}} while(0)
#include <string.h>
#include <stdio.h>
static char disk[256],pending[256];static size_t len,plen;static int fail;
bool gss_wifi_storage_read(void *p,size_t*n){if(!len||*n<len)return false;memcpy(p,disk,len);*n=len;return true;}
bool gss_wifi_storage_write(const void*p,size_t n){if(fail)return false;memcpy(disk,p,n);len=n;return true;}
bool gss_wifi_storage_forget(void){if(fail)return false;len=0;return true;}
int main(void){
 gss_wifi_start();assert(!*gss_wifi_ssid());
 gss_wifi_save("test-network","unitpass",0);assert(!strcmp(gss_wifi_ssid(),"test-network")&&gss_wifi_has_password());
 gss_wifi_save("test-network","",0);assert(gss_wifi_has_password());
 gss_wifi_save("other-network","",0);assert(!strcmp(gss_wifi_ssid(),"test-network"));
 gss_wifi_save("other-network","short",0);assert(!strcmp(gss_wifi_ssid(),"test-network"));
 gss_wifi_save("123456789012345678901234567890123","unitpass",0);assert(!strcmp(gss_wifi_ssid(),"test-network"));
 fail=1;gss_wifi_save("fail-network","unitpass",0);assert(!strcmp(gss_wifi_ssid(),"test-network"));assert(!gss_wifi_forget());assert(gss_wifi_has_password());fail=0;
 gss_wifi_start();assert(!strcmp(gss_wifi_ssid(),"test-network"));
 gss_wifi_save("open-network","",1);assert(gss_wifi_is_open()&&!gss_wifi_has_password());gss_wifi_start();assert(gss_wifi_is_open());
 assert(gss_wifi_forget());assert(!*gss_wifi_ssid());gss_wifi_start();assert(!*gss_wifi_ssid());
 gss_wifi_save("test-network","unitpass",0);disk[0]=99;gss_wifi_start();assert(!*gss_wifi_ssid());
 puts("PASS: credentials boundaries, retained password, open network, persistence, commit failure, forget, corrupt profile");
}
''')
exe=out/'wifi-test.exe'
subprocess.run([str(root/'tools/zig/zig.exe'),'cc','-O2','-I'+str(out),'-I'+str(root/'shared'),str(root/'shared/gss_wifi_profile.c'),str(out/'test.c'),'-o',str(exe)],check=True)
subprocess.run([str(exe)],check=True)
