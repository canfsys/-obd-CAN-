// app.js
App({
  onLaunch: function () {
    this.globalData = {
      env: "YOUR_ENV_ID",
      carInfo: null,           // 车辆最新数据
      unreadAlertCount: 0,     // 未读告警数
      alerts: [],              // 告警列表
    };
    if (!wx.cloud) {
      console.error("请使用 2.2.3 或以上的基础库以使用云能力");
    } else {
      wx.cloud.init({
        env: this.globalData.env,
        traceUser: true,
      });
    }

    // 初始化回调队列
    this._carDataReadyCallbacks = [];
    this._alertCallbacks = [];

    // 启动全局轮询
    this.startGlobalPolling();
  },

  onShow() {
    console.log("app onShow — 恢复全局轮询");
    this.startGlobalPolling();
  },

  onHide() {
    console.log("app onHide — 暂停全局轮询");
    this.stopGlobalPolling();
  },

  // ===========================
  // ★ 注册回调：首次数据就绪时通知页面
  // ===========================
  onCarDataReady(callback) {
    if (this.globalData.carInfo) {
      callback(this.globalData.carInfo);
    } else {
      this._carDataReadyCallbacks.push(callback);
    }
  },

  // ===========================
  // ★ 注册告警变更回调
  // ===========================
  onAlertChange(callback) {
    this._alertCallbacks.push(callback);
    // 立即通知当前状态
    if (this.globalData.unreadAlertCount > 0) {
      callback(this.globalData.unreadAlertCount, this.globalData.alerts);
    }
  },

  _notifyAlertChange() {
    const cbs = this._alertCallbacks;
    cbs.forEach(cb => cb(this.globalData.unreadAlertCount, this.globalData.alerts));
  },

  // 标记告警已读（供页面调用）
  markAlertsRead(ids) {
    wx.request({
      url: "https://YOUR_ENV_ID.tcloudbase.com/upload/alerts/read",
      method: "POST",
      header: { "Content-Type": "application/json" },
      data: { ids: ids },
      success: () => {
        this.globalData.unreadAlertCount = 0;
        this.globalData.alerts = [];
        this._notifyAlertChange();
      }
    });
  },

  // ===========================
  // 全局轮询：每 15 秒拉取 /upload/latest + 告警
  // ===========================
  startGlobalPolling() {
    if (this._globalTimer) return;

    this.fetchCarData();
    this.fetchAlerts();

    this._globalTimer = setInterval(() => {
      this.fetchCarData();
      this.fetchAlerts();
    }, 15000);
  },

  stopGlobalPolling() {
    if (this._globalTimer) {
      clearInterval(this._globalTimer);
      this._globalTimer = null;
    }
  },

  fetchCarData() {
    wx.request({
      url: "https://YOUR_ENV_ID.tcloudbase.com/upload/latest",
      method: "GET",
      success: (res) => {
        const d = res.data && res.data.data;
        if (d) {
          const isFirstData = !this.globalData.carInfo;
          this.globalData.carInfo = d;
          if (isFirstData) {
            const cbs = this._carDataReadyCallbacks;
            this._carDataReadyCallbacks = [];
            cbs.forEach(cb => cb(d));
          }
        }
      },
      fail: () => {},
    });
  },

  // ★ 轮询告警
  fetchAlerts() {
    wx.request({
      url: "https://YOUR_ENV_ID.tcloudbase.com/upload/alerts?deviceId=car001&since=0",
      method: "GET",
      success: (res) => {
        const data = res.data && res.data.data;
        if (data) {
          const prevCount = this.globalData.unreadAlertCount;
          this.globalData.unreadAlertCount = data.unreadCount || 0;
          this.globalData.alerts = data.alerts || [];

          // 告警数量有变化才通知
          if (this.globalData.unreadAlertCount !== prevCount) {
            this._notifyAlertChange();
          }
        }
      },
      fail: () => {},
    });
  },
});
