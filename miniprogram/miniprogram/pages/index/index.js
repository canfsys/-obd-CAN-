Page({
  
  data: {

    // 地图中心
    mapLat: 25.32,
    mapLng: 110.41,

    // 车辆位置
    lat: 25.32,
    lng: 110.41,

    // 是否跟随车辆
    followCar: true,

    // 当前速度
    speed: 0,

    // 手机位置
    myLat: 0,
    myLng: 0,

    // 与车辆距离
    distance: "--",

    // 地图 Marker
    markers: [],

    // 轨迹
    polyline: [],

    // 定位精度圈
    circles: [],

    // 告警
    unreadAlertCount: 0,
    alerts: [],

    // GPS 状态
    gpsStatus: "等待定位中...",

    // 车辆状态（底部卡片）
    carState: "online",
    carStateIcon: "/images/car_state/online.png",
    carStateText: "车辆在线",
    carStateClass: "car-state-online"

  },

  // ===========================
  // 页面加载
  // ===========================
  onLoad() {

    console.log("index onLoad");

    // 注册回调：首次全局数据就绪时立即更新（解决首次进入状态延迟问题）
    getApp().onCarDataReady((carInfo) => {
      console.log("index 收到首次数据就绪通知");
      this.applyCarData(carInfo);
    });
    
    // 首次加载所有数据
    this.loadAllData();
  
    // 页面渲染完成后获取卡片高度
    setTimeout(() => {
      this.getCardHeight();
    }, 500);

    this.isProgramMove = false;

    // ★ 注册告警变更回调
    this._alertCallback = (count, alerts) => {
      this.setData({
        unreadAlertCount: count,
        alerts: alerts
      });
      // 有新告警时弹出提示
      if (count > 0 && alerts.length > 0) {
        this.showAlertPopup(alerts[0]);
      }
    };
    getApp().onAlertChange(this._alertCallback);
  
  },

  onUnload() {
    this.stopPolling();
  },

  // ===========================
  // ★ 告警弹窗（仅弹窗，不请求订阅消息）
  // ===========================
  showAlertPopup(alert) {
    const message = alert.message || "未知告警";
    const typeName = alert.type === "illegal_movement" ? "非法移动" : "异常离线";
    wx.showModal({
      title: "⚠️ " + typeName,
      content: message + (alert.detail && alert.detail.distance ? `，移动距离 ${alert.detail.distance} 米` : ""),
      confirmText: "我知道了",
      showCancel: false,
      success: () => {
        if (alert._id) {
          getApp().markAlertsRead([alert._id]);
        }
      }
    });
  },

  // ===========================
  // ★ 用户点击按钮开启推送通知
  // ===========================
  // 注意：微信一次性订阅消息「授权一次 = 可接收一条通知」
  // 发送一条后额度即耗尽，需用户再次点击授权。
  // 因此这里不再永久标记 subscribed=true，而是记录授权次数与时间，
  // 并始终允许用户随时点击重新授权，保证额度充足。
  onSubscribeNotification() {
    wx.requestSubscribeMessage({
      tmplIds: ["YOUR_TEMPLATE_ID"],
      success: (res) => {
        const status = res["YOUR_TEMPLATE_ID"];
        if (status === "accept") {
          // 授权成功 → 累计授权次数 + 记录时间
          const info = wx.getStorageSync("subscribed") || { count: 0, lastTime: 0 };
          wx.setStorageSync("subscribed", {
            count: (info.count || 0) + 1,
            lastTime: Date.now()
          });
          // 注册 openid 到 notify 云函数（确保订阅用户表有记录）
          wx.cloud.callFunction({
            name: "notify",
            data: { action: "register" },
            success: (regRes) => {
              console.log("[注册] openid 注册成功:", regRes.result);
              wx.showToast({ title: "授权成功（1条通知额度）", icon: "none" });
            },
            fail: (regErr) => {
              console.error("[注册] openid 注册失败:", regErr);
              wx.showToast({ title: "注册失败", icon: "none" });
            }
          });
        } else {
          wx.showToast({ title: "已拒绝通知", icon: "none" });
        }
      },
      fail: (err) => {
        console.error("[授权] 失败:", err);
        wx.showToast({ title: "授权失败", icon: "none" });
      }
    });
  },

  // ===========================
  // 页面显示（从后台切回前台）
  // ===========================
  onShow() {
    console.log("index onShow — 从全局数据同步 + 恢复轮询");
    this.syncFromGlobal();
    this.startPolling();

    // ★ 检测通知是否已开启，未开启则弹窗引导
    this.checkAndPromptSubscribe();
  },

  // ===========================
  // ★ 检测通知状态：未开启或额度可能耗尽时弹窗引导重新授权
  // ===========================
  // 微信一次性订阅消息：授权一次 = 1 条通知额度
  // 发送告警后额度即耗尽，因此需定期提醒用户重新授权，
  // 否则将收不到后续告警推送。
  checkAndPromptSubscribe() {
    if (this._subscribePrompting) return;

    const subscribed = wx.getStorageSync("subscribed");

    // 从未授权 → 首次引导开启
    if (!subscribed) {
      this._promptSubscribe(
        "开启告警通知",
        "是否开启车辆告警推送？开启后，车辆非法移动或异常离线时，您将第一时间收到微信通知。"
      );
      return;
    }

    // 已授权过，但距上次授权超过 7 天 → 额度可能已用完，提醒重新授权
    const lastTime = (subscribed && subscribed.lastTime) || 0;
    const daysSince = lastTime ? (Date.now() - lastTime) / (24 * 60 * 60 * 1000) : 99;
    if (daysSince >= 7) {
      this._promptSubscribe(
        "重新授权通知",
        "微信订阅通知额度为每授权一次仅可接收一条。为确保您能持续收到车辆告警，请重新授权。"
      );
    }
  },

  // 统一的授权引导弹窗
  _promptSubscribe(title, content) {
    this._subscribePrompting = true;
    wx.showModal({
      title: title,
      content: content,
      confirmText: "开启",
      cancelText: "暂不",
      success: (res) => {
        if (res.confirm) {
          // 用户点击"开启" → 处于用户手势上下文，可弹出授权卡片
          this.onSubscribeNotification();
        }
      },
      complete: () => {
        this._subscribePrompting = false;
      }
    });
  },

  // ===========================
  // 页面隐藏（切到后台 / 跳转其他页面）
  // ===========================
  onHide() {
    console.log("index onHide — 暂停轮询（节省云资源）");
    this.stopPolling();
  },

  // ===========================
  // 页面卸载
  // ===========================
  onUnload() {
    this.stopPolling();
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
  // ★ 统一数据加载
  // ===========================
  loadAllData() {
    this.getMyLocation();
    this.syncFromGlobal();
    this.loadHistory();
  },

  // ===========================
  // 开启本地定时同步（15 秒间隔，从 globalData 读取）
  // 不再自己发 HTTP，由 app.js 统一拉取
  // ===========================
  startPolling() {
    // 防止重复开启
    if (this.timer) return;

    this.timer = setInterval(() => {
      this.syncFromGlobal();
    }, 15000);
  },

  // ===========================
  // 停止定时同步
  // ===========================
  stopPolling() {
    if (this.timer) {
      clearInterval(this.timer);
      this.timer = null;
    }
  },

  getCardHeight() {

    const query = wx.createSelectorQuery();
  
    query.select("#card").boundingClientRect();
  
    query.exec((res) => {
  
      if (!res || !res[0]) return;
  
      this.cardHeight = res[0].height;
  
      console.log("卡片高度：", this.cardHeight);
  
    });
  
  },

  // ===========================
  // 获取手机位置
  // ===========================
  getMyLocation() {

    wx.getLocation({

      type: "gcj02",

      success: (res) => {

        this.setData({

          myLat: res.latitude,
          myLng: res.longitude

        });

        this.calcDistance();

      },

      fail: (err) => {

        console.error(err);

      }

    });

  },

  // ===========================
  // ★ 应用车辆数据到页面（从 globalData 读取后统一渲染）
  // ===========================
  applyCarData(d) {
    // 根据 carState 选择对应图标
    const stateIcon = this.getCarStateIcon(d.carState);
    const stateText = this.getCarStateText(d.carState);
    const carState = d.carState || "online";
    const stateClass = this.getCarStateClass ? this.getCarStateClass(carState) : "car-state-online";

    const locationValid = d.locationValid !== false && d.gcj02_lat != null && d.lat != null;
    const gcj02Lat = d.gcj02_lat || d.lat || this.data.lat;
    const gcj02Lng = d.gcj02_lng || d.lng || this.data.lng;

    // 更新时间文本（智能显示：今天/昨天/前天/月-日）
    const ts = d.updateTime || d.time || Math.floor(Date.now() / 1000);
    const timeStr = this.formatRelativeTime(ts);

    // HDOP → 定位精度半径（米）
    const hdop = d.hdop || 5;
    const radius = Math.max(10, Math.min(500, hdop * 5));

    const updateData = {
      lat: gcj02Lat,
      lng: gcj02Lng,
      speed: locationValid ? (d.speed || 0) : 0,

      circles: locationValid ? [{
        latitude: gcj02Lat,
        longitude: gcj02Lng,
        radius: radius,
        color: "#1677ff33",
        fillColor: "#1677ff19",
        strokeWidth: 1
      }] : [],

      markers: [{
        id: 1,
        latitude: gcj02Lat,
        longitude: gcj02Lng,
        iconPath: stateIcon,
        width: locationValid ? 30 : 30,
        height: locationValid ? 30 : 30,
        alpha: locationValid ? 1 : 0.5,
        callout: {
          content: locationValid ? `车速 ${d.speed || 0} km/h` : `GPS信号暂失 (${timeStr})`,
          display: "ALWAYS"
        }
      }],

      // 底部卡片车辆状态
      carState: carState,
      carStateIcon: stateIcon,
      carStateText: stateText,
      carStateClass: stateClass,
      gpsStatus: locationValid ? `GPS正常 (${timeStr})` : `GPS信号暂失 (${timeStr})`,
    };
    
    // 仅在有有效位置时才更新地图中心和跟随
    if (locationValid && this.data.followCar) {
      this.isProgramMove = true;
      updateData.mapLat = gcj02Lat;
      updateData.mapLng = gcj02Lng;
    }
    
    this.setData(updateData);
    this.calcDistance();
  },

  // ===========================
  // ★ 获取轨迹（含本地缓存 + 增量合并）
  // ===========================
  loadHistory() {

    const cacheKey = "trajectoryCache";
    const cache = wx.getStorageSync(cacheKey);
    const now = Math.floor(Date.now() / 1000);
    const yesterday = now - 86400;

    // 有缓存则增量请求，否则全量
    let url;
    if (cache && cache.lastTime && cache.lastTime > yesterday) {
      url = "https://YOUR_ENV_ID.tcloudbase.com/upload/history?since=" + cache.lastTime;
    } else {
      url = "https://YOUR_ENV_ID.tcloudbase.com/upload/history";
    }

    wx.request({

      url: url,

      method: "GET",

      success: (res) => {

        const newData = res.data.data || [];

        // 合并缓存 + 新数据
        let merged;
        if (cache && cache.data && newData.length === 0) {
          merged = cache.data;
        } else if (cache && cache.data) {
          const timeMap = {};
          for (let i = 0; i < cache.data.length; i++) {
            const item = cache.data[i];
            if (item.time > yesterday) {
              timeMap[item.time] = item;
            }
          }
          for (let i = 0; i < newData.length; i++) {
            timeMap[newData[i].time] = newData[i];
          }
          merged = Object.values(timeMap).sort((a, b) => a.time - b.time);
        } else {
          merged = newData;
        }

        // 更新缓存
        if (merged.length > 0) {
          const lastTime = merged[merged.length - 1].time;
          try {
            wx.setStorageSync(cacheKey, {
              data: merged,
              lastTime: lastTime,
              timestamp: now
            });
          } catch (e) {
            console.warn("[缓存] 写入失败:", e);
          }
        }

        // 渲染轨迹
        this.applyTrajectory(merged);

      }

    });

  },

  // ===========================
  // ★ 渲染轨迹（去重 + 按小时着色）
  // ===========================
  applyTrajectory(list) {

    if (list.length < 2) {
      this.setData({ polyline: [] });
      return;
    }

    const now = Math.floor(Date.now() / 1000);

    // 去重：跳过连续相同坐标（使用 gcj02 坐标）
    const deduped = [list[0]];
    for (let i = 1; i < list.length; i++) {
      const prev = list[i - 1];
      const curr = list[i];
      const prevLat = prev.gcj02_lat || prev.lat;
      const prevLng = prev.gcj02_lng || prev.lng;
      const currLat = curr.gcj02_lat || curr.lat;
      const currLng = curr.gcj02_lng || curr.lng;
      if (prevLat !== currLat || prevLng !== currLng) {
        deduped.push(curr);
      }
    }

    if (deduped.length < 2) {
      this.setData({ polyline: [] });
      return;
    }

    // 按小时着色连线（使用 gcj02 坐标）
    const polylines = [];
    for (let i = 0; i < deduped.length - 1; i++) {
      const age = now - deduped[i].time;
      const bucketIndex = Math.min(23, Math.max(0, Math.floor(age / 3600)));
      const p1Lat = deduped[i].gcj02_lat || deduped[i].lat;
      const p1Lng = deduped[i].gcj02_lng || deduped[i].lng;
      const p2Lat = deduped[i + 1].gcj02_lat || deduped[i + 1].lat;
      const p2Lng = deduped[i + 1].gcj02_lng || deduped[i + 1].lng;
      polylines.push({
        points: [
          { latitude: p1Lat, longitude: p1Lng },
          { latitude: p2Lat, longitude: p2Lng }
        ],
        color: this.getTrackColor(23 - bucketIndex, 23),
        width: 4,
        dottedLine: false,
        arrowLine: false
      });
    }

    this.setData({ polyline: polylines });

  },

  // ===========================
  // 计算手机到车辆距离
  // ===========================
  calcDistance() {

    if (!this.data.myLat) return;

    if (!this.data.lat) return;

    const R = 6378137;

    function rad(d) {

      return d * Math.PI / 180;

    }

    const lat1 = rad(this.data.myLat);
    const lng1 = rad(this.data.myLng);

    const lat2 = rad(this.data.lat);
    const lng2 = rad(this.data.lng);

    const a =
      Math.sin((lat2 - lat1) / 2) *
      Math.sin((lat2 - lat1) / 2) +
      Math.cos(lat1) *
      Math.cos(lat2) *
      Math.sin((lng2 - lng1) / 2) *
      Math.sin((lng2 - lng1) / 2);

    const d = 2 * R * Math.asin(Math.sqrt(a));

    this.setData({

      distance: d.toFixed(1)

    });

  },

  goDetail() {

    console.log("点击了查看详情");

    wx.navigateTo({
        url: "/pages/detail/detail",
        success() {
            console.log("跳转成功");
        },
        fail(err) {
            console.error("跳转失败", err);
        }
    });

  },

  // ===========================
  // 地图移动到车辆（toggle 模式）
  // ===========================
  moveToCar() {

    if (this.data.followCar) {
      this.setData({
        followCar: false
      });
      return;
    }

    this.isProgramMove = true;
  
    this.setData({
      followCar: true,
      mapLat: this.data.lat,
      mapLng: this.data.lng
    });
  
  },

  // ===========================
  // 地图变化事件（区分拖动和缩放）
  // ===========================

  onRegionChange(e) {

    if (e.type !== "end") {
      return;
    }
  
    if (this.isProgramMove) {
      this.isProgramMove = false;
      return;
    }

    if (e.causedBy === "scale") {
      return;
    }

    this.setData({
      followCar: false
    });
  
  },


  // ===========================
  // 手机+车辆全部显示
  // ===========================
  showAll() {

    this.isProgramMove = true;

    const map = wx.createMapContext("map", this);
  
    const bottomPadding = (this.cardHeight || 220) + 30;
  
    map.includePoints({
  
      points: [
  
        {
          latitude: this.data.myLat,
          longitude: this.data.myLng
        },
  
        {
          latitude: this.data.lat,
          longitude: this.data.lng
        }
  
      ],
  
      padding: [
   
        80,
   
        120,
   
        bottomPadding,
   
        120
   
      ]
  
    });
  
  },

// ===========================
// 轨迹颜色（红→黄→绿→蓝）
// ===========================
getTrackColor(index, total) {

  const colors = [

    "#FF4D4F",
    "#FF8C00",
    "#FFD700",
    "#A0D911",
    "#52C41A",
    "#1677FF"

  ];

  const ratio = index / Math.max(total, 1);

  const pos = ratio * (colors.length - 1);

  return colors[Math.floor(pos)];

},

// ===========================
// 轨迹宽度（2→8）
// ===========================
getTrackWidth(index, total) {

  const min = 2;
  const max = 8;

  if (total <= 0) return max;

  return max - (max - min) * index / total;

},

  // ===========================
  // 车辆状态→图标映射
  // ===========================
  getCarStateIcon(state) {
    const icons = {
      online: "/images/car_state/online.png",
      offline: "/images/car_state/offline.png",
      lowpower: "/images/car_state/lowpower.png",
      waiting: "/images/car_state/waiting.png"
    };
    return icons[state] || icons.online;
  },

  // ===========================
  // 车辆状态→文字映射
  // ===========================
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
// 时间格式化（智能显示：今天→时:分，昨天→昨天 时:分，前天→前天 时:分，更早→月-日 时:分）
// ===========================
formatTime(timestamp) {
  if (!timestamp) return "--";
  const d = new Date(timestamp * 1000);
  const h = String(d.getHours()).padStart(2, "0");
  const m = String(d.getMinutes()).padStart(2, "0");
  const hm = h + ":" + m;
  return hm;
},

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
});