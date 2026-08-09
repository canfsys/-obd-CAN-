// ============================================================
// notify — 订阅消息推送云函数
// action: "register"  — 前端 callFunction 注册 openid
// action: "sendTo"    — 向指定 openid 发送订阅消息
// ============================================================
const cloud = require("wx-server-sdk");
cloud.init({ env: cloud.DYNAMIC_CURRENT_ENV });

const db = cloud.database();

// 模板ID
const TEMPLATE_ID = "YOUR_TEMPLATE_ID";
// 车牌号（占位，用户后续自行修改）
const PLATE_NUMBER = "car001";

// 发送订阅消息
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
      templateId: TEMPLATE_ID,
      // miniprogramState: "developer"  // 测试版；正式发布改为 "formal"
    });
    console.log("[send] 发送成功 touser:", touser, "result:", JSON.stringify(result));
    return { success: true, result };
  } catch (e) {
    // 常见错误: 43101 = 用户未授权/额度已用完
    console.error("[send] 发送失败 touser:", touser, "errMsg:", e.errMsg || e);
    return { success: false, errCode: e.errCode, errMsg: e.errMsg || e };
  }
}

exports.main = async (event) => {
  const wxContext = cloud.getWXContext();
  const openid = wxContext.OPENID;

  // ===== 注册：存储用户 openid =====
  if (event.action === "register") {
    try {
      const existing = await db.collection("subscribers").where({ openid }).limit(1).get();
      if (existing.data && existing.data.length > 0) {
        await db.collection("subscribers").where({ openid }).update({
          data: { lastRegisterTime: Date.now() }
        });
        return { success: true, msg: "已注册", openid };
      }
      await db.collection("subscribers").add({
        data: {
          openid,
          createTime: Date.now(),
          lastRegisterTime: Date.now()
        }
      });
      return { success: true, msg: "注册成功", openid };
    } catch (e) {
      console.error("[register] 失败:", e);
      return { success: false, errMsg: e };
    }
  }

  // ===== 向指定 openid 发送（alertChecker 调用）=====
  if (event.action === "sendTo") {
    if (!event.touser) return { success: false, msg: "缺少 touser" };
    return await sendSubscribeMessage(event.touser, event.content || "车辆告警");
  }

  return { success: false, msg: "未知 action" };
};