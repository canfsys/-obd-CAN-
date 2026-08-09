// ============================================================
// alertChecker — 定时云函数（每20分钟触发，见 config.json）
// 检测异常离线（超过阈值无数据上传）→ 写入告警 + 推送订阅消息
//
// 阈值说明：
//   - 默认 10 分钟（可修改下方 DEFAULT_OFFLINE_MINUTES）
//   - 也可在 device 集合的文档中添加 offlineAlertMinutes 字段，
//     按设备单独配置，优先于默认值
//
// 推送去重逻辑：
//   - 告警文档带 notified 字段（false=未推送，true=已推送）
//   - 只有不存在「未读且已推送」的告警时才推送，避免重复
//   - 设备恢复上线后（上传 POST）会把旧告警置 read:true / notified:false，
//     下次离线可再次触发
// ============================================================
const cloud = require("wx-server-sdk");
cloud.init({ env: cloud.DYNAMIC_CURRENT_ENV });

const db = cloud.database();

// 订阅消息模板ID
const TEMPLATE_ID = "YOUR_TEMPLATE_ID";
// 车牌号（占位，用户后续自行修改）
const PLATE_NUMBER = "car001";
// 默认离线告警阈值（分钟）
const DEFAULT_OFFLINE_MINUTES = 10;

// 发送订阅消息给指定用户
async function sendSubscribeMessage(touser, content) {
  try {
    const result = await cloud.openapi.subscribeMessage.send({
      touser: touser,
      page: "pages/index/index",
      lang: "zh_CN",
      data: {
        thing2: {
          value: content || "车辆告警"
        },
        car_number6: {
          value: PLATE_NUMBER
        }
      },
      templateId: TEMPLATE_ID
    });
    console.log("[send] 发送成功 touser:", touser, "result:", JSON.stringify(result));
    return { success: true, result };
  } catch (e) {
    // 常见错误: 43101 = 用户未授权/一次性订阅额度已用完
    console.error("[send] 发送失败 touser:", touser, "errMsg:", e.errMsg || e);
    return { success: false, errCode: e.errCode, errMsg: e.errMsg || e };
  }
}

exports.main = async () => {
  const now = Math.floor(Date.now() / 1000);
  const results = {
    devicesChecked: 0,
    offlineCount: 0,
    offlineAlertsCreated: 0,
    pushSucceeded: 0,
    pushFailed: 0
  };

  try {
    // ===== 1. 查询所有已注册的订阅用户 =====
    const subResult = await db.collection("subscribers").get();
    const subscribers = (subResult.data || []).map(s => s.openid);

    // ===== 2. 查询所有设备 =====
    const deviceResult = await db.collection("device").get();
    const devices = deviceResult.data || [];

    for (const device of devices) {
      const deviceId = device.deviceId;
      if (!deviceId) continue;
      results.devicesChecked++;

      const lastDataTime = device.lastDataTime || device.time || 0;
      const elapsed = now - lastDataTime;

      // 阈值：优先读设备配置 offlineAlertMinutes，默认 DEFAULT_OFFLINE_MINUTES
      const thresholdMinutes = device.offlineAlertMinutes || DEFAULT_OFFLINE_MINUTES;
      const thresholdSeconds = thresholdMinutes * 60;

      // 未超时 → 设备正常，跳过
      if (elapsed <= thresholdSeconds) continue;
      results.offlineCount++;

      // ===== 3. 检查是否已为本次离线推送过 =====
      // 存在「已推送」的 offline 告警 → 已推送过，跳过（避免重复推送）
      // 注意：不判断 read，因为用户读不读告警都不应影响去重；
      // 只有当设备恢复上线（POST /upload 重置 notified:false）后，才能再次触发。
      const notifiedAlert = await db.collection("alerts")
        .where({
          deviceId: deviceId,
          type: "abnormal_offline",
          notified: true
        })
        .limit(1)
        .get();

      if (notifiedAlert.data && notifiedAlert.data.length > 0) {
        console.log("[告警] 已推送过，跳过:", deviceId);
        continue;
      }

      // ===== 4. 复用或新建告警记录（notified:false）=====
      let alertDocId = null;
      const existing = await db.collection("alerts")
        .where({
          deviceId: deviceId,
          type: "abnormal_offline",
          notified: false
        })
        .limit(1)
        .get();

      const elapsedMinutes = Math.floor(elapsed / 60);

      if (existing.data && existing.data.length > 0) {
        alertDocId = existing.data[0]._id;
        // 复用已存在的未推送告警，更新时间与离线时长
        await db.collection("alerts").doc(alertDocId).update({
          data: {
            time: now,
            detail: {
              lastDataTime: lastDataTime,
              elapsedSeconds: elapsed,
              elapsedMinutes: elapsedMinutes
            }
          }
        });
      } else {
        const addRes = await db.collection("alerts").add({
          data: {
            deviceId: deviceId,
            type: "abnormal_offline",
            message: "设备异常离线",
            time: now,
            read: false,
            notified: false,
            detail: {
              lastDataTime: lastDataTime,
              elapsedSeconds: elapsed,
              elapsedMinutes: elapsedMinutes
            }
          }
        });
        alertDocId = addRes._id;
        results.offlineAlertsCreated++;
        console.log("[告警] 异常离线:", deviceId, "已离线" + elapsedMinutes + "分钟");
      }

      // ===== 5. 推送订阅消息给所有订阅用户 =====
      // 无订阅用户时不标记 notified，等有新用户订阅后再补推
      if (subscribers.length === 0) {
        console.log("[推送] 无订阅用户，暂不推送:", deviceId);
        continue;
      }

      const content = "设备已离线约" + elapsedMinutes + "分钟";
      let allFailed = true;
      for (const openid of subscribers) {
        const r = await sendSubscribeMessage(openid, content);
        if (r.success) {
          allFailed = false;
          results.pushSucceeded++;
        } else {
          results.pushFailed++;
        }
      }

      // ===== 6. 记录推送结果 =====
      // 全部成功（或至少一个成功）→ notified:true（本次离线不再重复推）
      // 全部失败（如 43101 额度用完）→ notified:false（下次定时器重试，前端可提示重新授权）
      const pushOk = !allFailed;
      await db.collection("alerts").doc(alertDocId).update({
        data: {
          notified: pushOk,
          pushFailed: !pushOk,
          pushTime: now
        }
      });
      console.log("[推送] 结果:", deviceId, pushOk ? "成功" : "失败（将重试）");
    }

    return {
      code: 0,
      msg: "alertChecker done",
      ...results
    };

  } catch (err) {
    console.error("[alertChecker] ERROR:", err);
    return {
      code: 1,
      msg: err.message
    };
  }
};