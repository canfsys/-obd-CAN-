// ============================================================
// pages/detail/detail.js
// 车辆详情页 — 位置、实时状态（轮询15s）、历史数据（真实接口+缓存）
// ============================================================

Page({
  data: {
    loading: true,

    // --- 车辆位置 ---
    lat: 0, lng: 0, speed: 0, altitude: 0, correctedAltitude: 0,
    updateTime: 0, updateTimeStr: "--",
    satellites: 0, hdop: 0,
    locationValid: true,
    gpsStatusText: "等待定位中...",

    // --- 实时状态原始值 ---
    door: "00000", window: "0000",
    // --- 实时状态显示值（预计算）---
    displayVoltage: "--", displayTemperature: "--",
    displayIgnition: "--", displayRpm: "--",
    displayThrottle: "--", displayBrake: "--",
    displayHandbrake: "--", displayLight: "--",
    displayWheelSpeed: "--",
    carStateIcon: "/images/car_state/online.png",
    displayCarState: "online",
    displayCarStateText: "车辆在线",
    carStateClass: "car-state-online",

    // --- 历史图表 ---
    chartType: "voltage",
    timeRangeOptions: ["近24小时", "近7天", "近30天"],
    timeRangeIndex: 0,
    historyList: [],
    historyDisplayList: [],
  },

  // ===========================
  // 生命周期
  // ===========================
  onLoad() {
    // 注册回调：首次全局数据就绪时立即更新
    getApp().onCarDataReady((carInfo) => {
      console.log("detail 收到首次数据就绪通知");
      this.applyCarData(carInfo);
    });

    this.loadData();
  },

  onShow() {
    console.log("detail onShow — 从全局数据同步 + 恢复轮询");
    this.syncFromGlobal();
    this.startPolling();
  },

  onHide() {
    console.log("detail onHide — 暂停轮询（节省云资源）");
    this.stopPolling();
  },

  onUnload() {
    this.stopPolling();
  },

  // ===========================
  // 定时轮询（15 秒，从 globalData 读取）
  // ===========================
  startPolling() {
    if (this.timer) return;
    this.timer = setInterval(() => {
      this.syncFromGlobal();
    }, 15000);
  },

  stopPolling() {
    if (this.timer) {
      clearInterval(this.timer);
      this.timer = null;
    }
  },

  // ===========================
  // ★ 从全局缓存同步数据（不再直接发起 HTTP 请求）
  // ===========================
  syncFromGlobal() {
    const carInfo = getApp().globalData.carInfo;
    if (carInfo) {
      this.applyCarData(carInfo);
    }
  },

  // ===========================
  // ★ 数据加载入口
  // ===========================
  async loadData() {
    this.setData({ loading: true });

    this.syncFromGlobal();
    this.fetchHistoryData();

    this.setData({ loading: false });

    setTimeout(() => {
      this.initChart();
    }, 300);
  },

  // ===========================
  // ★ 应用车辆数据到页面（从 globalData 读取）
  // ===========================
  applyCarData(d) {
    const ts = d.updateTime || d.time || Math.floor(Date.now() / 1000);
    const locationValid = d.locationValid !== false && d.lat != null;
    const gcj02Lat = d.gcj02_lat || d.lat || this.data.lat;
    const gcj02Lng = d.gcj02_lng || d.lng || this.data.lng;
    this.setData({
      locationValid: locationValid,
      lat: gcj02Lat, lng: gcj02Lng,
      speed: locationValid ? (d.speed || 0) : 0, altitude: d.altitude || 0,
      correctedAltitude: d.correctedAltitude || ((d.altitude || 0) + (d.height || 0)),
      updateTime: ts, updateTimeStr: this.formatTime(ts),
      gpsStatusText: locationValid ? `GPS正常 (${this.formatRelativeTime(ts)})` : `GPS信号暂失 (${this.formatRelativeTime(ts)})`,
      satellites: d.satellites || 0, hdop: d.hdop || 0,
      door: d.door || "00000", window: d.window || "0000",
      displayVoltage: this.fmtVoltage(d.voltage),
      displayTemperature: this.fmtTemperature(d.temperature),
      displayIgnition: this.fmtIgnition(d.ignition),
      displayRpm: this.fmtRpm(d.rpm),
      displayThrottle: this.fmtThrottle(d.throttle),
      displayBrake: this.fmtBrake(d.brake),
      displayHandbrake: this.fmtHandbrake(d.handbrake),
      displayLight: this.fmtLight(d.light),
      displayWheelSpeed: this.fmtWheelSpeed(d.wheelSpeed),
      displayCarState: d.carState || "online",
      carStateIcon: this.getCarStateIcon(d.carState),
      displayCarStateText: this.getCarStateText(d.carState),
      carStateClass: this.getCarStateClass(d.carState)
    });
  },

  // ===========================
  // ★ 获取历史数据（真实接口 + 本地缓存）
  // ===========================
  fetchHistoryData() {
    const hours = this.getTimeRangeHours();
    const cacheKey = "car_history_" + hours + "h";
    const now = Math.floor(Date.now() / 1000);

    const cache = wx.getStorageSync(cacheKey);
    if (cache && cache.lastTime && cache.cachedHours === hours) {
      const cacheAge = now - cache.lastTime;
      if (cacheAge < 3600) {
        console.log("[缓存] 命中 " + hours + "h 缓存");
        this.applyHistoryData(cache.data);
        return;
      }
    }

    wx.request({
      url: "https://YOUR_ENV_ID.tcloudbase.com/upload/statusHistory?hours=" + hours,
      method: "GET",
      success: (res) => {
        const list = res.data.data || [];
        const filtered = list.filter(item => item.time && item.voltage != null);
        const enriched = filtered.map(item => ({
          ...item,
          timeStr: this.formatTime(item.time)
        }));
        if (enriched.length > 0) {
          try {
            wx.setStorageSync(cacheKey, {
              data: enriched,
              lastTime: now,
              cachedHours: hours
            });
          } catch (e) {
            console.warn("[缓存] 写入失败:", e);
          }
        }
        this.applyHistoryData(enriched);
      },
      fail: () => {
        const cache = wx.getStorageSync(cacheKey);
        this.applyHistoryData(cache && cache.data ? cache.data : []);
      }
    });
  },

  // ===========================
  // 状态翻译函数
  // ===========================
  fmtIgnition(val) { return val == 1 ? "已点火" : (val == 0 ? "已熄火" : "--"); },
  fmtBrake(val) { return val == 1 ? "已踩下" : (val == 0 ? "未踩下" : "--"); },
  fmtHandbrake(val) { return val == 1 ? "已拉起" : (val == 0 ? "已放下" : "--"); },
  fmtLight(val) { if (val == 0) return "已关闭"; if (val == 1) return "近光"; if (val == 2) return "远光"; return "--"; },
  fmtThrottle(val) { return val != null && val !== undefined ? val + "%" : "--"; },
  fmtRpm(val) { return val != null && val !== undefined ? val + "rpm" : "--"; },
  fmtVoltage(val) { return val != null && val !== undefined ? val + "V" : "--"; },
  fmtTemperature(val) { return val != null && val !== undefined ? val + "\u00B0C" : "--"; },
  fmtWheelSpeed(val) { return val != null && val !== undefined ? val + "km/h" : "--"; },

  getCarStateIcon(state) {
    const icons = {
      online: "/images/car_state/online.png",
      offline: "/images/car_state/offline.png",
      lowpower: "/images/car_state/lowpower.png",
      waiting: "/images/car_state/waiting.png"
    };
    return icons[state] || icons.online;
  },

  getCarStateText(state) {
    const map = {
      online: "车辆在线",
      offline: "车辆已下线",
      lowpower: "电瓶电压低",
      waiting: "车辆待机"
    };
    return map[state] || "未知状态";
  },

  getCarStateClass(state) {
    const map = {
      online: "car-state-online",
      offline: "car-state-offline",
      lowpower: "car-state-lowpower",
      waiting: "car-state-waiting"
    };
    return map[state] || "car-state-online";
  },

  // ===========================
  // ★ 寻车按钮（EMQX REST API）
  // ===========================
  onFindCar() {
    wx.showLoading({ title: "正在寻车..." });

    // 构建 Basic Auth: "app_id:app_secret" 的 Base64
    // 使用微信原生接口编码，避免手写 Base64 的兼容性问题
    const appKey = "a1a196a9";
    const appSecret = "DJhemw1V0.U0e5wC";
    const authStr = appKey + ":" + appSecret;

    let authBase64 = "";
    try {
      // 微信基础库 >= 2.9.3 支持此方法
      const buf = new Uint8Array(authStr.length);
      for (let i = 0; i < authStr.length; i++) {
        buf[i] = authStr.charCodeAt(i);
      }
      authBase64 = wx.arrayBufferToBase64(buf.buffer);
    } catch (e) {
      console.warn("[API] wx.arrayBufferToBase64 失败, 回退到内置编码:", e);
      authBase64 = this._base64Encode(authStr);
    }

    wx.request({
      url: "https://YOUR_MQTT_HOST:8443/api/v5/publish",
      method: "POST",
      header: {
        "Content-Type": "application/json",
        "Authorization": "Basic " + authBase64
      },
      data: {
        topic: "car001down",
        payload: JSON.stringify({ cmd: "horn", timestamp: Math.floor(Date.now() / 1000) }),
        qos: 1
      },
      success: (res) => {
        console.log("[API] EMQX 返回:", res.statusCode, res.data);
        wx.hideLoading();

        if (res.statusCode === 200) {
          wx.showToast({ title: "指令已发送", icon: "success", duration: 2000 });
        } else if (res.statusCode === 202 && res.data && res.data.message === "no_matching_subscribers") {
          wx.showToast({ title: "设备离线，请稍后重试", icon: "none", duration: 3000 });
        } else if (res.statusCode === 401 || res.statusCode === 403) {
          wx.showToast({ title: "API认证失败: " + JSON.stringify(res.data), icon: "none", duration: 4000 });
        } else {
          wx.showToast({ title: "未知响应: " + res.statusCode, icon: "none", duration: 3000 });
        }
      },
      fail: (err) => {
        console.error("[API] EMQX 请求失败:", err);
        wx.hideLoading();
        wx.showToast({ title: "网络请求失败: " + (err.errMsg || "未知错误"), icon: "none", duration: 3000 });
      }
    });
  },

  // 备用 Base64 编码（当 wx.arrayBufferToBase64 不支持时使用）
  _base64Encode(str) {
    const chars = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/=";
    let result = "";
    for (let i = 0; i < str.length; i += 3) {
      const a = str.charCodeAt(i);
      const b = i + 1 < str.length ? str.charCodeAt(i + 1) : 0;
      const c = i + 2 < str.length ? str.charCodeAt(i + 2) : 0;
      result += chars.charAt(a >> 2);
      result += chars.charAt(((a & 3) << 4) | (b >> 4));
      result += chars.charAt(i + 1 < str.length ? ((b & 15) << 2) | (c >> 6) : 64);
      result += chars.charAt(i + 2 < str.length ? c & 63 : 64);
    }
    return result;
  },

  // ===========================
  // 历史数据渲染
  // ===========================
  applyHistoryData(list) {
    const displayList = [].concat(list).reverse();
    this.setData({
      historyList: list,
      historyDisplayList: displayList
    }, () => {
      this.drawChart();
    });
  },

  clearHistoryCache() {
    const hours = this.getTimeRangeHours();
    wx.removeStorageSync("car_history_" + hours + "h");
    this.fetchHistoryData();
  },

  getTimeRangeHours() {
    const rangeMap = [24, 168, 720];
    return rangeMap[this.data.timeRangeIndex] || 24;
  },

  getHistoryInterval() {
    const intervalMap = [10, 60, 240];
    return intervalMap[this.data.timeRangeIndex] || 10;
  },

  onTimeRangeChange(e) {
    const index = parseInt(e.detail.value);
    this.setData({ timeRangeIndex: index });
    this.fetchHistoryData();
  },

  onChartTypeChange(e) {
    const type = e.currentTarget.dataset.type;
    this.setData({ chartType: type }, () => {
      this.drawChart();
    });
  },

  // ===========================
  // ★ Canvas 2D 折线图
  // ===========================
  initChart() {
    const query = wx.createSelectorQuery();
    query.select("#historyChart")
      .fields({ node: true, size: true })
      .exec((res) => {
        if (!res || !res[0] || !res[0].node) return;
        const canvas = res[0].node;
        const ctx = canvas.getContext("2d");
        const dpr = wx.getSystemInfoSync().pixelRatio;
        canvas.width = res[0].width * dpr;
        canvas.height = res[0].height * dpr;
        ctx.scale(dpr, dpr);
        this.canvasRef = canvas;
        this.canvasCtx = ctx;
        this.canvasWidth = res[0].width;
        this.canvasHeight = res[0].height;
        this.drawChart();
      });
  },

  drawChart() {
    if (!this.canvasCtx || !this.canvasWidth) return;
    const ctx = this.canvasCtx;
    const w = this.canvasWidth;
    const h = this.canvasHeight;
    const list = this.data.historyList;
    const chartType = this.data.chartType;

    if (list.length < 2) {
      this.drawEmptyChart(ctx, w, h);
      return;
    }

    const values = list.map((item) => {
      if (chartType === "voltage") return item.voltage;
      if (chartType === "fuel") return item.fuel;
      if (chartType === "temperature") return item.temperature;
    });

    const minVal = Math.min(...values);
    const maxVal = Math.max(...values);
    const padding = { top: 20, right: 16, bottom: 30, left: 40 };
    const plotW = w - padding.left - padding.right;
    const plotH = h - padding.top - padding.bottom;

    ctx.clearRect(0, 0, w, h);
    ctx.fillStyle = "#fafafa";
    ctx.fillRect(0, 0, w, h);

    const gridLines = 4;
    ctx.strokeStyle = "#eee";
    ctx.lineWidth = 1;
    ctx.setLineDash([4, 4]);
    for (let i = 0; i <= gridLines; i++) {
      const y = padding.top + (plotH * i) / gridLines;
      ctx.beginPath();
      ctx.moveTo(padding.left, y);
      ctx.lineTo(w - padding.right, y);
      ctx.stroke();
      const val = maxVal - ((maxVal - minVal) * i) / gridLines;
      ctx.setLineDash([]);
      ctx.fillStyle = "#999";
      ctx.font = "10px sans-serif";
      ctx.textAlign = "right";
      ctx.textBaseline = "middle";
      ctx.fillText(
        chartType === "temperature" ? Math.round(val) + "\u00B0" : val.toFixed(1) + (chartType === "voltage" ? "V" : "%"),
        padding.left - 6, y
      );
    }

    const lineColors = { voltage: "#1677ff", fuel: "#fa8c16", temperature: "#f5222d" };
    const color = lineColors[chartType] || "#1677ff";

    ctx.beginPath();
    ctx.strokeStyle = color;
    ctx.lineWidth = 2;
    ctx.setLineDash([]);

    for (let i = 0; i < values.length; i++) {
      const x = padding.left + (plotW * i) / (values.length - 1);
      const ratio = (values[i] - minVal) / (maxVal - minVal || 1);
      const y = padding.top + plotH * (1 - ratio);
      if (i === 0) ctx.moveTo(x, y);
      else ctx.lineTo(x, y);
    }
    ctx.stroke();

    ctx.lineTo(padding.left + plotW, padding.top + plotH);
    ctx.lineTo(padding.left, padding.top + plotH);
    ctx.closePath();
    const gradient = ctx.createLinearGradient(0, padding.top, 0, padding.top + plotH);
    gradient.addColorStop(0, color + "40");
    gradient.addColorStop(1, color + "00");
    ctx.fillStyle = gradient;
    ctx.fill();

    const xLabelCount = Math.min(4, values.length);
    ctx.fillStyle = "#999";
    ctx.font = "9px sans-serif";
    ctx.textAlign = "center";
    ctx.textBaseline = "top";
    for (let i = 0; i < xLabelCount; i++) {
      const idx = Math.floor((i * (values.length - 1)) / (xLabelCount - 1));
      const x = padding.left + (plotW * idx) / (values.length - 1);
      ctx.fillText(this.formatShortTime(list[idx].time), x, h - padding.bottom + 8);
    }
  },

  drawEmptyChart(ctx, w, h) {
    ctx.clearRect(0, 0, w, h);
    ctx.fillStyle = "#fafafa";
    ctx.fillRect(0, 0, w, h);
    ctx.fillStyle = "#ccc";
    ctx.font = "14px sans-serif";
    ctx.textAlign = "center";
    ctx.textBaseline = "middle";
    ctx.fillText("暂无数据", w / 2, h / 2);
  },

  goToMap() {
    wx.navigateBack();
  },

  formatTime(timestamp) {
    if (!timestamp) return "--";
    const d = new Date(timestamp * 1000);
    const M = String(d.getMonth() + 1).padStart(2, "0");
    const D = String(d.getDate()).padStart(2, "0");
    const h = String(d.getHours()).padStart(2, "0");
    const m = String(d.getMinutes()).padStart(2, "0");
    const s = String(d.getSeconds()).padStart(2, "0");
    return M + "-" + D + " " + h + ":" + m + ":" + s;
  },

  // 智能时间显示：今天→时:分，昨天→昨天 时:分，前天→前天 时:分，更早→月-日 时:分
  formatRelativeTime(timestamp) {
    if (!timestamp) return "--";
    const now = new Date();
    const d = new Date(timestamp * 1000);
    const today = new Date(now.getFullYear(), now.getMonth(), now.getDate());
    const date = new Date(d.getFullYear(), d.getMonth(), d.getDate());
    const diffDays = Math.floor((today - date) / (24 * 60 * 60 * 1000));
    const h = String(d.getHours()).padStart(2, "0");
    const m = String(d.getMinutes()).padStart(2, "0");
    const hm = h + ":" + m;
    if (diffDays === 0) return hm;
    if (diffDays === 1) return "昨天 " + hm;
    if (diffDays === 2) return "前天 " + hm;
    const M = String(d.getMonth() + 1).padStart(2, "0");
    const D = String(d.getDate()).padStart(2, "0");
    return M + "-" + D + " " + hm;
  },

  formatShortTime(timestamp) {
    if (!timestamp) return "--";
    const d = new Date(timestamp * 1000);
    if (this.data.timeRangeIndex === 0) {
      return String(d.getHours()).padStart(2, "0") + ":" + String(d.getMinutes()).padStart(2, "0");
    } else {
      return String(d.getMonth() + 1).padStart(2, "0") + "-" + String(d.getDate()).padStart(2, "0");
    }
  },
});