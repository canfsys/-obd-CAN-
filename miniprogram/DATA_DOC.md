# 车辆监控小程序 — 数据对接文档

> 本文档说明完整的后端字段定义、数据库集合、接口、告警推送系统及缓存策略，覆盖当前项目全部数据链路。

---

## 🏗️ 系统架构总览

```
┌─────────────┐      ┌──────────────────────┐      ┌─────────────┐
│   终端设备    │ POST │  upload 云函数（HTTP） │      │   小程序前端  │
│  (GPS + CAN) │─────▶│  · 数据接收/字段转换    │      │  每15秒轮询   │
└─────────────┘      │  · 非法移动检测          │      └──────┬──────┘
                     │  · 异常离线检测(latest)   │             │
                     │  · /alerts 查询/已读     │◀────────────┘
                     └──────────┬───────────────┘
                                │ 写入
                 ┌──────────────┼───────────────┐
                 ▼              ▼               ▼
           ┌──────────┐  ┌──────────┐  ┌──────────────┐
           │  device  │  │can_history│  │  trochoid    │
           │ 最新状态  │  │  完整历史  │  │ 轨迹(30s采样) │
           └──────────┘  └──────────┘  └──────────────┘
                                │
                 ┌──────────────┼───────────────┐
                 ▼              ▼               ▼
           ┌──────────┐  ┌──────────┐  ┌──────────────┐
           │  alerts  │  │subscribers│  │  alertChecker│
           │  告警记录 │  │  订阅用户  │  │ 定时云函数     │
           └──────────┘  └──────────┘  └──────────────┘
                                                     │
                                                     ▼
                                            ┌──────────────────┐
                                            │  notify 云函数    │
                                            │ 订阅消息推送        │
                                            └──────────────────┘
```

---

## 📂 文件位置

| 文件 | 说明 |
|------|------|
| `cloudfunctions/upload/app.js` | 主后端：数据接收、协调转换、接口 |
| `cloudfunctions/alertChecker/index.js` | 定时云函数：异常离线检测 + 推送（每20分钟） |
| `cloudfunctions/alertChecker/config.json` | 定时触发器配置 |
| `cloudfunctions/notify/index.js` | 订阅消息云函数：注册 openid / 发送 |
| `miniprogram/app.js` | 全局轮询器（车辆数据 + 告警，每15秒） |
| `miniprogram/pages/index/index.js` | 首页：地图、轨迹、定位精度圈、告警红点 |
| `miniprogram/pages/detail/detail.js` | 详情页：位置、实时状态、历史图表 |

---

## 📊 终端上传字段定义（POST /upload）

### 基础位置字段

| 字段 | 类型 | 必须 | 说明 |
|------|------|:--:|------|
| `deviceId` | String | ✅ | 设备ID（固定 "car001"） |
| `lat` | Number | ✅ | WGS84 纬度 |
| `lng` | Number | ✅ | WGS84 经度 |
| `speed` | Number | | GPS 速度 (km/h) |
| `voltage` | Number | | 电压 (V) |
| `fuel` | Number | | 燃油 (%) |
| `temperature` | Number | | 车内温度 (°C) |
| `mileage` | Number | | 总里程 (km) |
| `direction` | Number | | 方向角 (°) |
| `csq` | Number | | 4G 信号质量 |
| `gps` | Number | | 定位开关 (1=开 0=关) |
| `altitude` | Number | | 原始 GPS 海拔 (m) |
| `height` | Number | | 大地水准面差距 (m) |
| `satellites` | Number | | GPS 卫星数 |
| `hdop` | Number | | 水平精度因子 |
| `fix` | Number | | 定位修复状态 |
| `time` | Number | | Unix 秒时间戳 |

### CAN 总线字段

| 字段 | 类型 | 说明 |
|------|------|------|
| `door` | String | 车门状态 "00000"（0=关 1=开，左前/右前/左后/右后/尾门） |
| `window` | String | 车窗状态 "0000"（0=升 1=降，左前/右前/左后/右后） |
| `handbrake` | Number | 手刹 (1=拉起 0=放下) |
| `ignition` | Number | 点火 (1=点火 0=熄火) |
| `clutch` | Number | 离合器 (1=踩下 0=未踩) |
| `brake` | Number | 刹车 (1=踩下 0=未踩) |
| `light` | Number | 灯光 (0=关 1=近光 2=远光) |
| `ac` | Number | 空调 (1=开 0=关) |
| `cd` | Number | 中控屏 (1=开 0=关) |
| `cdVolume` | Number | 音量 |
| `seatbelt` | Number | 安全带 (1=系上 0=未系) |
| `passenger` | Number | 乘客检测 |
| `throttle` | Number | 油门开度 (%) |
| `rpm` | Number | 发动机转速 (rpm) |
| `wheelSpeed` | Number | 轮速 (km/h) |

### 芯片状态字段

| 字段 | 类型 | 说明 |
|------|------|------|
| `sit1145_1_init` | Number | SIT1145-1 状态 (1=正常 0=故障) |
| `sit1145_2_init` | Number | SIT1145-2 状态 (1=正常 0=故障) |
| `mcp2515_init` | Number | MCP2515 状态 (1=正常 0=故障) |

---

## 📐 后端计算/转换字段

由 `cloudfunctions/upload/app.js` 的 `buildFullData()` 计算生成：

| 字段 | 类型 | 说明 |
|------|------|------|
| `locationValid` | Boolean | 是否收到有效经纬度 |
| `gcj02_lat` / `gcj02_lng` | Number | WGS84 → GCJ02 转换结果（小程序地图用） |
| `correctedAltitude` | Number | **校准后海拔 = altitude + height** |
| `carState` | String | 车辆状态（online/offline/lowpower/waiting） |
| `lastDataTime` | Number | 最后收到数据时间（用于离线检测，每次都更新） |
| `time` | Number | 数据时间戳 |

### ⚠️ 无经纬度时的保护逻辑

终端上传无 `lat`/`lng` 时：

```
POST /upload:
  · device  → update() 保留旧坐标，设 locationValid=false，更新 lastDataTime
  · can_history → 正常写入（CAN 数据仍有价值）
  · trochoid   → 跳过（无轨迹节点）
  · 前端：显示"GPS信号暂失 (上次更新时间)"，地图不跳转
```

---

## 🔌 接口定义

### 接口 1：终端数据上传（POST /upload）

```
POST https://YOUR_ENV_ID.tcloudbase.com/upload
Content-Type: application/json
```

**返回：** `{ "code": 0, "msg": "upload success" }`

**逻辑：**
1. 更新 `device` 集合（始终写入，无位置时保留旧坐标）
2. 写入 `can_history`（每次无条件写入）
3. 写入 `trochoid`（每 30 秒一次，仅当有经纬度）
4. **非法移动检测**：offline 状态且位置变化 >100米 → 写入 `alerts`

---

### 接口 2：获取车辆最新数据（GET /latest）

```
GET .../upload/latest
```

返回 `device` 集合完整文档（含全部计算字段）。

**附加逻辑：** 前端每 15 秒调用此接口：
- 超过 60 秒无数据 → `carState` 标记 offline
- waiting 超过 180 秒 → offline
- **异常离线检测**：超过阈值（默认 10 分钟，见 `upload/app.js` 中 `OFFLINE_ALERT_MINUTES`）无数据 → 写 `alerts`（`notified:false`，由 alertChecker 负责推送）

---

### 接口 3：获取轨迹（GET /history）

```
GET .../upload/history[?since=timestamp]
```

| 参数 | 必填 | 说明 |
|------|:--:|------|
| `since` | 否 | 增量查询：只返回该时间之后的数据（默认24小时内，最多1000条） |

读取 `trochoid` 集合，时间升序。返回含 `gcj02_lat`/`gcj02_lng`。

---

### 接口 4：获取历史状态（GET /statusHistory）

```
GET .../upload/statusHistory?hours=24|168|720
```

读取 `can_history` 集合。返回精简字段：`time` / `voltage` / `fuel` / `temperature`。

---

### 接口 5：查询未读告警（GET /alerts）

```
GET .../upload/alerts?deviceId=car001&since=0
```

**返回：**
```json
{
  "code": 0,
  "data": {
    "unreadCount": 2,
    "alerts": [ { "_id", "deviceId", "type", "message", "time", "read", "detail" } ]
  }
}
```

---

### 接口 6：标记告警已读（POST /alerts/read）

```
POST .../upload/alerts/read
Body: { "ids": ["告警_id1", "告警_id2"] }
```

---

## 🗂️ 数据库集合

### `device`（最新状态，单文档）

| 字段 | 说明 |
|------|------|
| deviceId | 设备ID |
| lat / lng / gcj02_lat / gcj02_lng | WGS84 + GCJ02 坐标 |
| locationValid | 位置有效性 |
| correctedAltitude | 校准后海拔 |
| 全部上传字段 | 见「终端上传字段定义」 |
| lastDataTime | 最后数据时间（离线检测） |
| time | 数据时间戳 |

### `can_history`（完整历史，追加）

同 `device` 全字段，每次上传追加一条。

### `trochoid`（轨迹，30秒采样）

| 字段 | 说明 |
|------|------|
| deviceId / lat / lng | 坐标（WGS84） |
| gcj02_lat / gcj02_lng | GCJ02 坐标 |
| speed / time | 速度 / 时间戳 |

### `alerts`（告警记录）

| 字段 | 类型 | 说明 |
|------|------|------|
| deviceId | String | 设备ID |
| type | String | `illegal_movement`（非法移动）/ `abnormal_offline`（异常离线） |
| message | String | 告警描述 |
| time | Number | 告警时间戳 |
| read | Boolean | 是否已读 |
| detail | Object | 详情（移动距离 / 离线小时数等） |

### `subscribers`（订阅用户）

| 字段 | 类型 | 说明 |
|------|------|------|
| openid | String | 用户 openid |
| createTime / lastRegisterTime | Number | 注册/更新时间 |

---

## 🔔 告警推送系统

### 告警类型与触发

| 类型 | 条件 | 检测位置 | 频率 |
|------|------|---------|------|
| `illegal_movement` | offline 状态位置变化 >100m | `upload` POST | 每次终端上传 |
| `abnormal_offline` | 超过阈值无数据（默认 10 分钟，`alertChecker` 的 `DEFAULT_OFFLINE_MINUTES`，也可在 device 文档加 `offlineAlertMinutes` 按设备配置） | `upload` GET /latest + `alertChecker` 定时器 | 15秒轮询 + 20分钟定时 |

### 推送链路

```
告警产生 → alerts 集合（notified:false 待推送）
  ├─ 微信订阅消息：alertChecker（每20分钟）查「未推送」告警
  │    → cloud.openapi.subscribeMessage.send() 推给所有 subscribers
  │    → 成功置 notified:true（本次离线不再重复推）
  │    → 失败保留 notified:false（下次定时器重试，如 43101 额度用完）
  │    → 无订阅用户时不标记，等有新用户订阅后补推
  └─ 应用内提示：app.js 每15秒 fetchAlerts()
       → 首页红点角标 + wx.showModal 弹窗
       → 点"我知道了" → markAlertsRead → 红点消失
```

**恢复上线自动复位：** 终端恢复上传（POST /upload）时，自动把该设备未读的 `abnormal_offline` 告警置为已读，使下次离线可再次触发告警与推送。

### 订阅消息模板

| 项 | 值 |
|----|----|
| 模板ID | `YOUR_TEMPLATE_ID` |
| 异常名称 | `{{thing2.DATA}}` |
| 车牌号 | `{{car_number6.DATA}}`（当前占位 "car001"，在 notify/alertChecker 顶部 PLATE_NUMBER 修改） |

### 订阅授权流程

```
打开小程序（首页 onShow）→ 检测本地 subscribed 记录
  ├─ 从未授权 → 弹窗"是否开启车辆告警推送？" → 点"开启"
  │              → 微信授权卡片 → 允许 → 记录授权时间/次数 + 注册 openid
  └─ 距上次授权 ≥7 天 → 弹窗"重新授权通知"（额度可能已用完）
        → 点"开启" → 微信授权卡片 → 允许 → 更新授权时间/次数
```

> ⚠️ **一次性订阅限制**：每授权一次仅可接收 1 条订阅消息，发送后额度即耗尽。
> 前端 `subscribed` 本地缓存已改为对象 `{ count, lastTime }`，不再永久标记"已开启"；
> 首页可随时点击"🔔 推送通知 → 点击授权（1次=1条）"补充分额度。

---

## 🎨 前端显示逻辑

### 车辆状态映射

| carState | 图标 | 文本 | 颜色 |
|----------|------|------|------|
| online | online.png | 车辆在线 | 绿 #52c41a |
| offline | offline.png | 车辆已下线 | 灰 #999 |
| lowpower | lowpower.png | 电瓶电压低 | 黄 #faad14 |
| waiting | waiting.png | 车辆待机 | 蓝 #1677ff |

### 定位精度圈（HDOP）

地图 marker 外围半透明蓝色圆，半径 = HDOP × 5 米（范围 10~500m）。

| HDOP | 半径 | 定位质量 |
|------|------|---------|
| 1 | 10m | 高精度 |
| 3 | 15m | 良好 |
| 5 | 25m | 一般 |
| 10 | 50m | 较差 |

### 智能时间显示

`formatRelativeTime()`：今天→`时:分`，昨天→`昨天 时:分`，前天→`前天 时:分`，更早→`月-日 时:分`

---

## 🔄 全局轮询机制（app.js）

```
startGlobalPolling():
  · 立即 fetchCarData() + fetchAlerts()
  · 每 15 秒重复
  · onHide 停止 / onShow 恢复
```

- `globalData.carInfo`：最新车辆数据缓存
- `globalData.unreadAlertCount / alerts`：未读告警缓存
- `onCarDataReady(callback)`：首次数据就绪通知
- `onAlertChange(callback)`：告警变化通知
- `markAlertsRead(ids)`：标记已读

---

## 💰 云资源优化策略

| 优化项 | 说明 |
|--------|------|
| 全局轮询 15 秒 | app.js 统一拉取，页面读取缓存不再各自请求 |
| 后台暂停轮询 | onHide 停止 / onShow 恢复 |
| 历史数据缓存 | can_history 1小时缓存；轨迹增量合并 |
| 正常离线判断 | online 60秒 / waiting 180秒 无数据自动 offline |
| 告警去重 | 同设备 `notified:true` 且未读 → 不再重复推送；推送失败保留 `notified:false` 定时重试 |

---

## 📄 云函数部署说明

| 云函数 | 说明 | 部署方式 |
|--------|------|---------|
| `upload` | HTTP 主后端 | 上传并部署：云端安装依赖 |
| `alertChecker` | 定时检测 + 推送（每20分钟，config.json 配置触发器） | 上传并部署：云端安装依赖 |
| `notify` | 订阅消息注册/发送 | 上传并部署：云端安装依赖 |