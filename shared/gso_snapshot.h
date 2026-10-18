#ifndef GSO_SNAPSHOT_H
#define GSO_SNAPSHOT_H
#include "gso_model.h"

typedef struct {
    char model[641];
    gso_metric load,temperature,power,used,total,clock,memory_clock,memory_rate;bool memory_clock_configured;
} gso_hardware;
/* busy is the share of time with requests outstanding, read/write the mean service time in ms; all three are optional in the contract. */
typedef struct { char id[641],model[641];gso_metric total,free,busy,read_ms,write_ms,response_ms,read_rate,write_rate; } gso_disk;
typedef struct {gso_freshness source;char raw_state[641];} gso_service;
#define GSO_MAX_HOME_ENTITIES 16u
enum {GSO_HOME_ON=1,GSO_HOME_OFF=2,GSO_HOME_TEMPERATURE=4,GSO_HOME_OPEN=8,GSO_HOME_CLOSE=16,GSO_HOME_STOP=32,GSO_HOME_HVAC=64,GSO_HOME_BRIGHTNESS=128,GSO_HOME_COLOR=256,GSO_HOME_SPEED=512,GSO_HOME_PRESET=1024,GSO_HOME_MODE=2048,GSO_HOME_HUMIDITY=4096};
/* Vehicle actions travel through the same command path; they are only valid for the car's own lock and refresh entities. */
enum {GSO_HOME_LOCK=8192,GSO_HOME_UNLOCK=16384,GSO_HOME_PRESS=32768};
typedef struct {
    char id[161],name[641],state[641];
    gso_freshness source;
    gso_metric current_temperature,target_temperature,min_temperature,max_temperature,temperature_step;
    unsigned actions;
    gso_metric brightness,kelvin,min_kelvin,max_kelvin,percentage,percentage_step,humidity,min_humidity,max_humidity,remaining;
    char mode[65],modes[8][65],detail_state[641];unsigned mode_count;
} gso_home_entity;
const char *gso_home_action_name(unsigned action);
bool gso_home_action_allowed(const gso_home_entity *entity,unsigned action,uint64_t now);

enum {GSO_VIBE_CODEX=0,GSO_VIBE_CLAUDE=1,GSO_VIBE_PROVIDERS=2};
#define GSO_VIBE_NONE GSO_VIBE_PROVIDERS /* no provider is working or has data: nothing is shown, never a default */
const char *gso_vibe_provider_name(unsigned provider);
/* Session ids prefixed "claude-" come from Claude Code; all others are Codex. */
unsigned gso_vibe_session_provider(const char *id);
typedef struct {int64_t date;char condition[33];gso_metric high,low;} gso_forecast;
/* VIBE client facts from the optional codex.clients: reported=false when the producer does not send them. */
typedef struct {bool reported,online;gso_metric tokens_today;} gso_client;
/* NAS volume capacity from the optional storage object. */
#define GSO_MAX_VOLUMES 2u
typedef struct {char label[25];gso_metric total,free;} gso_volume;
/* NAS host status from the optional nas object (CPU, memory, SoC/GPU and one hard disk temperature); reported=false when the producer does not send it. */
typedef enum {GSO_DISK_UNKNOWN,GSO_DISK_ACTIVE,GSO_DISK_IDLE,GSO_DISK_STANDBY,GSO_DISK_QUIET} gso_disk_state;
typedef enum {GSO_DISK_HEALTH_UNKNOWN,GSO_DISK_HEALTH_GOOD,GSO_DISK_HEALTH_WATCH,GSO_DISK_HEALTH_BAD} gso_disk_health;
typedef struct {bool reported;gso_metric cpu,memory,memory_total,soc,gpu,disk,disk_busy,disk_read_rate,disk_write_rate,emmc_busy,emmc_read_rate,emmc_write_rate,disk_wakes;gso_disk_state disk_state;
 gso_metric disk_hours,disk_reallocated,disk_pending,disk_crc,disk_power_cycles,disk_load_cycles;gso_disk_health disk_health;} gso_nas;
/* Bambu Lab printer job status from the optional printer object; reported=false when the producer does not send it. */
enum {GSO_PRINT_UNKNOWN=0,GSO_PRINT_IDLE,GSO_PRINT_PREPARING,GSO_PRINT_RUNNING,GSO_PRINT_PAUSED,GSO_PRINT_FINISHED,GSO_PRINT_FAILED};
/* Vehicle from the optional car object (Home Assistant Leapmotor integration); reported=false when the producer does not send it. */
enum {GSO_CAR_VEHICLE_UNKNOWN=0,GSO_CAR_DRIVING,GSO_CAR_PARKED};
enum {GSO_CAR_CHARGE_UNKNOWN=0,GSO_CAR_CHARGING,GSO_CAR_PLUGGED,GSO_CAR_UNPLUGGED};
enum {GSO_CAR_LOCK_UNKNOWN=0,GSO_CAR_LOCKED,GSO_CAR_UNLOCKED,GSO_CAR_LOCKING,GSO_CAR_UNLOCKING,GSO_CAR_JAMMED};
enum {GSO_CAR_TYRE_UNKNOWN=0,GSO_CAR_TYRE_OK,GSO_CAR_TYRE_ALARM};
enum {GSO_CAR_OPEN_UNKNOWN=0,GSO_CAR_CLOSED,GSO_CAR_OPEN};
typedef struct {
    bool reported;unsigned vehicle,charge,lock;
    unsigned tyre_alarm,windows,trunk; /* GSO_CAR_TYRE_* and GSO_CAR_OPEN_*; unknown when the producer predates them */
    unsigned actions; /* GSO_HOME_LOCK/UNLOCK/PRESS bits the producer currently offers */
    char name[641],lock_id[161],refresh_id[161];
    int64_t updated_at; /* vehicle's own state time, 0 when unknown */
    gso_metric battery,range,remaining_minutes,cabin,odometer,week,consumption;
    gso_metric tyre[4]; /* bar, front-left, front-right, rear-left, rear-right */
    gso_metric charging_power; /* kW, only while charging */
} gso_car;
typedef struct {
    bool reported;unsigned state;char job[481];
    char thumbnail[17]; /* id of the model picture the producer serves for this job; empty when there is none */
    gso_metric progress,remaining_minutes,layer,layer_total,nozzle,nozzle_target,bed,bed_target,error_code;
} gso_printer;

typedef struct {
    gso_freshness windows,wan,codex,game,home,storage,printer,car_source,nas_source;
    int64_t generated_at;
    uint64_t received_ms;
    char home_epoch[37];
    gso_home_entity home_entities[GSO_MAX_HOME_ENTITIES];
    unsigned home_count;
    gso_hardware cpu,gpu;
    gso_disk disks[GSO_MAX_DISKS];
    unsigned disk_count;
    gso_metric download,upload,fps,latency;
    char game_process[641];
    uint8_t game_icon[24*24*4];bool game_icon_valid;
    char wan_error[641],game_error[641];
    bool rendering;
    gso_service services[4];
    gso_freshness net_details[4];
    int64_t subscription_updated,antiad_updated;
    gso_metric antiad_count,dhcp_leases;
    gso_freshness vpn,weather,ambient;
    gso_metric illuminance;
    gso_metric vpn_remaining,vpn_total,weather_temperature,humidity,pressure,wind,wind_bearing;
 gso_forecast forecast[6],hourly_forecast[12];unsigned forecast_count,hourly_count;
    char vpn_total_origin[641];
    char weather_condition[641],weather_location[641];
    /* VIBE providers share the contract's codex object: [provider][0=5h,1=7d]. */
    gso_metric quota_remaining[GSO_VIBE_PROVIDERS][2];
    int64_t quota_resets[GSO_VIBE_PROVIDERS][2];
    gso_client clients[GSO_VIBE_PROVIDERS];
    gso_volume volumes[GSO_MAX_VOLUMES];
    unsigned volume_count;
    gso_printer print;
    gso_car car;
    gso_nas nas;
    unsigned vibe_provider; /* provider whose task/quota the summary card shows */
    unsigned task_provider; /* provider of the selected running/unknown session */
    char task_state[16];
    char task_title[641];
    gso_metric task_duration_ms;
    int64_t task_started_at;
    gso_metric fps_history[GSO_GAME_HISTORY_POINTS];
    unsigned fps_history_count;
} gso_snapshot;

/* True when `id` is a HOME entity that offers `action`, or the car's lock/refresh entity for a vehicle action, and its source is fresh. */
bool gso_snapshot_action_allowed(const gso_snapshot *model,const char *id,unsigned action,uint64_t now);
/* Commands that physically move something take longer to confirm: covers and the door locks. */
bool gso_home_action_slow(unsigned action);
void gso_snapshot_empty(gso_snapshot *model);
bool gso_snapshot_parse(const char *json,size_t length,uint64_t now_ms,gso_snapshot *out);
#endif
