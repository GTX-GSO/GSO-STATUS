#pragma once
/* 厂家仓库 p4xc5 README / sdkconfig.defaults 与原厂镜像交叉核对。
 * 网页原理图为 R5，实物丝印 R3；C5 接线尚待这块实物验证。
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
/* SY7200AABC 官方建议 EN/PWM 20kHz–1MHz。满亮使用直流高电平。 */
#define GSO_BACKLIGHT_PWM_HZ 20000
#define GSO_BACKLIGHT_PWM_BITS 11
#define GSO_BACKLIGHT_PWM_MAX ((1U << GSO_BACKLIGHT_PWM_BITS)-1)
/* QMI8658A accelerometer (schematic U20): I2C_SDA0/I2C_SCL0 are GPIO7/GPIO8, shared with the camera and audio codec, behind a
   level shifter to the 1.8 V rail. SA0 is pulled up, so the address should be 0x6B; 0x6A is tried as well. The touch panel uses
   I2C port 0, this bus uses port 1. Not yet verified on a board: whether the 1.8 V rail is on without PMIC setup, and how the
   chip is oriented on the PCB. The axis table maps the sensor's x,y,z to the screen's right, up and out of the glass. */
#define GSO_IMU_SDA_GPIO 7
#define GSO_IMU_SCL_GPIO 8
#define GSO_IMU_I2C_PORT 1
#define GSO_IMU_ADDRESS_PRIMARY 0x6B
#define GSO_IMU_ADDRESS_ALTERNATE 0x6A
#define GSO_IMU_SCREEN_X_AXIS 0
#define GSO_IMU_SCREEN_X_SIGN 1
#define GSO_IMU_SCREEN_Y_AXIS 1
#define GSO_IMU_SCREEN_Y_SIGN 1
#define GSO_IMU_SCREEN_Z_AXIS 2
#define GSO_IMU_SCREEN_Z_SIGN 1
