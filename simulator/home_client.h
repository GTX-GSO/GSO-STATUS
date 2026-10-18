#pragma once
#include "gso_snapshot.h"
#include <wchar.h>
typedef enum {DESK_HOME_NONE,DESK_HOME_QUEUED,DESK_HOME_SENT,DESK_HOME_CONFIRMED,DESK_HOME_REJECTED,DESK_HOME_TIMED_OUT,DESK_HOME_UNKNOWN,DESK_HOME_SUPERSEDED} desk_home_state;
typedef struct {desk_home_state state;bool busy;unsigned action;uint64_t updated_ms;char entity_id[161];} desk_home_result;
bool desk_home_start(const wchar_t *host,unsigned short port,bool secure,const wchar_t *route,const wchar_t *read_header,const wchar_t *control_header);
void desk_home_stop(void);
bool desk_home_ready(void);
bool desk_home_command(const char *entity_id,unsigned action,gso_metric temperature);
bool desk_home_setting(const char *entity_id,unsigned action,gso_metric value,const char *mode);
desk_home_result desk_home_latest(void);
#ifdef GSO_TEST
bool desk_home_test_isolated(void);
#endif
