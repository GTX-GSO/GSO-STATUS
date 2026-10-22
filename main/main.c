#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_ldo_regulator.h"
#include "esp_timer.h"
#include "esp_psram.h"
#include "nvs_flash.h"
#include "driver/gpio.h"
#include "driver/ledc.h"
#include "driver/ppa.h"
#include "esp_lcd_axs15260.h"
#include "esp_lcd_touch_axs15260.h"
#include "esp_lcd_panel_ops.h"
#include "ui.h"
#include "data_source.h"
#include "display_brightness.h"
#include "gss_build_info.h"
#include "gss_wifi.h"
#include "gss_board.h"
#include "gss_bench_config.h"
#include "gss_present.h"

#define W 1280
#define H 452
#define FB_BYTES (W*H*3)
static esp_lcd_panel_handle_t panel,dpi;
static axs15260_touch_handle_t touch;
static ppa_client_handle_t ppa;
static void *framebuffers[2];
static volatile uint32_t vsyncs,touch_irqs,scanouts;
static uint32_t frames;
static unsigned next_fb=1;
static bool swap_pending;
static uint32_t swap_target;
static gss_present_state present;
static bool flipped=true; /* default: upside-down mounting; GPIO47 still toggles */
bool gss_display_flipped(void){return flipped;}
static bool backlight_dc;
static void set_backlight(unsigned percent){
 if(percent>=100){ESP_ERROR_CHECK(ledc_stop(LEDC_LOW_SPEED_MODE,LEDC_CHANNEL_0,1));backlight_dc=true;}
 else {ESP_ERROR_CHECK(ledc_set_duty(LEDC_LOW_SPEED_MODE,LEDC_CHANNEL_0,GSO_BACKLIGHT_PWM_MAX*percent/100));ESP_ERROR_CHECK(ledc_update_duty(LEDC_LOW_SPEED_MODE,LEDC_CHANNEL_0));backlight_dc=false;}
}
static SemaphoreHandle_t swap_done;
#ifdef GSO_GSS_BENCHMARK
/* Benchmark: 24 page opens/closes, then 8 strip drags by a synthetic finger. Each section reports the cost of a frame (render,
   wait, PPA), how far apart finished frames are while something moves (this includes the main loop's own sleep), and for page
   opens also the time spent building the page before the first frame. */
typedef struct {uint64_t render,wait,ppa,total,total_max,interval,interval_max;unsigned frames,intervals;uint32_t samples[512];} bench_stats;
static bench_stats nav_stats,drag_stats,*bench_now;
static uint64_t render_started,active_until,event_at,event_sum,event_max,build_sum,build_max,last_end;
static unsigned event_count,active_frames;
static uint64_t build_kind_sum[5],build_kind_max[5]; /* opening pages[0..3], closing */
static volatile bool bench_moving;static bool bench_finger;static lv_point_t bench_point;static unsigned freeze_cards;static uint32_t freeze_sum,freeze_max;
static int compare_time(const void *a,const void *b){uint32_t x=*(const uint32_t*)a,y=*(const uint32_t*)b;return (x>y)-(x<y);}
static void render_event(lv_event_t *event){(void)event;render_started=esp_timer_get_time();}
/* render: LVGL drawing the frame; wait: flush waiting for the previous frame's presentation; ppa: rotating it into a panel
   framebuffer, including the wait for the panel to release that buffer; total: from render start until the frame is queued
   on the panel. Called by the present task as each frame finishes. */
static void bench_frame(uint64_t render_start,uint64_t render_end,uint64_t wait,uint64_t ppa_start,uint64_t end){
 bench_stats *s=bench_now;if(!s)return;
 uint64_t total=end-render_start;s->render+=render_end-render_start;s->wait+=wait;s->ppa+=end-ppa_start;s->total+=total;
 if(total>s->total_max)s->total_max=total;
 if(s->frames<512)s->samples[s->frames]=(uint32_t)total;
 s->frames++;
 /* Page opens count frames within 350 ms of the event; drags only while the finger is down or the snap still animates, so
    the idle time after the strip settles is not taken for a slow frame. */
 bool moving=s==&drag_stats?bench_moving:end<=active_until;
 if(moving){
  active_frames++;
  if(last_end){uint64_t gap=end-last_end;s->interval+=gap;s->intervals++;if(gap>s->interval_max)s->interval_max=gap;}
  last_end=end;
 }else last_end=0;
 if(event_at&&end>event_at){uint64_t latency=end-event_at;event_sum+=latency;if(latency>event_max)event_max=latency;event_count++;event_at=0;}
}
static unsigned p95(bench_stats *s){unsigned n=s->frames<512?s->frames:512;qsort(s->samples,n,sizeof(s->samples[0]),compare_time);return n?s->samples[((n*95+99)/100)-1]:0;}
static unsigned long long avg(uint64_t sum,unsigned n){return (unsigned long long)(sum/(n?n:1));}
static void bench_step(void){
 bench_moving=bench_finger||lv_anim_count_running()>0; /* for the present task, which must not call LVGL */
 static unsigned phase,drag,step;static uint32_t next=15000,started;const int pages[]={10,11,15,16};uint32_t now=esp_timer_get_time()/1000;
 if(phase>26||now<next)return;
 if(phase==0&&!desk_data_connected())return;
 if(phase==0){memset(&nav_stats,0,sizeof(nav_stats));active_frames=event_count=0;event_sum=event_max=build_sum=build_max=0;bench_now=&nav_stats;started=now;}
 if(phase<24){
  uint64_t t=esp_timer_get_time();event_at=t;active_until=t+350000;last_end=0;
  if(phase%2)desk_ui_close_detail();else desk_ui_detail(pages[(phase/2)%4]);
  uint64_t build=esp_timer_get_time()-t;build_sum+=build;if(build>build_max)build_max=build;
  unsigned kind=phase%2?4:(phase/2)%4;build_kind_sum[kind]+=build;if(build>build_kind_max[kind])build_kind_max[kind]=build;
  phase++;next=now+800;return;
 }
 bench_stats *s=&nav_stats;
 if(phase==24){
  ESP_LOGI("gso","BENCH frames=%u elapsed_ms=%lu render_avg_us=%llu ppa_avg_us=%llu wait_avg_us=%llu total_avg_us=%llu max_us=%llu",s->frames,(unsigned long)(now-started),avg(s->render,s->frames),avg(s->ppa,s->frames),avg(s->wait,s->frames),avg(s->total,s->frames),(unsigned long long)s->total_max);
  ESP_LOGI("gso","BENCH2 active_frames=%u active_window_ms=8400 event_count=%u event_avg_us=%llu event_max_us=%llu p95_us=%u",active_frames,event_count,avg(event_sum,event_count),(unsigned long long)event_max,p95(s));
  ESP_LOGI("gso","BENCH3 build_avg_us=%llu build_max_us=%llu interval_avg_us=%llu interval_max_us=%llu intervals=%u",avg(build_sum,24),(unsigned long long)build_max,avg(s->interval,s->intervals),(unsigned long long)s->interval_max,s->intervals);
  ESP_LOGI("gso","BENCH4 open%d=%llu/%llu open%d=%llu/%llu open%d=%llu/%llu open%d=%llu/%llu close=%llu/%llu (avg/max us)",pages[0],avg(build_kind_sum[0],3),(unsigned long long)build_kind_max[0],pages[1],avg(build_kind_sum[1],3),(unsigned long long)build_kind_max[1],pages[2],avg(build_kind_sum[2],3),(unsigned long long)build_kind_max[2],pages[3],avg(build_kind_sum[3],3),(unsigned long long)build_kind_max[3],avg(build_kind_sum[4],12),(unsigned long long)build_kind_max[4]);
 bench_now=NULL;desk_ui_page(0);phase++;next=now+1500;return;
 }
 if(phase==25){
  /* One drag: press at x=800 (or 440 going back), 9 moves of 40 px every 30 ms, release, then 900 ms for the snap. */
  if(drag==0&&step==0){memset(&drag_stats,0,sizeof(drag_stats));bench_now=&drag_stats;active_frames=0;event_at=0;}
  int from=drag%2?440:800,dir=drag%2?40:-40;
  if(step==3){uint32_t f=desk_ui_strip_freeze_ms();freeze_cards=desk_ui_strip_frozen_cards();freeze_sum+=f;if(f>freeze_max)freeze_max=f;}
  if(step==0){bench_point.x=from;bench_point.y=60;bench_finger=true;active_until=esp_timer_get_time()+2000000;last_end=0;step++;next=now+60;return;}
  if(step<=9){bench_point.x=from+dir*(int)step;step++;next=now+30;return;}
  if(step==10){bench_finger=false;active_until=esp_timer_get_time()+900000;step++;next=now+900;return;}
  step=0;drag++;next=now+300;
  if(drag==8){phase++;next=now;}
  return;
 }
 s=&drag_stats;
 ESP_LOGI("gso","BENCH_FREEZE avg_ms=%lu max_ms=%lu cards=%u",(unsigned long)(freeze_sum/8),(unsigned long)freeze_max,freeze_cards);
 ESP_LOGI("gso","BENCH_DRAG drags=8 frames=%u render_avg_us=%llu ppa_avg_us=%llu wait_avg_us=%llu total_avg_us=%llu max_us=%llu p95_us=%u interval_avg_us=%llu interval_max_us=%llu intervals=%u page=%d",s->frames,avg(s->render,s->frames),avg(s->ppa,s->frames),avg(s->wait,s->frames),avg(s->total,s->frames),(unsigned long long)s->total_max,p95(s),avg(s->interval,s->intervals),(unsigned long long)s->interval_max,s->intervals,desk_ui_current_page());
 bench_now=NULL;phase++;
}
#endif
static uint32_t ticks(void){return esp_timer_get_time()/1000;}
static bool IRAM_ATTR vsync_cb(esp_lcd_panel_handle_t p,esp_lcd_dpi_panel_event_data_t *e,void *u){(void)p;(void)e;(void)u;vsyncs++;return false;}
/* Each DMA restart rescans the framebuffer the driver has selected at that moment, then counts as one scan-out. */
static bool IRAM_ATTR swap_cb(esp_lcd_panel_handle_t p,esp_lcd_dpi_panel_event_data_t *e,void *u){(void)p;(void)e;(void)u;scanouts++;BaseType_t wake=pdFALSE;xSemaphoreGiveFromISR(swap_done,&wake);return wake==pdTRUE;}
static void IRAM_ATTR touch_irq(axs15260_touch_handle_t t,void *u){(void)t;(void)u;touch_irqs++;}
/* A frame is presented by its own task: PPA rotates the changed areas of LVGL's landscape RGB565 buffer into the back panel
   framebuffer (portrait RGB888) and the panel is told to scan that buffer next. LVGL has two draw buffers, so it renders the
   next frame into the other one meanwhile; flush only waits when the previous presentation is still running. LVGL's own
   wait before copying the changed areas between its buffers is not needed here: that copy only reads the buffer PPA reads. */
typedef struct {const uint8_t *pixels;gss_rect plan[GSS_PRESENT_MAX_PLAN];unsigned areas;bool flipped;uint64_t render_start,render_end,wait,ppa_start,end;} present_job;
static present_job job;
static SemaphoreHandle_t job_ready,job_done;
static bool job_pending;
static volatile bool ppa_failed;
static void present_task(void *unused){
 (void)unused;
 for(;;){
  xSemaphoreTake(job_ready,portMAX_DELAY);
  /* The framebuffer written next is the one that was on screen until the previous swap completed. Tokens given before that
     swap was queued are stale; only the scan-out counter says whether the new buffer is on screen. */
  if(swap_pending){
   while((int32_t)(scanouts-swap_target)<0)if(xSemaphoreTake(swap_done,pdMS_TO_TICKS(100))!=pdTRUE){ESP_LOGE("gso","LCD swap timeout");abort();}
   swap_pending=false;next_fb^=1;
  }
  job.ppa_start=esp_timer_get_time();
  esp_err_t e=ESP_OK;
  uint32_t rows_from=W,rows_to=0;
  for(unsigned i=0;i<job.areas&&e==ESP_OK;i++){
   gss_block block=gss_panel_block(job.plan[i],job.flipped,W,H);
   if(block.out_y<rows_from)rows_from=block.out_y;
   if(block.out_y+block.in_w>rows_to)rows_to=block.out_y+block.in_w;
   ppa_srm_oper_config_t op={.in={.buffer=job.pixels,.pic_w=W,.pic_h=H,.block_w=block.in_w,.block_h=block.in_h,.block_offset_x=block.in_x,.block_offset_y=block.in_y,.srm_cm=PPA_SRM_COLOR_MODE_RGB565},
    .out={.buffer=framebuffers[next_fb],.buffer_size=FB_BYTES,.pic_w=H,.pic_h=W,.block_offset_x=block.out_x,.block_offset_y=block.out_y,.srm_cm=PPA_SRM_COLOR_MODE_RGB888},
    .rotation_angle=job.flipped?PPA_SRM_ROTATION_ANGLE_90:PPA_SRM_ROTATION_ANGLE_270,.scale_x=1,.scale_y=1,.mode=PPA_TRANS_MODE_BLOCKING};
   e=ppa_do_scale_rotate_mirror(ppa,&op);
  }
  if(e==ESP_OK){
   /* draw_bitmap on one of the panel's own framebuffers only selects it and writes back the cache for the given rows. PPA
      already invalidated what it wrote, so the rows it touched are enough. This task and the DMA interrupt both run on the
      GUI core, so a scan-out counted after draw_bitmap returned is a scan of the new buffer; after it the old one is free. */
   e=esp_lcd_panel_draw_bitmap(dpi,0,(int)rows_from,H,(int)rows_to,framebuffers[next_fb]);
   swap_target=scanouts+1;
   ESP_ERROR_CHECK(e);
   swap_pending=true;frames++;
  }else{
   /* The back framebuffer may be half written and was never shown: the next two frames convert everything. */
   ESP_LOGE("gso","PPA flush %s",esp_err_to_name(e));
   ppa_failed=true;
  }
  job.end=esp_timer_get_time();
#ifdef GSO_GSS_BENCHMARK
  if(e==ESP_OK)bench_frame(job.render_start,job.render_end,job.wait,job.ppa_start,job.end);
#endif
  xSemaphoreGive(job_done);
 }
}
static void flush(lv_display_t *display,const lv_area_t *area,uint8_t *pixels){
 gss_present_add(&present,(gss_rect){area->x1,area->y1,area->x2,area->y2},W,H);
 if(lv_display_flush_is_last(display)){
  uint64_t render_end=esp_timer_get_time();
  if(job_pending){
   xSemaphoreTake(job_done,portMAX_DELAY);job_pending=false;
  }
  if(ppa_failed){ppa_failed=false;gss_present_invalidate(&present);}
  /* Only the areas that differ from what the back framebuffer holds are rotated: this frame's and the previous frame's. */
  job.pixels=pixels;job.flipped=flipped;job.areas=gss_present_plan(&present,flipped,job.plan,W,H);
#ifdef GSO_GSS_BENCHMARK
  job.render_start=render_started;
#endif
  job.render_end=render_end;job.wait=esp_timer_get_time()-render_end;
  job_pending=true;xSemaphoreGive(job_ready);
 }
 lv_display_flush_ready(display);
}
/* The controller raises its interrupt on touch changes only, so a held or moving finger is polled every 15 ms like the vendor
   firmware does; waiting for interrupts alone made drags choppy and a lift unnoticed for up to 500 ms. A read that finds no
   finger must repeat before it counts as a lift (an I2C glitch reads as "no finger"), and a position that jumps across the
   screen between two polls is noise, not motion: either one used to start or end a scroll in the middle of a tap. */
static void pointer_read(lv_indev_t *indev,lv_indev_data_t *out){
 (void)indev;static lv_point_t point;static bool down;static uint32_t seen;static uint64_t touched,polled;static int empty;
 uint64_t now=esp_timer_get_time();
#ifdef GSO_GSS_BENCHMARK
 /* The drag section of the benchmark replaces the finger. */
 if(bench_finger||bench_now==&drag_stats){out->point=bench_point;out->state=bench_finger?LV_INDEV_STATE_PRESSED:LV_INDEV_STATE_RELEASED;return;}
#endif
 bool irq=touch&&touch_irqs!=seen,poll=touch&&down&&now-polled>=15000;
 if(irq||poll){seen=touch_irqs;polled=now;axs15260_touch_data_t data;
  if(axs15260_touch_read(touch,&data)==ESP_OK){
   if(data.point_num>0&&data.points[0].event==AXS15260_TOUCH_EVT_UP){down=false;empty=0;}
   else if(data.point_num>0){
    int x=data.points[0].y,y=H-1-data.points[0].x;if(flipped){x=W-1-x;y=H-1-y;}x=x<0?0:x>=W?W-1:x;y=y<0?0:y>=H?H-1:y;
    bool jump=down&&(abs(x-point.x)>400||abs(y-point.y)>400);
    if(!jump){point.x=x;point.y=y;down=true;touched=now;}
    empty=0;
   }
   else if(down&&++empty>=2)down=false;
  }
 }
 if(down&&now-touched>500000)down=false;
 out->point=point;out->state=down?LV_INDEV_STATE_PRESSED:LV_INDEV_STATE_RELEASED;
}
static void board_init(void){
 // Values recovered from the verified AIBOX R3 original image, not from another board's GPIO list.
 esp_ldo_channel_handle_t ldo;esp_ldo_channel_config_t power={.chan_id=3,.voltage_mv=2500};ESP_ERROR_CHECK(esp_ldo_acquire_channel(&power,&ldo));
 ledc_timer_config_t timer={.speed_mode=LEDC_LOW_SPEED_MODE,.timer_num=LEDC_TIMER_0,.duty_resolution=LEDC_TIMER_11_BIT,.freq_hz=GSO_BACKLIGHT_PWM_HZ,.clk_cfg=LEDC_AUTO_CLK};ESP_ERROR_CHECK(ledc_timer_config(&timer));
 ledc_channel_config_t led={.gpio_num=GSO_LCD_BACKLIGHT_GPIO,.speed_mode=LEDC_LOW_SPEED_MODE,.channel=LEDC_CHANNEL_0,.timer_sel=LEDC_TIMER_0,.duty=0};ESP_ERROR_CHECK(ledc_channel_config(&led));
 gpio_config_t reset={.pin_bit_mask=1ULL<<GSO_LCD_RESET_GPIO,.mode=GPIO_MODE_OUTPUT};ESP_ERROR_CHECK(gpio_config(&reset));gpio_set_level(GSO_LCD_RESET_GPIO,0);vTaskDelay(pdMS_TO_TICKS(10));gpio_set_level(GSO_LCD_RESET_GPIO,1);vTaskDelay(pdMS_TO_TICKS(170));
 esp_lcd_dsi_bus_handle_t bus;esp_lcd_dsi_bus_config_t bc=AXS15260_PANEL_BUS_DSI_2CH_CONFIG();ESP_ERROR_CHECK(esp_lcd_new_dsi_bus(&bc,&bus));
 esp_lcd_panel_io_handle_t io;esp_lcd_dbi_io_config_t dbi={.virtual_channel=0,.lcd_cmd_bits=8,.lcd_param_bits=8};ESP_ERROR_CHECK(esp_lcd_new_panel_io_dbi(bus,&dbi,&io));
 esp_lcd_dpi_panel_config_t dc=AXS15260_452_1280_PANEL_60HZ_DPI_CONFIG(LCD_COLOR_FMT_RGB888);dc.num_fbs=2;
 axs15260_vendor_config_t vc={.mipi_config={.dsi_bus=bus,.dpi_config=&dc,.lane_num=2},.flags.use_mipi_interface=1};
 esp_lcd_panel_dev_config_t pc={.reset_gpio_num=GSO_LCD_RESET_GPIO,.rgb_ele_order=LCD_RGB_ELEMENT_ORDER_RGB,.bits_per_pixel=24,.vendor_config=&vc};ESP_ERROR_CHECK(esp_lcd_new_panel_axs15260(io,&pc,&panel));ESP_ERROR_CHECK(esp_lcd_panel_init(panel));ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(panel,true));
 dpi=esp_lcd_axs15260_get_dpi_panel(panel);ESP_ERROR_CHECK(esp_lcd_dpi_panel_get_frame_buffer(dpi,2,&framebuffers[0],&framebuffers[1]));
 swap_done=xSemaphoreCreateBinary();assert(swap_done);esp_lcd_dpi_panel_event_callbacks_t callbacks={.on_vsync=vsync_cb,.on_frame_buf_complete=swap_cb};ESP_ERROR_CHECK(esp_lcd_dpi_panel_register_event_callbacks(dpi,&callbacks,NULL));
 ppa_client_config_t ppc={.oper_type=PPA_OPERATION_SRM};ESP_ERROR_CHECK(ppa_register_client(&ppc,&ppa));
 job_ready=xSemaphoreCreateBinary();job_done=xSemaphoreCreateBinary();assert(job_ready&&job_done);
 /* Above the GUI task so a queued frame starts at once; it spends its time blocked on PPA and the panel. Same core as the DMA interrupt. */
 xTaskCreatePinnedToCore(present_task,"gso_present",4096,NULL,5,NULL,1);
 axs15260_touch_config_t tc={.i2c_sda=GSO_TOUCH_SDA_GPIO,.i2c_scl=GSO_TOUCH_SCL_GPIO,.rst_gpio=-1,.int_gpio=GSO_TOUCH_INT_GPIO,.i2c_port=I2C_NUM_0,.i2c_freq_hz=400000,.x_max=H,.y_max=W};
 esp_err_t e=axs15260_touch_new(&tc,&touch);if(e==ESP_OK){ESP_ERROR_CHECK(axs15260_touch_register_cb(touch,touch_irq,NULL));}else ESP_LOGE("gso","Touch init %s",esp_err_to_name(e));
 gpio_config_t button={.pin_bit_mask=1ULL<<GSO_FLIP_KEY_GPIO,.mode=GPIO_MODE_INPUT,.pull_up_en=1};ESP_ERROR_CHECK(gpio_config(&button));
 ESP_LOGI("gso","BOARD_READY LCD_RESET=%d BL=%d SDA=%d SCL=%d INT=%d key=%d DSI=%dx%d pixel=%dMHz",GSO_LCD_RESET_GPIO,GSO_LCD_BACKLIGHT_GPIO,GSO_TOUCH_SDA_GPIO,GSO_TOUCH_SCL_GPIO,GSO_TOUCH_INT_GPIO,GSO_FLIP_KEY_GPIO,AXS15260_MIPI_LANES,AXS15260_HSCLK_MBPS,AXS15260_DCLK_MHZ);
}
static void gui_task(void *unused){
 (void)unused;ESP_LOGI("gso","GSO STATUS SCREEN %s PSRAM=%u",GSO_VERSION,(unsigned)esp_psram_get_size());board_init();
 gss_present_init(&present);lv_init();lv_tick_set_cb(ticks);lv_display_t *display=lv_display_create(W,H);lv_display_set_color_format(display,LV_COLOR_FORMAT_RGB565);
 void *draw=heap_caps_aligned_calloc(128,1,W*H*2,MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT),*draw2=heap_caps_aligned_calloc(128,1,W*H*2,MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);assert(draw&&draw2);lv_display_set_buffers(display,draw,draw2,W*H*2,LV_DISPLAY_RENDER_MODE_DIRECT);lv_display_set_flush_cb(display,flush);
#ifdef GSO_GSS_BENCHMARK
 lv_display_add_event_cb(display,render_event,LV_EVENT_RENDER_START,NULL);
#endif
 lv_indev_t *input=lv_indev_create();lv_indev_set_type(input,LV_INDEV_TYPE_POINTER);lv_indev_set_read_cb(input,pointer_read);lv_indev_set_scroll_limit(input,24);lv_timer_set_period(lv_indev_get_read_timer(input),10); /* a tap must not scroll: 24 px before a drag starts */
 gss_wifi_start();desk_brightness_start(NULL);desk_data_start(NULL);desk_ui_init();lv_refr_now(display);gss_wifi_radio_start();
 ESP_LOGI("gso","UI_READY %dx%d frames=%lu vsync=%lu Wi-Fi GSN direct",W,H,(unsigned long)frames,(unsigned long)vsyncs);
 uint64_t report=0;bool key_up=true;uint32_t key_at=0;unsigned last_brightness=999;
 for(;;){
#ifdef GSO_GSS_BENCHMARK
  bench_step();
#endif
  uint32_t idle_ms=lv_timer_handler();unsigned brightness=desk_ui_brightness_percent();if(brightness!=last_brightness){set_backlight(brightness);last_brightness=brightness;}
  bool up=gpio_get_level(GSO_FLIP_KEY_GPIO);if(!up&&key_up&&ticks()-key_at>300){flipped=!flipped;lv_obj_invalidate(lv_screen_active());key_at=ticks();}key_up=up;
  if(esp_timer_get_time()-report>10000000){report=esp_timer_get_time();ESP_LOGI("gso","HEALTH frames=%lu vsync=%lu touch_irq=%lu brightness=%u pwm_duty=%lu internal=%u psram_free=%u source=%d backlight_dc=%d pwm_hz=%u",(unsigned long)frames,(unsigned long)vsyncs,(unsigned long)touch_irqs,last_brightness,(unsigned long)ledc_get_duty(LEDC_LOW_SPEED_MODE,LEDC_CHANNEL_0),(unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),(unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),desk_data_status(),backlight_dc,GSO_BACKLIGHT_PWM_HZ);}
  /* Sleep only until LVGL's next timer is due: while something moves a frame takes longer than the 16 ms refresh period, so a
     fixed 5 ms sleep was pure idle between frames. One tick at least, so the idle task still runs. */
  vTaskDelay(pdMS_TO_TICKS(idle_ms<1?1:idle_ms>5?5:idle_ms));
 }
}
void app_main(void){esp_err_t e=nvs_flash_init();if(e!=ESP_OK){ESP_LOGE("gso","NVS init failed %s; original NVS is not erased",esp_err_to_name(e));}xTaskCreatePinnedToCore(gui_task,"gso_gui",24576,NULL,4,NULL,1);}
