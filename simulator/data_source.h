#pragma once
#include "gso_snapshot.h"
bool desk_data_start(const char *path);
void desk_data_stop(void);
bool desk_data_latest(gso_snapshot *out);
bool desk_data_connected(void);
/* Model picture of the printer's current job: GSO_THUMBNAIL_SIZE square, raw ARGB8888 (blue, green, red, alpha bytes,
   straight alpha), served by GSN under the id the snapshot names. Copies it and returns true once the picture for that id
   has arrived; false while it is still being fetched, or when the producer has none. */
#define GSO_THUMBNAIL_SIZE 120
#define GSO_THUMBNAIL_BYTES (GSO_THUMBNAIL_SIZE*GSO_THUMBNAIL_SIZE*4)
bool desk_data_thumbnail(const char *key,uint8_t *pixels);
uint64_t desk_data_now_ms(void);

typedef enum { DESK_CONNECTING, DESK_ONLINE, DESK_NETWORK_OFFLINE, DESK_GSN_OFFLINE, DESK_GSN_AUTH, DESK_GSN_RESPONSE } desk_connection_status;
desk_connection_status desk_data_status(void);

#ifdef GSO_TEST
void desk_data_test_status(desk_connection_status status);
void desk_data_test_thumbnail(const char *key,const uint8_t *pixels); /* pretend the picture for key has arrived */
#endif
