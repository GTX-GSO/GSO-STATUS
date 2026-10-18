#pragma once
#include <stdbool.h>
#include <stdint.h>

/* GSN link debounce shared by the Windows simulator and the ESP32-P4 firmware: one slow or lost poll must not flash
   "GSN 离线". A transport failure is only shown after GSO_LINK_MIN_FAILURES consecutive failures AND
   GSO_LINK_GRACE_MS without a good snapshot. Certain failures (authentication, no network interface) show at once.
   Status values are the caller's own desk_connection_status integers. */
#define GSO_LINK_GRACE_MS 10000u
#define GSO_LINK_MIN_FAILURES 3u

typedef struct {uint64_t last_ok_ms;unsigned failures;int shown;} gso_link;

void gso_link_init(gso_link *link,uint64_t now_ms,int initial);
/* Returns the status to show. ok: a valid snapshot was received. definite: the failure is certain, not transient. */
int gso_link_update(gso_link *link,uint64_t now_ms,bool ok,int failure,bool definite,int online);
