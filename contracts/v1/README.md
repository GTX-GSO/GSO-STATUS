# GSO STATUS 数据契约 v1

此目录是 C01 的实现，样例都是显式测试输入，不是当前设备读数。Schema 使用 JSON Schema 2020-12，拒绝未知字段；协议破坏性变更必须升 API 版本。

- `telemetry.schema.json`：POST `/api/v1/agents/{agent_id}/telemetry` 请求体。agent_id 为 1–64 个 ASCII 字母、数字、下划线或连字符。
- `snapshot.schema.json`：GET `/api/v1/snapshot` 响应体。
- `telemetry-partial.json`：真实零负载与不支持的温度并存。
- `telemetry-unavailable.json`、`snapshot-unconfigured.json`：未配置/等待来源。
- `snapshot-offline.json`：保留最后读数，同时明确离线。

每个指标是 `{value, reason}`：有值时 reason=null；无值时 value=null 且 reason 是非空原因码。0 是有效读数，不能代表缺失。容量采用整数 bytes，速率 B/s，温度 °C，功耗 W，负载百分比 0–100，延迟 ms，FPS frames/s。cpu.memory 为系统 RAM；gpu.memory 为所选独立 GPU 的专用显存。antiad_rule_count 是整数计数。磁盘最多三项，以稳定 id 绑定，不假定盘符连续。剩余配额按 bucket 和窗口分钟数区分。

source.observed_at 是该来源最后有效观测的 UTC 时间，source_updated_at 是上游原始更新时间，无则 null。age_ms 是接收端单调时间计算的年龄；未接收过则 null，不能用 generated_at 刷新来源时间。高频来源 age>3000ms 为 stale，>10000ms 为 offline；低频来源由适配器单独配置 TTL。源 ok 不保证所有指标可用。未知 Codex 状态必须为 unknown。

服务状态保留 raw_state，映射器依据实际来源判断，不能把任意非空字符串解释为健康。VPN total_origin 记录订阅或用户配置及单位来源；口径未核对前不从配置总量推导精确已用值。HOME actions 是真实能力白名单；本阶段不提供控制接口。

## 传输与接收约束

服务已实现，以下是 N02 的接收约束，Schema 本身不能代替这些检查：

- 写入和读取分别配置 Bearer token，不把 token 放在 URL、样例或日志中；未配置认证时拒绝启动对外监听。
- 最大请求/响应 32768 bytes；目标快照 <=16384 bytes。先限流读取原始字节，再解析 JSON；拒绝重复键、非有限数、无效 UTF-8。不得仅凭 Content-Length 放行。
- `(agent_id, boot_id, seq)` 标识上报。相同 boot 内 seq 必须严格增加。新 boot 替换活动 boot 后，有界保存已退役 boot，拒绝其迟到上报；拒绝记录过期后仍须按允许时钟偏差检查 sampled_at，避免旧 boot 回滚。具体容量、保留期及时钟偏差在 N02 配置中固定并测试。
- 接收端校验 free<=total、used<=total、实体/磁盘 id 唯一、额度窗口唯一。跨字段约束由实现检查。
- HTTP 200 表示已接受；400 JSON/字段错误，401 缺失或错误凭据，403 写入身份不匹配，409 旧序号/退役 boot，413 超限，415 非 application/json，429 限流。错误体 `{ "error": { "code": "invalid_payload", "message": "..." } }`，不回显凭据或完整遥测。
- `/healthz` 仅表示进程存活；`/readyz` 表示存在有效快照，未配置来源不导致全局失败。未准备时 503。时间戳使用带时区 RFC3339，输出统一 UTC。
- 每个客户端只保留最新模型；无效响应不覆盖最后有效模型。网络任务不调用 LVGL。

## 校验

Python 开发依赖固定在 `contracts/requirements-dev.txt`，仅用于开发，不部署到 NAS/P4。包含 RFC3339 格式校验依赖，防止时间格式检查被静默跳过。

```powershell
python -m pip install --no-user --target work/contract-deps -r contracts/requirements-dev.txt
python scripts/validate-contracts.py
```

校验包含正向样例、真实零与缺失区分、越界值、错误时间、额外字段、无界历史等反向用例。Schema 不验证真实传感器、实时链路或 NAS 资源预算。

## VIBE 会话元数据扩展（GSN 0.1.6 / GSW 0.1.4）

会话 `title` 允许 null，`title_reason` 说明缺失原因。可选 `started_at` 为真实当前 turn 开始时间，`running_duration_ms` 是非负毫秒，非运行状态为 null；`duration_reason` 说明不可用原因。旧生产者省略新增字段仍可接收。标题取自本用户 session_index.jsonl 的 thread_name，不从会话正文推断；会话 id 仍脱敏。

GSN 响应头 `X-GSO-Session-Metadata: 1` 声明此扩展。GSW 首次按旧字段上报，获知支持后才发送新增字段；旧 NAS 上保留 Windows 基础遥测，标题缺失显示“会话名称不可用”，待更新 NAS 后自动启用计时。

`*-vibe-running.json` 是明确的受控测试输入，不是当前真实会话。


## HOME 扩展（GSN 0.1.7 / 显示端 0.5.9）

`home.entities` 为明确绑定的light/climate/cover，最多16项；actions按已观测能力和来源质量生成，未启用独立控制令牌时为空。可选min_temperature_c/max_temperature_c/temperature_step_c沿用指标结构，不填造单位、室温或步长；可选control_epoch为NAS本次启动标识。

`home-command.schema.json` 定义POST请求，含request_id、control_epoch、entity_id、action、expires_at及可选temperature_c。`home-result.schema.json` 定义queued/sent/confirmed/rejected/timed_out/superseded回报；sent仅表示发送，confirmed表示后续HA状态已符合目标。请求ID去重及epoch检查防止重复执行和重启重放。优先停止可替代同一窗帘的待确认开合。

`home-*-example.json`、`snapshot-home.json` 均为受控样例，不含真实家居实体或授权。接入说明见[HOME接入与控制](../../docs/HOME接入与控制.md)。

### 环境光（GSN0.1.19）

可选根字段`ambient`：`source`使用统一来源质量/年龄；`illuminance_lux`使用nullable metric，范围0–200000 lx。无实体/来源异常使用缺失值及reason，不用0代替；旧快照无该字段时自动亮度回退手动。自动调光策略在显示端，GSN只转发经验证的HA光照数据。

## VIBE客户端与NAS存储（GSN0.1.20 / GSW0.1.16）

可选`codex.clients`：至多2项`{id: "codex"|"claude", online: bool, tokens_today: integer|null}`。`tokens_today`为本机当日（本地日历日）的消耗，Claude为输入+缓存写入+输出，Codex为输入（不含缓存读取）+输出；null表示尚未统计完。GSN以响应头`X-GSO-Vibe-Clients: 1`声明支持，GSW获知后才上报。

可选根字段`storage`：`source`使用统一来源质量/年龄（30秒采样，90秒过期、180秒离线）；`volumes`至多2项`{id, label, total_bytes, free_bytes}`，容量为整数bytes的nullable指标，读取失败用reason说明，不用0代替。旧快照省略这两项时显示端显示“未上报/未接入”。

## NAS主机状态（GSN0.1.31）

可选根字段`nas`：`source`同其他来源（5秒采样，20秒过期、45秒离线）；`cpu_percent`、`memory_percent`为0–100的nullable指标，`memory_total_bytes`为整数bytes，`soc_temperature_c`、`gpu_temperature_c`、`disk_temperature_c`为摄氏度。指标缺失时`value`为null并给出`reason`：`warming_up`（CPU首次采样）、`sensor_not_found`、`temperature_invalid`、`source_unavailable`、`not_configured`，硬盘另有`disk_standby`（硬盘休眠，没有读取）、`disk_idle`（盘没有读写，没有读取，也没有可用的旧读数）和`disk_unavailable`（SMART读取失败）。`gpu_temperature_c`仍保留在契约里，界面不显示。7个字段都必填；可选`disk_state`（`active/idle/standby/quiet`，只在配置了硬盘时出现，旧GSN省略时显示端不显示状态）；可选`disk_busy_percent`（0–100）、`disk_read_bytes_per_second`、`disk_write_bytes_per_second`（0.1.29起，取自内核I/O计数器，休眠时为0，不触碰硬盘；缺失时显示端显示“-”）；0.1.31起`disk_state`只用`active`（已唤醒）和`standby`（已休眠），`quiet`表示还没判断出来，另有可选`disk_wake_count_today`（今天的唤醒次数）；0.1.33起可选`disk_health`（`good/watch/bad/unknown`，GSN按硬盘自检数据判定：状态失败、待处理坏扇区≥10或已替换≥100为`bad`，有任何坏扇区为`watch`）和六个非负计数`disk_power_on_hours`（通电小时）、`disk_reallocated_sectors`（已替换坏扇区）、`disk_pending_sectors`（待处理加无法读取的坏扇区）、`disk_crc_errors`（接口传输错误）、`disk_power_cycles`（开关机次数）、`disk_load_cycles`（磁头加载次数），硬盘不支持的计数为`not_supported`；它们来自与温度同一次SMART读取，GSN重启前保留最后一次的值，第一次读取之前这些字段都不出现；可选`emmc_busy_percent`、`emmc_read_bytes_per_second`、`emmc_write_bytes_per_second`（0.1.30起，同样的含义，来自eMMC的块设备计数器）；旧快照省略整个`nas`时显示端不显示NAS卡片。

## 硬盘活动（GSW0.1.17 / 0.1.18）

`windows.disks[]`每项可选三个metric：`busy_percent`（0–100，该时间段内有请求在处理的时间占比）、`read_ms`、`write_ms`（该时间段内完成的读/写请求的平均响应时间，毫秒，≥0）。数据是与上一次采样的差值：首次为`warming_up`，没有读（写）请求时为`disk_no_io`，取不到计数为`disk_perf_unavailable`，不用0代替。GSN以响应头`X-GSO-Disk-Activity: 1`声明支持，GSW获知后才上报；旧GSW省略这三项时显示端显示“-”。GSW0.1.18再增加三个可选metric：`response_ms`（读写合计的平均响应时间）、`read_bytes_per_second`、`write_bytes_per_second`（≥0，该时间段内的平均速率），GSN以响应头`X-GSO-Disk-Throughput: 1`声明支持。

## 拓竹打印机状态（GSN0.1.21）

可选根字段`printer`：GSN作为MQTT客户端订阅打印机局域网模式的`device/+/report`（端口8883，TLS，用户名`bblp`，密码为打印机访问码），只订阅，另外每个会话和每15分钟发一次`pushall`请求完整状态，从不发送控制命令。字段：`source`（统一来源质量；以收到任意数据包为准，30秒过期、90秒离线）；`state`取值`idle/preparing/running/paused/finished/failed/unknown`，由`gcode_state`映射；`job_name`来自`subtask_name`，空则为null；`progress_percent`（0–100）、`remaining_minutes`、`layer`、`layer_total`、`nozzle_c`、`nozzle_target_c`、`bed_c`、`bed_target_c`（0–400）、`error_code`（`print_error`，0为无错误），均为nullable指标，离线或未收到时用reason，不用0代替。访问码只在GSN配置里，不进入快照。可选`thumbnail_id`（16位小写十六进制或null）是GSN当前持有的该任务模型图的编号；图本身不放在快照里（快照上限16KB），而是由`GET /api/v1/printer/thumbnail?id=<编号>`（读令牌）返回：`application/octet-stream`，120×120的原始ARGB8888（每像素按蓝、绿、红、透明度四个字节，非预乘），长度恰为57600字节，响应头`X-GSO-Thumbnail-Id`；没有图或编号过期返回404。图来自打印机`/cache/<任务名>.3mf`里的`Metadata/plate_N.png`（FTPS读取文件开头约256KB，裁到模型再缩放），每个任务只取一次。未配置时`source.quality`为`unconfigured`，旧快照省略该字段时显示端不显示PRINT卡片。

可选根字段`car`：车辆状态，由GSN经Home Assistant的零跑集成读取（`ha.car.entities`按角色绑定实体：state、battery、range、charge、updated、cabin、odometer、week、consumption、lock、refresh，以及可选的tyre_fl、tyre_fr、tyre_rl、tyre_rr四个胎压传感器），复用HA订阅，不另开连接。字段：`source`（统一来源质量）；`name`；`vehicle_state`（`unknown/driving/parked`）；`charge_state`（`unknown/charging/plugged/unplugged`）；`lock_state`（`unknown/locked/unlocked/locking/unlocking/jammed`）；`battery_percent`（0–100）、`range_km`、`remaining_charge_minutes`（仅充电中有值，其余reason为`not_charging`）、`cabin_temperature_c`（按传感器单位换算为摄氏）、`odometer_km`、`week_distance_km`（近7天里程）、`energy_kwh_per_100km`，均为nullable指标；`tyre_fl_bar/tyre_fr_bar/tyre_rl_bar/tyre_rr_bar`（胎压，bar，0–10）、`charging_power_kw`（充电功率，充电电压×电流，仅充电中有值，0–500）也是nullable指标；`tyre_alarm`（`unknown/ok/alarm`）、`windows_state`、`trunk_state`（`unknown/closed/open`）为枚举，只读；`updated_at`为车辆状态更新时间（RFC 3339字符串或null）。`actions`只在GSN配置了`home_control_token`且`ha.car.actions`列出对应动作时才非空：已锁车时为`unlock`，未锁时为`lock`，刷新按钮可用时为`press`；`lock_entity_id`/`refresh_entity_id`是命令里要用的实体。命令走既有的`/api/v1/home/commands`，动作`lock`/`unlock`/`press`不带参数，以HA随后上报的锁状态（或按钮新的按压时间）确认，确认等待30秒。车辆PIN不进入GSN、快照或命令，只留在HA集成选项里。未配置时`car.name`为null、各指标`not_configured`，旧快照省略该字段时显示端不显示CAR卡片。
