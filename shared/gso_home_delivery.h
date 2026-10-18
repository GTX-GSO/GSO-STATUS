#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
typedef struct {
 void *context;
 int (*request)(void *,bool,const char *,char *,size_t);
 bool (*apply)(void *,const char *);
 bool (*current)(void *);
 uint64_t (*now)(void *);
 void (*wait)(void *);
} gso_home_delivery;
typedef enum {GSO_DELIVERY_DONE,GSO_DELIVERY_REJECTED,GSO_DELIVERY_UNKNOWN} gso_delivery_result;
/* POST exactly once; an ambiguous outcome is resolved exclusively using GET. */
gso_delivery_result gso_home_deliver(const gso_home_delivery *io,const char *payload,uint64_t deadline,char *response,size_t capacity);
