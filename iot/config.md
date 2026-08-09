# 4G 物联网模块（银尔达 M100PG-DTU 固件版）— 网络通道配置

> 本文档对应银尔达平台「网络通道」配置界面，供 `espBridge.lua` 任务使用。
> ⚠️ 以下均为**示例占位**，部署时请替换为你自己的服务器地址、MQTT 凭据与设备 ID。

## 网络通道 1 — HTTP（数据上报）

| 配置项 | 值 |
|--------|-----|
| 网络通讯协议 | HTTP |
| 绑定通讯串口 | UART |
| 请求方法 | POST |
| 服务器地址 | `https://YOUR_ENV_ID.tcloudbase.com` |
| 服务器端口 | 443 |
| 请求的 URL | `/upload` |
| 等待超时时间 | 30 |
| 自定义头部 | `Content-Type=application/json` |
| 返回数据过滤 | 过滤 |
| 登录注册信息 | 不发送 |
| 支持 IPv6 | 否 |
| 支持 SSL | 无证书加密 |

上报字段定义见小程序端 `DATA_DOC.md`（POST /upload）。

## 网络通道 2 — MQTT（下行指令 / 状态透传）

| 配置项 | 值 |
|--------|-----|
| 网络通讯协议 | MQTT |
| 绑定通讯串口 | UART |
| 心跳包发送间隔 | 60 秒 |
| 服务器地址 | `YOUR_MQTT_HOST` |
| 服务器端口 | 8883 |
| 登录客户端 ID | `YOUR_DEVICE_ID`（如 car001） |
| 登录用户名 | `YOUR_DEVICE_ID` |
| 登录密码 | `YOUR_MQTT_PASSWORD` |
| 协议版本 | 3.1.1 |
| 清除会话 | 离线自动销毁 |
| 订阅消息主题 | `YOUR_DEVICE_IDdown`（如 car001down） |
| 发布消息主题 | `YOUR_DEVICE_IDup`（如 car001up） |
| 支持 SSL | 无证书加密 |

> MQTT 通道用于下行指令（例如远程控制）与透传数据，HTTP 通道用于车辆状态上报。
