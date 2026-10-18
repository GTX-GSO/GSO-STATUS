#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "esp_flash.h"
#include "esp_ota_ops.h"
#include "esp_image_format.h"
#include "driver/i2c_master.h"
#include "driver/temperature_sensor.h"
#include "gss_system.h"
#include "gss_board.h"
#define TAG "gss_system"
#define TASK_SLOTS 48
#define CPU_WINDOW_MAX_US 5000000u
extern bool gss_display_flipped(void);

/* ---- busy share of each core: the idle tasks' run time against wall time ---- */
#if CONFIG_FREERTOS_USE_TRACE_FACILITY && CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS
static bool read_cpu(unsigned percent[2]){
 static TaskStatus_t tasks[TASK_SLOTS];
 static uint32_t last_total,last_idle[2];
 static bool have;
 if(uxTaskGetNumberOfTasks()>TASK_SLOTS)return false;
 uint32_t total=0;
 UBaseType_t count=uxTaskGetSystemState(tasks,TASK_SLOTS,&total);
 uint32_t idle[2]={0,0};
 unsigned found=0;
 for(UBaseType_t i=0;i<count;i++){
  if(strncmp(tasks[i].pcTaskName,"IDLE",4))continue;
  int core=tasks[i].pcTaskName[4]-'0';
  if(core<0||core>1)continue;
  idle[core]=(uint32_t)tasks[i].ulRunTimeCounter;
  found|=1u<<core;
 }
 if(found!=3)return false;
 uint32_t window=total-last_total;
 bool usable=have&&window>0&&window<=CPU_WINDOW_MAX_US;
 if(usable){
  for(int core=0;core<2;core++){
   uint32_t idle_time=idle[core]-last_idle[core];
   unsigned idle_percent=idle_time>=window?100u:(unsigned)((uint64_t)idle_time*100u/window);
   percent[core]=100u-idle_percent;
  }
 }
 last_total=total;
 last_idle[0]=idle[0];
 last_idle[1]=idle[1];
 have=true;
 return usable;
}
#else
static bool read_cpu(unsigned percent[2]){
 (void)percent;
 return false;
}
#endif

/* ---- firmware image size against its partition, read once ---- */
static bool read_storage(gss_system_info *info){
 static bool done,ok;
 static uint32_t flash_total,app_total,app_used;
 if(!done){
  done=true;
  uint32_t flash_size=0;
  const esp_partition_t *running=esp_ota_get_running_partition();
  if(running&&esp_flash_get_size(NULL,&flash_size)==ESP_OK){
   esp_partition_pos_t position={.offset=running->address,.size=running->size};
   esp_image_metadata_t metadata;
   memset(&metadata,0,sizeof(metadata));
   if(esp_image_get_metadata(&position,&metadata)==ESP_OK){
    flash_total=flash_size;
    app_total=running->size;
    app_used=metadata.image_len;
    ok=true;
   }
   else ESP_LOGW(TAG,"image size not readable");
  }
 }
 if(!ok)return false;
 info->flash_total=flash_total;
 info->app_total=app_total;
 info->app_used=app_used;
 return true;
}

/* ---- die temperature ---- */
static bool read_temperature(float *celsius){
 static temperature_sensor_handle_t sensor;
 static bool tried;
 if(!sensor&&!tried){
  tried=true;
  temperature_sensor_config_t config=TEMPERATURE_SENSOR_CONFIG_DEFAULT(20,100);
  if(temperature_sensor_install(&config,&sensor)!=ESP_OK||temperature_sensor_enable(sensor)!=ESP_OK){
   ESP_LOGW(TAG,"temperature sensor not available");
   sensor=NULL;
  }
 }
 return sensor&&temperature_sensor_get_celsius(sensor,celsius)==ESP_OK;
}
void gss_system_read(gss_system_info *info){
 memset(info,0,sizeof(*info));
 info->cpu_valid=read_cpu(info->cpu_percent);
 info->internal_total=(uint32_t)heap_caps_get_total_size(MALLOC_CAP_INTERNAL);
 info->internal_free=(uint32_t)heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
 info->psram_total=(uint32_t)heap_caps_get_total_size(MALLOC_CAP_SPIRAM);
 info->psram_free=(uint32_t)heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
 info->memory_valid=info->internal_total>0;
 info->storage_valid=read_storage(info);
 info->temperature_valid=read_temperature(&info->temperature_c);
 info->uptime_s=(uint32_t)(esp_timer_get_time()/1000000);
}

/* ---- QMI8658A accelerometer ---- */
#define IMU_WHO_AM_I 0x00
#define IMU_CTRL1 0x02
#define IMU_CTRL2 0x03
#define IMU_CTRL7 0x08
#define IMU_RESET 0x60
#define IMU_ACCEL_X_LOW 0x35
#define IMU_ID 0x05
#define IMU_RESET_COMMAND 0xB0
#define IMU_ADDRESS_AUTO_INCREMENT 0x40
#define IMU_ACCEL_2G_125HZ 0x06
#define IMU_ACCEL_ENABLE 0x01
#define IMU_LSB_PER_G 16384.0f
#define IMU_TIMEOUT_MS 20
static i2c_master_bus_handle_t imu_bus;
static i2c_master_dev_handle_t imu_device;
static esp_err_t imu_write(uint8_t reg,uint8_t value){
 uint8_t data[2]={reg,value};
 return i2c_master_transmit(imu_device,data,sizeof(data),IMU_TIMEOUT_MS);
}
static esp_err_t imu_read(uint8_t reg,uint8_t *data,size_t length){
 return i2c_master_transmit_receive(imu_device,&reg,1,data,length,IMU_TIMEOUT_MS);
}
void gss_imu_close(void){
 if(imu_device){
  imu_write(IMU_CTRL7,0x00);
  i2c_master_bus_rm_device(imu_device);
  imu_device=NULL;
 }
 if(imu_bus){
  i2c_del_master_bus(imu_bus);
  imu_bus=NULL;
 }
}
bool gss_imu_open(void){
 if(imu_device)return true;
 i2c_master_bus_config_t bus={.i2c_port=GSO_IMU_I2C_PORT,.sda_io_num=GSO_IMU_SDA_GPIO,.scl_io_num=GSO_IMU_SCL_GPIO,.clk_source=I2C_CLK_SRC_DEFAULT,.glitch_ignore_cnt=7};
 if(i2c_new_master_bus(&bus,&imu_bus)!=ESP_OK){
  imu_bus=NULL;
  return false;
 }
 const uint8_t addresses[2]={GSO_IMU_ADDRESS_PRIMARY,GSO_IMU_ADDRESS_ALTERNATE};
 for(int i=0;i<2&&!imu_device;i++){
  if(i2c_master_probe(imu_bus,addresses[i],IMU_TIMEOUT_MS)!=ESP_OK)continue;
  i2c_device_config_t config={.dev_addr_length=I2C_ADDR_BIT_LEN_7,.device_address=addresses[i],.scl_speed_hz=100000};
  if(i2c_master_bus_add_device(imu_bus,&config,&imu_device)!=ESP_OK){
   imu_device=NULL;
   continue;
  }
  uint8_t id=0;
  if(imu_read(IMU_WHO_AM_I,&id,1)!=ESP_OK||id!=IMU_ID){
   ESP_LOGW(TAG,"0x%02X answers with id 0x%02X, not a QMI8658",addresses[i],id);
   i2c_master_bus_rm_device(imu_device);
   imu_device=NULL;
  }
 }
 if(!imu_device){
  ESP_LOGW(TAG,"no accelerometer found");
  gss_imu_close();
  return false;
 }
 imu_write(IMU_RESET,IMU_RESET_COMMAND);
 vTaskDelay(pdMS_TO_TICKS(30));
 bool ok=imu_write(IMU_CTRL1,IMU_ADDRESS_AUTO_INCREMENT)==ESP_OK&&imu_write(IMU_CTRL2,IMU_ACCEL_2G_125HZ)==ESP_OK&&imu_write(IMU_CTRL7,IMU_ACCEL_ENABLE)==ESP_OK;
 if(!ok){
  ESP_LOGW(TAG,"accelerometer setup failed");
  gss_imu_close();
  return false;
 }
 ESP_LOGI(TAG,"accelerometer ready");
 return true;
}
bool gss_imu_read(gso_accel *sample){
 if(!imu_device)return false;
 uint8_t raw[6];
 if(imu_read(IMU_ACCEL_X_LOW,raw,sizeof(raw))!=ESP_OK)return false;
 float axis[3];
 for(int i=0;i<3;i++)axis[i]=(float)(int16_t)(raw[i*2]|(raw[i*2+1]<<8))/IMU_LSB_PER_G;
 float x=GSO_IMU_SCREEN_X_SIGN*axis[GSO_IMU_SCREEN_X_AXIS];
 float y=GSO_IMU_SCREEN_Y_SIGN*axis[GSO_IMU_SCREEN_Y_AXIS];
 float z=GSO_IMU_SCREEN_Z_SIGN*axis[GSO_IMU_SCREEN_Z_AXIS];
 if(gss_display_flipped()){
  x=-x;
  y=-y;
 }
 *sample=(gso_accel){x,y,z};
 return true;
}
