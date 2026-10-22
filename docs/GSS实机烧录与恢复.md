# GSO STATUS SCREEN（GSS）实机

当前源码、应用描述符与`artifacts/GSS`打包版本均为0.7.25-gss，构建位于`build/`；2026-10-20已在新板（ESP32-P4 rev v3.2、16MiB Flash、32MiB PSRAM、COM5）上刷写并启动，除启动外的功能仍待实机验证。旧板为COM4，新板须以实际串口为准。上电后自动连接已保存Wi-Fi并直连GSN，无需电脑端桥接。当前工作顺序见[换板前工作清单](换板前工作清单.md)，网络设置见[P4 Wi-Fi直连](GSS-WiFi接入.md)。

## 编译与升级

`scripts/build-gss.ps1` 使用本机Espressif Installation Manager登记的ESP-IDF6.1、配套Python和GCC。加 `-Flash` 会刷写COM4；串口诊断程序需先关闭。构建和烧录均不写eFuse。

`scripts/package-gss.py` 根据SDK生成的地址打包固件；`artifacts/GSS/flash-manifest.json` 包含地址、长度、SHA256。`artifacts/GSS/flash-firmware.ps1` 校验文件后烧录。交付前须重新构建并打包同版文件；`artifacts/P4`仅保留历史0.7.2实机包。普通升级保留设备网络和亮度配置。

当前单factory应用位于0x20000，15.875MiB；NVS位于0x9000/24KiB，PHY位于0xF000/4KiB，GSN私有配置位于0x10000/24KiB。应用连续使用至16MiB末尾。没有OTA双应用，不整片擦除。GSN配置生成及首次写入见网络文档。

## 日常操作

GPIO47按钮切换横屏180°方向。设置支持自动/手动亮度、日夜主题和Wi-Fi。背光GPIO30、20kHz/11bit，100%使用直流高电平；偏好保存在原NVS。环境光来源丢失时回退保存的手动亮度。

USB只供电、烧录、诊断。已移除旧桥接协议、PC运行脚本和“启动/停止实屏”入口。WIN/VIBE/NET/HOME快照与家电命令均通过Wi-Fi请求GSN。

## 原厂备份与恢复边界

原厂16MiB原始备份只在根目录保留一份（artifacts/GSS的发布包由打包脚本复制），SHA256 `1203ee2cd0248549783611c0265a42774ba6555b7edcf2417fa9f9460b6a23a0`，不可覆盖。原厂factory仅0x10000/1MiB，不能将完整原始备份视为16MiB有效应用。

`scripts/restore-original.ps1` 是整片回写工具，会覆盖GSO和全部配置；此前完整回写在异常区域未通过，因此不作为当前常规升级或必经验证步骤。旧板已寄回；正常板优先使用当前分区表和应用，原厂整片备份仅用于明确需要的恢复。

历史Flash复测报告（0.6.4复测、BOYA板端复测、ROM无压缩复测）保留原始日期和结果，已归档到artifacts/project-history/retired-docs.zip。这些证据不单独证明芯片永久物理损坏，也不代表当前运行版本。
