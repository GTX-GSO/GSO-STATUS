#pragma once
#include "lvgl.h"
#include "project_config.h"
void desk_ui_init(void);
void desk_ui_page(int page);
void desk_ui_detail(int item);
void desk_ui_close_detail(void);
unsigned desk_ui_strip_frozen_cards(void); /* test hooks: snapshots standing in for the cards while the strip is dragged */
void desk_ui_test_strip_freeze(int direction);
int desk_ui_test_level_bubble_x(void);
void desk_ui_test_strip_thaw(void);
void desk_ui_cancel_snapshot_animations(void); /* cancel stale cached pixels before theme/navigation changes */
#ifdef GSO_TEST
void desk_ui_test_cache_limit(uint32_t bytes);
#endif
int desk_ui_strip_scroll_x(void);
uint32_t desk_ui_strip_freeze_ms(void); /* how long the last freeze spent taking snapshots */
uint32_t desk_ui_animation_cache_bytes(void);
uint32_t desk_ui_animation_cache_peak_bytes(void);
void desk_ui_pause(void);
int desk_ui_current_page(void);
bool desk_ui_home_power_center(int *x,int *y); /* test hooks: centre of the first visible HOME power button / mode chip */
bool desk_ui_home_mode_center(unsigned chip,int *x,int *y);
bool desk_ui_home_cover_center(unsigned button,int *x,int *y);
bool desk_ui_home_card_heights(int *minimum,int *maximum);
unsigned desk_ui_home_chip_count(const char *label); /* visible mode chips with this label (NULL: all) */ /* 0 = open, 1 = close */
uint32_t desk_ui_root_background(void); /* test hook: screen background (0xRRGGBB) */
uint32_t desk_ui_sidebar_background(void); /* test hook: current sidebar background (0xRRGGBB) */
void desk_ui_vibe_hold(uint32_t ms); /* test hook: how long a gap keeps the last VIBE state (default 20 s) */
int desk_ui_card_slot(unsigned module); /* position of WIN(0), VIBE(1), NET(2), HOME(3) on the main page; -1 if unknown */
bool desk_ui_detail_is_open(void);
bool desk_ui_network_is_visible(void);
void desk_ui_countdown_text(int64_t reset,int64_t now,char *out,size_t length);
int desk_ui_vpn_reset_days(int64_t now_utc,int *month,int *day); /* whole days to the next monthly reset on the 24th */
void desk_ui_game_sample(const char *process_name,bool rendering);
void desk_ui_game_source(bool offline);
bool desk_ui_game_mode_active(void);
void desk_ui_game_view_closed(void);

void desk_ui_game_manual_open(void);
void desk_ui_theme_auto(void);
void desk_ui_theme_set(bool light);
int desk_ui_theme_mode(void);
void desk_ui_theme_mode_set(int mode);
bool desk_ui_solar_day(int year,int month,int day,int minute,int *rise,int *set);
void desk_ui_solar_today(int *rise,int *set);
uint32_t desk_ui_theme_color(uint32_t value);

uint32_t desk_ui_sample_sequence(void);

unsigned desk_ui_brightness_percent(void);
bool desk_ui_print_thumbnail_visible(bool detail); /* test hook: is the model picture on the card (false) or detail page (true) */
