#pragma once
/* 厂家仓库 p4xc5 README / sdkconfig.defaults 与原厂镜像交叉核对，2026-10-20 新板（P4 v3.2）上屏幕、触摸、C5 Wi-Fi、
 * 加速度计、翻转键均已实测。网页原理图为 R5，实物丝印 R3。
 * 这里只声明接线，不初始化无线、不写 PMIC、不触发 C5 首刷。 */
#define GSO_LCD_RESET_GPIO 26
#define GSO_LCD_BACKLIGHT_GPIO 30
#define GSO_TOUCH_SDA_GPIO 28
#define GSO_TOUCH_SCL_GPIO 29
#define GSO_TOUCH_INT_GPIO 27
#define GSO_C5_SDIO_D0_GPIO 14
#define GSO_C5_SDIO_D1_IRQ_GPIO 15
#define GSO_C5_SDIO_D2_GPIO 16
#define GSO_C5_SDIO_D3_GPIO 17
#define GSO_C5_SDIO_CLK_GPIO 18
#define GSO_C5_SDIO_CMD_GPIO 19
#define GSO_C5_BOOT_GPIO 53
#define GSO_C5_ENABLE_GPIO 54
/* Key that turns the picture by 180 degrees (active low, internal pull-up). */
#define GSO_FLIP_KEY_GPIO 47
/* SY7200AABC 官方建议 EN/PWM 20kHz–1MHz。满亮使用直流高电平。 */
#define GSO_BACKLIGHT_PWM_HZ 20000
#define GSO_BACKLIGHT_PWM_BITS 11
#define GSO_BACKLIGHT_PWM_MAX ((1U << GSO_BACKLIGHT_PWM_BITS)-1)
/* Accelerometer: SC7A20 (address 0x18 with SA0 low or 0x19 with SA0 high, WHO_AM_I 0x11, INT1 on GPIO32, not used). The schematic
   lists a QMI8658A, but the vendor measured an SC7A20 on this board generation and the QMI8658A never answered here, so only the
   SC7A20 is supported. It sits on I2C_SDA0/I2C_SCL0 = GPIO7/GPIO8, shared with the camera and audio codec. The touch panel uses
   I2C port 0, this bus uses port 1. The axis table maps the sensor's x,y,z to the screen's right, up and out of the glass
   (checked on the board with the level page, 0.7.31). */
#define GSO_IMU_SDA_GPIO 7
#define GSO_IMU_SCL_GPIO 8
#define GSO_IMU_I2C_PORT 1
#define GSO_IMU_ADDRESS_LOW 0x18
#define GSO_IMU_ADDRESS_HIGH 0x19
#define GSO_IMU_SCREEN_X_AXIS 0
#define GSO_IMU_SCREEN_X_SIGN 1
#define GSO_IMU_SCREEN_Y_AXIS 1
#define GSO_IMU_SCREEN_Y_SIGN 1
#define GSO_IMU_SCREEN_Z_AXIS 2
#define GSO_IMU_SCREEN_Z_SIGN 1
