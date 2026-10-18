#include "gso_link.h"

void gso_link_init(gso_link *link,uint64_t now_ms,int initial){link->last_ok_ms=now_ms;link->failures=0;link->shown=initial;}

int gso_link_update(gso_link *link,uint64_t now_ms,bool ok,int failure,bool definite,int online){
    if(ok){link->failures=0;link->last_ok_ms=now_ms;link->shown=online;return link->shown;}
    if(link->failures<0xFFFFu)link->failures++;
    if(definite||(link->failures>=GSO_LINK_MIN_FAILURES&&now_ms-link->last_ok_ms>=GSO_LINK_GRACE_MS))link->shown=failure;
    return link->shown;
}
