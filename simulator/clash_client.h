#pragma once
#include <stdbool.h>
#include <stdint.h>
typedef enum {CLASH_IDLE,CLASH_SWITCHING,CLASH_CONFIRMED,CLASH_UNAVAILABLE,CLASH_AUTH,CLASH_UNCONFIRMED} desk_clash_state;
typedef struct {bool configured,busy;desk_clash_state state;char mode[8];uint64_t observed_ms;} desk_clash_result;
bool desk_clash_start(const char *base_url,const char *token);
void desk_clash_stop(void);
bool desk_clash_mode(const char *mode);
desk_clash_result desk_clash_latest(void);
