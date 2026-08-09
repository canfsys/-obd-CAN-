# 车辆监控微信小程序

基于 **微信云开发** 的车载终端监控小程序，配合 ESP32 车载终端（`../esp32/`）与 4G 物联网模块（`../iot/`）使用，实时展示车辆位置、行驶状态、历史轨迹与告警推送。

## 功能特性

- 🗺️ **实时位置地图** — 车辆定位、定位精度圈（HDOP）、手机与车辆距离
- 🛤️ **轨迹回放** — 30 秒采样的历史轨迹（`trochoid` 集合），增量合并绘制
- 🚗 **CAN 实时状态** — 车门/车窗/手刹/点火/灯光/空调/油门/转速/安全带等
- 📈 **历史图表** — 电压 / 燃油 / 车内温度曲线（近 24 小时 / 7 天 / 30 天）
- 🔔 **告警推送** — 非法移动 + 异常离线检测，微信订阅消息推送
- ⚡ **资源优化** — 全局 15 秒轮询 + 页面缓存 + 后台暂停，节省云资源

## 目录结构

```
├── miniprogram/                  # 小程序前端
│   ├── app.js                    # 全局轮询器（每15秒拉取车辆数据+告警）
│   ├── app.json / app.wxss
│   └── pages/
│       ├── index/                # 首页：地图、轨迹、告警红点
│       └── detail/               # 详情页：实时状态、历史图表
├── cloudfunctions/               # 云函数
│   ├── upload/                   # 主后端：HTTP 数据接收 /latest /history /alerts
│   ├── alertChecker/             # 定时检测（每20分钟）：异常离线 + 推送
│   └── notify/                   # 订阅消息：openid 注册 / 发送
├── project.config.json           # 小程序项目配置
├── DATA_DOC.md                   # 数据对接文档（字段定义/告警链路/部署）
└── uploadCloudFunction.sh        # 云函数上传脚本
```

## 数据流

```
终端(GPS+CAN) ──HTTP POST──▶ upload 云函数
                                │ 写入
                 ┌──────────────┼────────────────┐
                 ▼              ▼                ▼
              device        can_history      trochoid
             (最新状态)      (完整历史)       (轨迹30s采样)
                                │
                 ┌──────────────┼────────────────┐
                 ▼              ▼                ▼
             alerts          subscribers    alertChecker(定时)
             告警记录          订阅用户          └──▶ notify ──▶ 订阅消息
```

## 部署步骤

1. **导入项目**：微信开发者工具 → 导入 `miniprogram/` 所在目录（本仓库根）
2. **替换占位符**（本仓库已脱敏，部署前需填回你自己的配置）：
   | 占位符 | 说明 | 位置 |
   |--------|------|------|
   | `YOUR_APPID` | 小程序 AppID | `project.config.json` |
   | `YOUR_ENV_ID` | 云开发环境 ID | `miniprogram/app.js`、`cloudfunctions/upload/app.js` |
   | `YOUR_ENV_ID.tcloudbase.com` | 云开发域名 | `miniprogram/app.js`、各页面 |
   | `YOUR_TEMPLATE_ID` | 订阅消息模板 ID | `alertChecker/index.js`、`notify/index.js`、`pages/index/index.js` |
   | `YOUR_MQTT_HOST` | MQTT 服务器地址 | `pages/detail/detail.js` |
3. **开通云开发**：创建环境 → 创建集合 `device`、`can_history`、`trochoid`、`alerts`、`subscribers`
4. **部署云函数**：在开发者工具中对 `upload`、`alertChecker`、`notify` 分别「上传并部署：云端安装依赖」
5. **配置定时触发器**：`alertChecker/config.json` 已配置每 20 分钟触发
6. **添加请求域名**：在微信公众平台 → 开发管理 → 服务器域名，添加云开发域名到 request 合法域名

## 相关文档

- [DATA_DOC.md](DATA_DOC.md) — 完整的数据对接文档（字段定义、数据库集合、告警链路、部署说明）
- ESP32 固件见 `../esp32/`，4G 桥接脚本见 `../iot/`

## 许可证

本项目基于 GPL-3.0 开源，详见根目录 `LICENSE`。
