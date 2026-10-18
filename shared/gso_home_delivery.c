#include "gso_home_delivery.h"
gso_delivery_result gso_home_deliver(const gso_home_delivery *io,const char *payload,uint64_t deadline,char *response,size_t capacity){
 if(!io->current(io->context)||io->now(io->context)>=deadline)return GSO_DELIVERY_REJECTED;
 int code=io->request(io->context,true,payload,response,capacity);
 if(code>=300&&code<500)return GSO_DELIVERY_REJECTED;
 if(code>=200&&code<300&&io->apply(io->context,response))return GSO_DELIVERY_DONE;
 while(io->current(io->context)&&io->now(io->context)<deadline){
  io->wait(io->context);
  if(!io->current(io->context)||io->now(io->context)>=deadline)break;
  code=io->request(io->context,false,NULL,response,capacity);
  if(code==200&&io->apply(io->context,response))return GSO_DELIVERY_DONE;
 }
 return GSO_DELIVERY_UNKNOWN;
}
