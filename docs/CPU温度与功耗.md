# CPU温度与封装功耗

GSW0.1.2已在本机Ryzen 9 3900X上读取到真实温度和封装功耗，并经NAS快照渲染到显示端。温度选择LibreHardwareMonitor的Core (Tctl/Tdie)，功耗选择Package，未用TDP、核心功耗相加或利用率估算代替。

## 结构与运行

- 使用LibreHardwareMonitorLib 0.9.6，仅开启CPU采集；不开主板、风扇、GPU、网络、存储和内存SPD探测。
- PawnIO正式签名驱动已安装，安装文件签名已验证。未安装不受限版，未改变Windows安全设置。
- 硬件采样由按需高权限任务GSO STATUS CPU Sensor承担，程序安装在Program Files/GSO STATUS/CpuSensor。普通用户对安装目录只有读/执行权限，对任务只有读/运行权限；不能从桌面目录替换高权限工作进程。
- GSW保留asInvoker，读取按用户SID隔离的固定64字节共享帧；以序号校验避免读到半帧，超过3秒即视为过期，温度/功耗缺失不补0。
- 进程间仅传读数和心跳，不接收硬件控制指令；正常运行不写传感器快照或资源日志。没有GSW心跳约60秒后工作进程退出，下次GSW启动时按需再开。

## 当前电脑

驱动、任务和读数均已配置完成，正常入口仍为“启动NAS模式.cmd”。主卡、WIN详情和游戏页使用同一真实CPU字段。独立探测读数及75秒链路检查见artifacts/verification，长期稳定性仍需后续观察。

## 重装或换目录

私有Windows包包含install-cpu-sensor.ps1、官方原版PawnIO安装文件和许可证。以管理员运行一次脚本即可配置受保护的传感器工作进程；脚本检查安装器签名并复用已运行的PawnIO。普通GSW无需改成强制管理员启动。

本机EnableLUA原本为0，受限计划任务也取得管理员令牌，因此本次不能据此宣称已实测标准用户令牌；代码与任务权限按普通GSW/特权工作进程分离。未改动EnableLUA。

## 依赖来源

- [LibreHardwareMonitor官方仓库](https://github.com/LibreHardwareMonitor/LibreHardwareMonitor)，固定NuGet 0.9.6，源提交3d331e3370efb858411f19511373eff65a218701；MPL-2.0及第三方声明随Windows包提供。
- [PawnIO官方站点](https://pawnio.eu/)，下载和SHA256记录在toolchains.lock.json，安装器原样分发。

## 新增频率（GSW0.1.9）

CPU优先使用LHM核心Clock均值，独立.Clocks共享帧保留原温度/功耗协议；不可用时退回Windows报告频率。GPU和VRAM采用[NVML当前时钟查询](https://docs.nvidia.com/deploy/nvml-api/latest/api/group__nvmlDeviceQueries.html)。RAM本机无实时Clock传感器，显示明确标记的配置DDR频率：1800MHz，对应WMI配置3600MT/s；不把它描述为实时测量。受保护采集任务与普通GSW已更新，未新增驱动。
