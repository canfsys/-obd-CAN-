const http = require("http");
const cloudbase = require("@cloudbase/node-sdk");

// 初始化 CloudBase
const app = cloudbase.init({
    env: "YOUR_ENV_ID"   // 域名
});

const db = app.database();

// 时间戳
function now() {
    return Math.floor(Date.now() / 1000);
}

// 异常离线告警阈值（分钟）
// 与 alertChecker 保持一致：终端超过该时长无数据上传，视为异常离线
const OFFLINE_ALERT_MINUTES = 10;
const OFFLINE_ALERT_SECONDS = OFFLINE_ALERT_MINUTES * 60;

// 控制 trochoid 写入频率（30 秒一次）
let lastTrochoidTime = 0;

// ============================
// WGS84 -> GCJ02 坐标转换
// ============================
function outOfChina(lng, lat) {
    return (
        lng < 72.004 || lng > 137.8347 ||
        lat < 0.8293 || lat > 55.8271
    );
}

function transformLat(x, y) {
    let ret = -100.0 + 2.0 * x + 3.0 * y + 0.2 * y * y + 0.1 * x * y + 0.2 * Math.sqrt(Math.abs(x));
    ret += (20.0 * Math.sin(6.0 * x * Math.PI) + 20.0 * Math.sin(2.0 * x * Math.PI)) * 2.0 / 3.0;
    ret += (20.0 * Math.sin(y * Math.PI) + 40.0 * Math.sin(y / 3.0 * Math.PI)) * 2.0 / 3.0;
    ret += (160.0 * Math.sin(y / 12.0 * Math.PI) + 320 * Math.sin(y * Math.PI / 30.0)) * 2.0 / 3.0;
    return ret;
}

function transformLng(x, y) {
    let ret = 300.0 + x + 2.0 * y + 0.1 * x * x + 0.1 * x * y + 0.1 * Math.sqrt(Math.abs(x));
    ret += (20.0 * Math.sin(6.0 * x * Math.PI) + 20.0 * Math.sin(2.0 * x * Math.PI)) * 2.0 / 3.0;
    ret += (20.0 * Math.sin(x * Math.PI) + 40.0 * Math.sin(x / 3.0 * Math.PI)) * 2.0 / 3.0;
    ret += (150.0 * Math.sin(x / 12.0 * Math.PI) + 300.0 * Math.sin(x / 30.0 * Math.PI)) * 2.0 / 3.0;
    return ret;
}

function wgs84ToGcj02(lng, lat) {
    // 中国境外不转换
    if (outOfChina(lng, lat)) {
        return { lng, lat };
    }
    let dLat = transformLat(lng - 105.0, lat - 35.0);
    let dLng = transformLng(lng - 105.0, lat - 35.0);
    const radLat = lat / 180.0 * Math.PI;
    let magic = Math.sin(radLat);
    magic = 1 - 0.00669342162296594323 * magic * magic;
    const sqrtMagic = Math.sqrt(magic);
    dLat = (dLat * 180.0) / (6335552.717000426 / (magic * sqrtMagic) * Math.PI);
    dLng = (dLng * 180.0) / (6378245 / sqrtMagic * Math.cos(radLat) * Math.PI);
    return { lat: lat + dLat, lng: lng + dLng };
}

// 构建设备数据对象（包含全部 CAN 字段）
// oldCarState: 可传入 DB 中已有的 carState，当新数据未携带时保留旧值
function buildFullData(data, timestamp, oldCarState) {
    // 如果新数据未提供 carState，沿用 DB 中的旧状态；首次写入时默认 "online"
    const carState = data.carState !== undefined ? data.carState : (oldCarState || "online");
    // 计算校准后海拔 = altitude + height
    const altitude = data.altitude || null;
    const height = data.height !== undefined ? data.height : null;
    const correctedAltitude = (altitude != null && height != null) ? altitude + height : null;
    // 判断当前位置是否有效
    const locationValid = (data.lat != null && data.lng != null);
    // WGS84 -> GCJ02 坐标转换（无经纬度时置 null）
    const gcj02 = locationValid ? wgs84ToGcj02(data.lng, data.lat) : { lat: null, lng: null };
    return {
        deviceId: data.deviceId || "unknown",
        lat: data.lat,
        lng: data.lng,
        locationValid: locationValid,
        speed: data.speed || 0,
        voltage: data.voltage != null ? data.voltage : null,
        fuel: data.fuel != null ? data.fuel : null,
        temperature: data.temperature != null ? data.temperature : null,
        mileage: data.mileage != null ? data.mileage : null,
        direction: data.direction != null ? data.direction : null,
        csq: data.csq != null ? data.csq : null,
        gps: data.gps != null ? data.gps : null,
        fix: data.fix != null ? data.fix : null,
        satellites: data.satellites != null ? data.satellites : null,
        altitude: altitude,
        hdop: data.hdop != null ? data.hdop : null,
        door: data.door != null ? data.door : null,
        window: data.window != null ? data.window : null,
        handbrake: data.handbrake !== undefined ? data.handbrake : null,
        ignition: data.ignition !== undefined ? data.ignition : null,
        carState: carState,
        clutch: data.clutch !== undefined ? data.clutch : null,
        brake: data.brake !== undefined ? data.brake : null,
        light: data.light !== undefined ? data.light : null,
        ac: data.ac !== undefined ? data.ac : null,
        cd: data.cd !== undefined ? data.cd : null,
        cdVolume: data.cdVolume !== undefined ? data.cdVolume : null,
        seatbelt: data.seatbelt !== undefined ? data.seatbelt : null,
        passenger: data.passenger !== undefined ? data.passenger : null,
        throttle: data.throttle != null ? data.throttle : null,
        rpm: data.rpm != null ? data.rpm : null,
        wheelSpeed: data.wheelSpeed != null ? data.wheelSpeed : null,
        // GCJ02 坐标系（用于小程序地图显示）
        gcj02_lat: gcj02.lat,
        gcj02_lng: gcj02.lng,
        // 新增字段
        sit1145_1_init: data.sit1145_1_init !== undefined ? data.sit1145_1_init : null,
        sit1145_2_init: data.sit1145_2_init !== undefined ? data.sit1145_2_init : null,
        mcp2515_init: data.mcp2515_init !== undefined ? data.mcp2515_init : null,
        height: height,
        correctedAltitude: correctedAltitude,
        time: timestamp
    };
}

const server = http.createServer(async (req, res) => {

  const pathname = req.url.split("?")[0].replace(/^\/upload/, "");

  console.log("Method:", req.method);
  console.log("URL:", req.url);
  console.log("Path:", pathname);

    // 处理 upload上传接口
    if (req.method === "POST" && (pathname === "" || pathname === "/")) {

        let body = "";

        req.on("data", chunk => {
            body += chunk;
        });

        req.on("end", async () => {

            try {
                const data = JSON.parse(body);

                const deviceId = data.deviceId || "unknown";
                const timestamp = data.time || now();

                // 查询 DB 中已有的 carState（用于新数据未携带时保留旧状态）
                let oldCarState = null;
                try {
                    const oldDoc = await db.collection("device").doc(deviceId).get();
                    if (oldDoc.data && oldDoc.data.carState) {
                        oldCarState = oldDoc.data.carState;
                    }
                } catch (e) {
                    // 文档不存在或查询失败，忽略
                }

                const fullData = buildFullData(data, timestamp, oldCarState);
                const locationValid = (data.lat != null && data.lng != null);

                // 查询旧 device 文档（用于告警判断）
                let oldDevice = null;
                try {
                    oldDevice = await db.collection("device").doc(deviceId).get();
                    oldDevice = oldDevice.data || null;
                } catch (e) { /* 忽略 */ }

                // =========================
                // 1. 更新 device（始终写入）
                //    无经纬度时：保留旧坐标，设 locationValid=false 并更新时间
                // =========================
                // 每次收到数据都更新 lastDataTime
                const nowTs = now();

                if (locationValid) {
                    await db.collection("device")
                        .doc(deviceId)
                        .set({
                            ...fullData,
                            lastDataTime: nowTs
                        });
                } else {
                    // 无有效位置：只更新 CAN 状态字段，保留旧坐标
                    const { lat, lng, gcj02_lat, gcj02_lng, altitude, speed, hdop, satellites, ...restFields } = fullData;
                    await db.collection("device")
                        .doc(deviceId)
                        .update({
                            ...restFields,
                            locationValid: false,
                            correctedAltitude: null,
                            lastDataTime: nowTs
                        });
                }

                // =========================
                // ★ 设备恢复上线 → 复位旧的离线告警
                // =========================
                // 收到新数据说明设备已恢复：标记已读 + 重置 notified:false。
                // 这样 alertChecker 的「notified:true 去重」失效，
                // 下次设备再离线时可重新写入并推送告警。
                try {
                    await db.collection("alerts")
                        .where({
                            deviceId: deviceId,
                            type: "abnormal_offline"
                        })
                        .update({
                            read: true,
                            notified: false
                        });
                } catch (e) {
                    console.error("[告警] 清理旧离线告警失败:", e.message);
                }

                // =========================
                // ★ 告警检测：非法移动
                // =========================
                // 条件：offline 状态下，位置发生变化
                if (locationValid && oldDevice && oldDevice.carState === "offline") {
                    const oldLat = oldDevice.lat || 0;
                    const oldLng = oldDevice.lng || 0;
                    const newLat = data.lat;
                    const newLng = data.lng;
                    // 粗略距离计算（1度≈111km）
                    const dist = Math.sqrt(Math.pow((newLat - oldLat) * 111000, 2) + Math.pow((newLng - oldLng) * 111000, 2));
                    if (dist > 100) {  // 超过 100 米视为移动
                        await db.collection("alerts").add({
                            deviceId: deviceId,
                            type: "illegal_movement",
                            message: "车辆非法移动",
                            time: timestamp,
                            read: false,
                            detail: {
                                fromLat: oldLat,
                                fromLng: oldLng,
                                toLat: newLat,
                                toLng: newLng,
                                distance: Math.round(dist)
                            }
                        });
                        console.log("[告警] 非法移动:", deviceId, dist + "m");
                    }
                }

                // =========================
                // 2. 写入 can_history（完整历史，每次无条件写入）
                // =========================
                await db.collection("can_history")
                    .add(fullData);

                // =========================
                // 3. 写入 trochoid（每 30 秒写一次，仅当有经纬度时）
                // =========================
                if (locationValid && timestamp - lastTrochoidTime >= 30) {
                    const gcj02 = wgs84ToGcj02(data.lng, data.lat);
                    await db.collection("trochoid")
                        .add({
                            deviceId,
                            lat: data.lat,
                            lng: data.lng,
                            speed: data.speed || 0,
                            gcj02_lat: gcj02.lat,
                            gcj02_lng: gcj02.lng,
                            time: timestamp
                        });
                    lastTrochoidTime = timestamp;
                }

                // =========================
                // 返回成功
                // =========================
                res.writeHead(200, {
                    "Content-Type": "application/json"
                });

                res.end(JSON.stringify({
                    code: 0,
                    msg: "upload success"
                }));

            } catch (err) {

                console.error("ERROR:", err);

                res.writeHead(500, {
                    "Content-Type": "application/json"
                });

                res.end(JSON.stringify({
                    code: 1,
                    msg: "server error",
                    error: err.message
                }));
            }
        });

        return;
    }
    // 处理 device 状态接口（超过 60 秒无数据自动设为 offline）
    if (req.method === "GET" && pathname === "/latest") {
      const result = await db.collection("device").where(
        {
          deviceId: "car001"
        }).get();
    
        let data = result.data[0] || null;
        if (data) {
          const now_ts = Math.floor(Date.now()/1000);
          // updateTime 或 time 中取最新的时间戳
          const lastTime = data.updateTime || data.time || 0;
          const elapsed = now_ts - lastTime;
          const currentState = data.carState || "online";

          if (currentState === "lowpower") {
            // lowpower 状态不自动转为 offline
          } else if (currentState === "waiting") {
            // waiting 状态超过 3 分钟(180s)无新数据才转为 offline
            if (elapsed > 180) {
              data.carState = "offline";
            }
          } else {
            // online 及其他状态超过 1 分钟(60s)无新数据转为 offline
            if (elapsed > 60) {
              data.carState = "offline";
            }
          }

          // ★ 异常离线检测（超过阈值无数据 → 写入待推送告警）
          // notified:false 表示还未推送订阅消息，由 alertChecker 定时器负责推送
          if (elapsed > OFFLINE_ALERT_SECONDS) {
            try {
              const existingAlerts = await db.collection("alerts")
                .where({
                  deviceId: "car001",
                  type: "abnormal_offline",
                  notified: false
                })
                .limit(1)
                .get();
              if (!existingAlerts.data || existingAlerts.data.length === 0) {
                const minutes = Math.floor(elapsed / 60);
                await db.collection("alerts").add({
                  deviceId: "car001",
                  type: "abnormal_offline",
                  message: "设备异常离线",
                  time: now_ts,
                  read: false,
                  notified: false,
                  detail: {
                    lastDataTime: lastTime,
                    elapsedSeconds: elapsed,
                    elapsedMinutes: minutes
                  }
                });
                console.log("[告警] 异常离线: car001 已离线" + minutes + "分钟（待推送）");
              }
            } catch (e) {
              console.error("[告警] 写入失败:", e.message);
            }
          }
        }

        res.end(JSON.stringify(
          {
            code: 0,
            data: data
          }));
      return;
    }

    // 处理 trochoid 轨迹接口（支持 ?since=timestamp 增量查询）
    if (req.method === "GET" && pathname.startsWith("/history")) {

      const now_ts = Math.floor(Date.now()/1000);
      const yesterday = now_ts - 24*3600;
      
      const queryUrl = new URL(req.url, "http://localhost");
      const since = parseInt(queryUrl.searchParams.get("since")) || 0;
      const minTime = Math.max(yesterday, since + 1);

      const result = await db.collection("trochoid")
      .where({
      
          deviceId:"car001",
      
          time: db.command.gte(minTime)
      
      })
      .orderBy("time","asc")
      .limit(1000)
      .get();

      res.end(JSON.stringify({
          code: 0,
          data: result.data
      }));
      return;
    }

    // 处理 can_history 状态历史接口
    if (req.method === "GET" && pathname.startsWith("/statusHistory")) {

      const now_ts = Math.floor(Date.now()/1000);
      const queryUrl = new URL(req.url, "http://localhost");
      const hours = parseInt(queryUrl.searchParams.get("hours")) || 24;
      const minTime = now_ts - hours * 3600;

      const result = await db.collection("can_history")
      .where({
          deviceId: "car001",
          time: db.command.gte(minTime)
      })
      .orderBy("time", "asc")
      .limit(1000)
      .get();

      // 只返回前端需要的字段：时间、电压、燃油、温度
      const slimData = result.data.map(item => ({
          time: item.time,
          voltage: item.voltage,
          fuel: item.fuel,
          temperature: item.temperature
      }));

      res.end(JSON.stringify({
          code: 0,
          data: slimData
      }));
      return;
    }

    // 处理 alerts 告警查询接口（返回未读告警数量 + 列表）
    if (req.method === "GET" && pathname === "/alerts") {
      const queryUrl = new URL(req.url, "http://localhost");
      const deviceId = queryUrl.searchParams.get("deviceId") || "car001";
      const since = parseInt(queryUrl.searchParams.get("since")) || 0;

      // 查未读告警
      const unreadResult = await db.collection("alerts")
      .where({
          deviceId: deviceId,
          read: false,
          time: db.command.gte(since)
      })
      .orderBy("time", "desc")
      .limit(20)
      .get();

      res.end(JSON.stringify({
          code: 0,
          data: {
              unreadCount: unreadResult.data.length,
              alerts: unreadResult.data
          }
      }));
      return;
    }

    // 标记告警为已读
    if (req.method === "POST" && pathname === "/alerts/read") {
        let body = "";
        req.on("data", chunk => { body += chunk; });
        req.on("end", async () => {
            try {
                const { ids } = JSON.parse(body);
                if (ids && ids.length > 0) {
                    await db.collection("alerts")
                        .where({
                            _id: db.command.in(ids)
                        })
                        .update({
                            read: true
                        });
                }
                res.end(JSON.stringify({ code: 0, msg: "ok" }));
            } catch (e) {
                res.end(JSON.stringify({ code: 1, msg: e.message }));
            }
        });
        return;
    }

    // 默认接口
    res.writeHead(200, {
        "Content-Type": "application/json"
    });

    res.end(JSON.stringify({
        code: 0,
        msg: "HTTP OK"
    }));
});

server.listen(9000, "0.0.0.0");
