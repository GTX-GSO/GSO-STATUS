#include <assert.h>
#include <stdio.h>
#include "gso_link.h"

enum {CONNECTING=0,ONLINE=1,NETWORK_OFFLINE=2,GSN_OFFLINE=3,AUTH=4};

int main(void){
    gso_link link;gso_link_init(&link,0,CONNECTING);
    assert(gso_link_update(&link,1000,true,0,false,ONLINE)==ONLINE);
    /* One slow or lost poll keeps the last status. */
    assert(gso_link_update(&link,2000,false,GSN_OFFLINE,false,ONLINE)==ONLINE);
    assert(gso_link_update(&link,3000,true,0,false,ONLINE)==ONLINE&&link.failures==0);
    /* Many failures inside the grace window are still hidden; the grace window and the count must both be met. */
    for(int i=0;i<5;i++)assert(gso_link_update(&link,4000+i*1000,false,GSN_OFFLINE,false,ONLINE)==ONLINE);
    assert(gso_link_update(&link,3000+GSO_LINK_GRACE_MS-1,false,GSN_OFFLINE,false,ONLINE)==ONLINE);
    assert(gso_link_update(&link,3000+GSO_LINK_GRACE_MS,false,GSN_OFFLINE,false,ONLINE)==GSN_OFFLINE);
    /* One good snapshot recovers at once. */
    assert(gso_link_update(&link,20000,true,0,false,ONLINE)==ONLINE);
    /* Two failures after a long silence are not enough; the third is. */
    gso_link_init(&link,0,ONLINE);
    assert(gso_link_update(&link,30000,false,GSN_OFFLINE,false,ONLINE)==ONLINE);
    assert(gso_link_update(&link,32000,false,GSN_OFFLINE,false,ONLINE)==ONLINE);
    assert(gso_link_update(&link,34000,false,GSN_OFFLINE,false,ONLINE)==GSN_OFFLINE);
    /* Certain failures show immediately. */
    gso_link_init(&link,0,CONNECTING);gso_link_update(&link,100,true,0,false,ONLINE);
    assert(gso_link_update(&link,200,false,AUTH,true,ONLINE)==AUTH);
    assert(gso_link_update(&link,300,false,NETWORK_OFFLINE,true,ONLINE)==NETWORK_OFFLINE);
    /* Never connected: stays connecting until the grace window ends. */
    gso_link_init(&link,0,CONNECTING);
    assert(gso_link_update(&link,1000,false,GSN_OFFLINE,false,ONLINE)==CONNECTING);
    assert(gso_link_update(&link,5000,false,GSN_OFFLINE,false,ONLINE)==CONNECTING);
    assert(gso_link_update(&link,GSO_LINK_GRACE_MS,false,GSN_OFFLINE,false,ONLINE)==GSN_OFFLINE);
    puts("PASS: GSN link debounce hides single misses, shows certain failures at once, recovers on one good snapshot");
    return 0;
}
