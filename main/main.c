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
static volatile uint32_t vsyncs,touch_irqs;
static uint32_t frames;
static unsigned next_fb=1;
static bool swap_pending;
static gss_present_state present;
static bool flipped;
bool gss_display_flipped(void){return flipped;}
static bool backlight_dc;
static void set_backlight(unsigned percent){
 if(percent>=100){ESP_ERROR_CHECK(ledc_stop(LEDC_LOW_SPEED_MODE,LEDC_CHANNEL_0,1));backlight_dc=true;}
 else {ESP_ERROR_CHECK(ledc_set_duty(LEDC_LOW_SPEED_MODE,LEDC_CHANNEL_0,GSO_BACKLIGHT_PWM_MAX*percent/100));ESP_ERROR_CHECK(ledc_update_duty(LEDC_LOW_SPEED_MODE,LEDC_CHANNEL_0));backlight_dc=false;}
}
static SemaphoreHandle_t swap_done;
static portMUX_TYPE swap_lock=portMUX_INITIALIZER_UNLOCKED;
#ifdef GSO_GSS_BENCHMARK
static uint64_t render_started,render_sum,ppa_sum,wait_sum,total_sum,total_max;
static unsigned profiled;
static uint64_t active_until,event_at,event_sum,event_max;
static unsigned active_frames,event_count;
static uint32_t processing[512];
static int compare_time(const void *a,const void *b){uint32_t x=*(const uint32_t*)a,y=*(const uint32_t*)b;return (x>y)-(x<y);}
static void render_event(lv_event_t *event){(void)event;render_started=esp_timer_get_time();}
static void bench_step(void){
 static unsigned phase;static uint32_t next=15000,started;const int pages[]={10,11,15,16};uint32_t now=esp_timer_get_time()/1000;
 if(phase>24||now<next)return;
 if(phase==0&&!desk_data_connected())return;
 if(phase==0){render_sum=ppa_sum=wait_sum=total_sum=total_max=profiled=0;active_frames=event_count=0;event_sum=event_max=0;started=now;}
 if(phase==24){unsigned n=profiled<512?profiled:512;qsort(processing,n,sizeof(processing[0]),compare_time);
  ESP_LOGI("gso","BENCH frames=%u elapsed_ms=%lu render_avg_us=%llu ppa_avg_us=%llu wait_avg_us=%llu total_avg_us=%llu max_us=%llu",profiled,(unsigned long)(now-started),(unsigned long long)(render_sum/(profiled?profiled:1)),(unsigned long long)(ppa_sum/(profiled?profiled:1)),(unsigned long long)(wait_sum/(profiled?profiled:1)),(unsigned long long)(total_sum/(profiled?profiled:1)),(unsigned long long)total_max);
  ESP_LOGI("gso","BENCH2 active_frames=%u active_window_ms=8400 event_count=%u event_avg_us=%llu event_max_us=%llu p95_us=%lu",active_frames,event_count,(unsigned long long)(event_sum/(event_count?event_count:1)),(unsigned long long)event_max,(unsigned long)(n?processing[((n*95+99)/100)-1]:0));phase++;return;}
 event_at=esp_timer_get_time();active_until=event_at+350000;
 if(phase%2)desk_ui_close_detail();else desk_ui_detail(pages[(phase/2)%4]);phase++;next=now+800;
}
#endif
static uint32_t ticks(void){return esp_timer_get_time()/1000;}
static bool IRAM_ATTR vsync_cb(esp_lcd_panel_handle_t p,esp_lcd_dpi_panel_event_data_t *e,void *u){(void)p;(void)e;(void)u;vsyncs++;return false;}
static bool IRAM_ATTR swap_cb(esp_lcd_panel_handle_t p,esp_lcd_dpi_panel_event_data_t *e,void *u){(void)p;(void)e;(void)u;BaseType_t wake=pdFALSE;xSemaphoreGiveFromISR(swap_done,&wake);return wake==pdTRUE;}
static void IRAM_ATTR touch_irq(axs15260_touch_handle_t t,void *u){(void)t;(void)u;touch_irqs++;}
static void flush(lv_display_t *display,const lv_area_t *area,uint8_t *pixels){
 gss_present_add(&present,(gss_rect){area->x1,area->y1,area->x2,area->y2},W,H);
 if(lv_display_flush_is_last(display)){
#ifdef GSO_GSS_BENCHMARK
  uint64_t wait_started=esp_timer_get_time();
#endif
  /* The swap queued by the previous frame is awaited here, not right after queueing it, so the wait for the panel overlaps
     the rendering of this frame. The framebuffer written next is the one that was on screen until that swap completed. */
  if(swap_pending){
   if(xSemaphoreTake(swap_done,pdMS_TO_TICKS(100))!=pdTRUE){ESP_LOGE("gso","LCD swap timeout");abort();}
   swap_pending=false;next_fb^=1;
  }
#ifdef GSO_GSS_BENCHMARK
  uint64_t ppa_started=esp_timer_get_time();
#endif
  /* Only the areas that differ from what this framebuffer holds are rotated: this frame's and the previous frame's. */
  gss_rect plan[GSS_PRESENT_MAX_PLAN];
  unsigned areas=gss_present_plan(&present,flipped,plan,W,H);
  esp_err_t e=ESP_OK;
  for(unsigned i=0;i<areas&&e==ESP_OK;i++){
   gss_block block=gss_panel_block(plan[i],flipped,W,H);
   ppa_srm_oper_config_t op={.in={.buffer=pixels,.pic_w=W,.pic_h=H,.block_w=block.in_w,.block_h=block.in_h,.block_offset_x=block.in_x,.block_offset_y=block.in_y,.srm_cm=PPA_SRM_COLOR_MODE_RGB565},
    .out={.buffer=framebuffers[next_fb],.buffer_size=FB_BYTES,.pic_w=H,.pic_h=W,.block_offset_x=block.out_x,.block_offset_y=block.out_y,.srm_cm=PPA_SRM_COLOR_MODE_RGB888},
    .rotation_angle=flipped?PPA_SRM_ROTATION_ANGLE_90:PPA_SRM_ROTATION_ANGLE_270,.scale_x=1,.scale_y=1,.mode=PPA_TRANS_MODE_BLOCKING};
   e=ppa_do_scale_rotate_mirror(ppa,&op);
  }
  if(e==ESP_OK){
   /* LCD and its DMA interrupt were allocated on this GUI core. Keep queue update
      and stale-token removal atomic with respect to that ISR. After the DMA
      completion callback, the new buffer is selected and the old one is free. */
   portENTER_CRITICAL(&swap_lock);
   xSemaphoreTake(swap_done,0);
   e=esp_lcd_panel_draw_bitmap(dpi,0,0,H,W,framebuffers[next_fb]);
   portEXIT_CRITICAL(&swap_lock);
   ESP_ERROR_CHECK(e);
   swap_pending=true;frames++;
#ifdef GSO_GSS_BENCHMARK
   uint64_t end=esp_timer_get_time(),total=end-render_started;render_sum+=wait_started-render_started;wait_sum+=ppa_started-wait_started;ppa_sum+=end-ppa_started;total_sum+=total;if(total>total_max)total_max=total;if(profiled<512)processing[profiled]=(uint32_t)total;profiled++;
   if(end<=active_until)active_frames++;
   if(event_at){uint64_t latency=end-event_at;event_sum+=latency;if(latency>event_max)event_max=latency;event_count++;event_at=0;}
#endif
  }else{
   /* The back framebuffer may be half written and was never shown: convert everything for the next two frames. */
   ESP_LOGE("gso","PPA flush %s",esp_err_to_name(e));
   gss_present_invalidate(&present);
  }
 }
 lv_display_flush_ready(display);
}
static void pointer_read(lv_indev_t *indev,lv_indev_data_t *out){
 (void)indev;static lv_point_t point;static bool down;static uint32_t seen;static uint64_t touched;
 uint64_t now=esp_timer_get_time();
 if(touch&&touch_irqs!=seen){seen=touch_irqs;axs15260_touch_data_t data;
  if(axs15260_touch_read(touch,&data)==ESP_OK){down=data.point_num>0&&data.points[0].event!=AXS15260_TOUCH_EVT_UP;
   if(down){int x=data.points[0].y,y=H-1-data.points[0].x;if(flipped){x=W-1-x;y=H-1-y;}point.x=x<0?0:x>=W?W-1:x;point.y=y<0?0:y>=H?H-1:y;touched=now;}
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
 gpio_config_t reset={.pin_bit_mask=1ULL<<26,.mode=GPIO_MODE_OUTPUT};ESP_ERROR_CHECK(gpio_config(&reset));gpio_set_level(26,0);vTaskDelay(pdMS_TO_TICKS(10));gpio_set_level(26,1);vTaskDelay(pdMS_TO_TICKS(170));
 esp_lcd_dsi_bus_handle_t bus;esp_lcd_dsi_bus_config_t bc={.bus_id=0,.num_data_lanes=2,.phy_clk_src=MIPI_DSI_PHY_PLLREF_CLK_SRC_DEFAULT,.lane_bit_rate_mbps=1000};ESP_ERROR_CHECK(esp_lcd_new_dsi_bus(&bc,&bus));
 esp_lcd_panel_io_handle_t io;esp_lcd_dbi_io_config_t dbi={.virtual_channel=0,.lcd_cmd_bits=8,.lcd_param_bits=8};ESP_ERROR_CHECK(esp_lcd_new_panel_io_dbi(bus,&dbi,&io));
 esp_lcd_dpi_panel_config_t dc={.dpi_clk_src=MIPI_DSI_DPI_CLK_SRC_DEFAULT,.dpi_clock_freq_mhz=48,.in_color_format=LCD_COLOR_FMT_RGB888,.out_color_format=LCD_COLOR_FMT_RGB888,.num_fbs=2,
  .video_timing={.h_size=H,.v_size=W,.hsync_back_porch=90,.hsync_pulse_width=10,.hsync_front_porch=90,.vsync_back_porch=10,.vsync_pulse_width=50,.vsync_front_porch=250}};
 axs15260_vendor_config_t vc={.mipi_config={.dsi_bus=bus,.dpi_config=&dc,.lane_num=2},.flags.use_mipi_interface=1};
 esp_lcd_panel_dev_config_t pc={.reset_gpio_num=26,.rgb_ele_order=LCD_RGB_ELEMENT_ORDER_RGB,.bits_per_pixel=24,.vendor_config=&vc};ESP_ERROR_CHECK(esp_lcd_new_panel_axs15260(io,&pc,&panel));ESP_ERROR_CHECK(esp_lcd_panel_init(panel));ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(panel,true));
 dpi=esp_lcd_axs15260_get_dpi_panel(panel);ESP_ERROR_CHECK(esp_lcd_dpi_panel_get_frame_buffer(dpi,2,&framebuffers[0],&framebuffers[1]));
 swap_done=xSemaphoreCreateBinary();assert(swap_done);esp_lcd_dpi_panel_event_callbacks_t callbacks={.on_vsync=vsync_cb,.on_frame_buf_complete=swap_cb};ESP_ERROR_CHECK(esp_lcd_dpi_panel_register_event_callbacks(dpi,&callbacks,NULL));
 ppa_client_config_t ppc={.oper_type=PPA_OPERATION_SRM};ESP_ERROR_CHECK(ppa_register_client(&ppc,&ppa));
 axs15260_touch_config_t tc={.i2c_sda=28,.i2c_scl=29,.rst_gpio=-1,.int_gpio=27,.i2c_port=I2C_NUM_0,.i2c_freq_hz=100000,.x_max=H,.y_max=W};
 esp_err_t e=axs15260_touch_new(&tc,&touch);if(e==ESP_OK){ESP_ERROR_CHECK(axs15260_touch_register_cb(touch,touch_irq,NULL));}else ESP_LOGE("gso","Touch init %s",esp_err_to_name(e));
 gpio_config_t button={.pin_bit_mask=1ULL<<47,.mode=GPIO_MODE_INPUT,.pull_up_en=1};ESP_ERROR_CHECK(gpio_config(&button));
 ESP_LOGI("gso","BOARD_READY LCD_RESET=26 BL=30 SDA=28 SCL=29 INT=27 key=47 DSI=2x1000 pixel=48MHz");
}
static void gui_task(void *unused){
 (void)unused;ESP_LOGI("gso","GSO STATUS SCREEN %s PSRAM=%u",GSO_VERSION,(unsigned)esp_psram_get_size());board_init();
 gss_present_init(&present);lv_init();lv_tick_set_cb(ticks);lv_display_t *display=lv_display_create(W,H);lv_display_set_color_format(display,LV_COLOR_FORMAT_RGB565);
 void *draw=heap_caps_aligned_calloc(128,1,W*H*2,MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);assert(draw);lv_display_set_buffers(display,draw,NULL,W*H*2,LV_DISPLAY_RENDER_MODE_DIRECT);lv_display_set_flush_cb(display,flush);
#ifdef GSO_GSS_BENCHMARK
 lv_display_add_event_cb(display,render_event,LV_EVENT_RENDER_START,NULL);
#endif
 lv_indev_t *input=lv_indev_create();lv_indev_set_type(input,LV_INDEV_TYPE_POINTER);lv_indev_set_read_cb(input,pointer_read);
 gss_wifi_start();desk_brightness_start(NULL);desk_data_start(NULL);desk_ui_init();lv_refr_now(display);gss_wifi_radio_start();
 ESP_LOGI("gso","UI_READY %dx%d frames=%lu vsync=%lu Wi-Fi GSN direct",W,H,(unsigned long)frames,(unsigned long)vsyncs);
 uint64_t report=0;bool key_up=true;uint32_t key_at=0;unsigned last_brightness=999;
 for(;;){
#ifdef GSO_GSS_BENCHMARK
  bench_step();
#endif
  lv_timer_handler();unsigned brightness=desk_ui_brightness_percent();if(brightness!=last_brightness){set_backlight(brightness);last_brightness=brightness;}
  bool up=gpio_get_level(47);if(!up&&key_up&&ticks()-key_at>300){flipped=!flipped;lv_obj_invalidate(lv_screen_active());key_at=ticks();}key_up=up;
  if(esp_timer_get_time()-report>10000000){report=esp_timer_get_time();ESP_LOGI("gso","HEALTH frames=%lu vsync=%lu touch_irq=%lu brightness=%u pwm_duty=%lu internal=%u psram_free=%u source=%d backlight_dc=%d pwm_hz=%u",(unsigned long)frames,(unsigned long)vsyncs,(unsigned long)touch_irqs,last_brightness,(unsigned long)ledc_get_duty(LEDC_LOW_SPEED_MODE,LEDC_CHANNEL_0),(unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),(unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),desk_data_status(),backlight_dc,GSO_BACKLIGHT_PWM_HZ);}
  vTaskDelay(pdMS_TO_TICKS(5));
 }
}
void app_main(void){esp_err_t e=nvs_flash_init();if(e!=ESP_OK){ESP_LOGE("gso","NVS init failed %s; original NVS is not erased",esp_err_to_name(e));}xTaskCreatePinnedToCore(gui_task,"gso_gui",24576,NULL,4,NULL,1);}
