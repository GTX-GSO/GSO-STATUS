"""Compile actual delivery engine; verify ambiguous-response and cancellation safety."""
from pathlib import Path
import subprocess
root=Path(__file__).resolve().parents[1];out=root/'work/home-delivery-test';out.mkdir(parents=True,exist_ok=True)
(out/'test.c').write_text(r'''
#include <assert.h>
#include <stdio.h>
#include "gso_home_delivery.h"
typedef struct {int post_code,get_code,posts,gets;bool active,confirmed,cancel_on_wait;uint64_t now;} fixture;
static int request(void *c,bool post,const char *payload,char *response,size_t capacity){fixture *f=c;(void)payload;(void)response;(void)capacity;if(post){f->posts++;return f->post_code;}f->gets++;return f->get_code;}
static bool apply(void *c,const char *body){(void)body;return ((fixture*)c)->confirmed;}
static bool current(void *c){return ((fixture*)c)->active;}
static uint64_t now(void *c){return ((fixture*)c)->now;}
static void wait(void *c){fixture *f=c;f->now+=500;if(f->cancel_on_wait)f->active=false;}
static gso_delivery_result run(fixture *f){char body[8];gso_home_delivery io={f,request,apply,current,now,wait};return gso_home_deliver(&io,"{}",2000,body,7);}
int main(void){
 fixture f={.post_code=-1,.get_code=200,.active=true,.confirmed=true};assert(run(&f)==GSO_DELIVERY_DONE&&f.posts==1&&f.gets==1);
 f=(fixture){.post_code=401,.active=true};assert(run(&f)==GSO_DELIVERY_REJECTED&&f.posts==1&&f.gets==0);
 f=(fixture){.post_code=500,.get_code=404,.active=true};assert(run(&f)==GSO_DELIVERY_UNKNOWN&&f.posts==1&&f.gets==3);
 f=(fixture){.post_code=-1,.get_code=200,.active=true,.confirmed=true,.cancel_on_wait=true};assert(run(&f)==GSO_DELIVERY_UNKNOWN&&f.posts==1&&f.gets==0);
 f=(fixture){.post_code=200,.active=true,.confirmed=true};assert(run(&f)==GSO_DELIVERY_DONE&&f.posts==1&&f.gets==0);
 f=(fixture){.active=true,.now=2000};assert(run(&f)==GSO_DELIVERY_REJECTED&&f.posts==0);
 puts("PASS: lost POST response uses GET only; auth failure stops; 5xx never replays; superseded work stops; expiration prevents send");
}
''',encoding='utf-8')
exe=out/'test.exe'
subprocess.run([str(root/'tools/zig/zig.exe'),'cc','-O2','-I'+str(root/'shared'),str(root/'shared/gso_home_delivery.c'),str(out/'test.c'),'-o',str(exe)],check=True)
subprocess.run([str(exe)],check=True)
