// pages/index/index.js
Page({
  data: {
    // 传感器数据
    temp: 0.0,
    humi: 0.0,
    mq135_val: 0,
    air_quality: "未校准",
    airQualityClass: "air-worse",
    // 柜门状态【强制赋值永不空白，核心修复】
    door_status: "关闭",
    doorStatusClass: "door-close",
    // 磁吸原始数据 保留兼容硬件
    is_gps23_magnet_near: false,
    // 设备状态
    roast_led_status: "未运行",
    fan_status: "未运行",
    night_led_status: "未运行",
    // WebSocket
    wsConnected: false,
    webSocket: null
  },

  onLoad(options) {
    this.initWebSocket();
  },

  onUnload() {
    if (this.data.webSocket) {
      wx.closeSocket();
    }
  },

  // 初始化WebSocket连接ESP8266
  initWebSocket() {
    const that = this;
    const wsUrl = 'ws://192.168.4.1:81';
    const socketTask = wx.connectSocket({
      url: wsUrl,
      success: () => { console.log('✅ WebSocket连接中...'); },
      fail: (err) => {
        console.log('❌ WebSocket连接失败', err);
        wx.showToast({ title: '连接失败，请检查设备', icon: 'none', duration: 2000 });
        setTimeout(() => { that.initWebSocket(); }, 3000);
      }
    });

    this.setData({ webSocket: socketTask });

    socketTask.onOpen(() => {
      that.setData({ wsConnected: true });
      wx.showToast({ title: '连接成功', icon: 'success', duration: 1500 });
    });

    // 接收硬件数据 核心：磁吸→柜门映射 + 强制兜底永不空白
    socketTask.onMessage((res) => {
      try {
        const data = JSON.parse(res.data);
        // 1. 更新温湿度空气质量
        that.setData({
          temp: data.temp || 0.0,
          humi: data.humi || 0.0,
          mq135_val: data.mq135_val || 0,
          air_quality: data.air_quality || "未校准"
        });
        // 空气质量样式
        switch (data.air_quality) {
          case "优": that.setData({ airQualityClass: "air-excellent" }); break;
          case "良": that.setData({ airQualityClass: "air-good" }); break;
          case "一般": that.setData({ airQualityClass: "air-normal" }); break;
          case "差": that.setData({ airQualityClass: "air-bad" }); break;
          case "极差": that.setData({ airQualityClass: "air-worse" }); break;
          default: that.setData({ airQualityClass: "air-worse" }); break;
        }

        // ✅✅✅ 核心修复：磁吸状态转柜门状态 强制赋值 绝对不空白
        const magnetState = data.is_gps23_magnet_near === true;
        const doorNow = magnetState ? "关闭" : "开启";
        const doorClassNow = magnetState ? "door-close" : "door-open";
        
        // 2. 更新柜门+磁吸+设备状态
        that.setData({
          is_gps23_magnet_near: magnetState,
          door_status: doorNow,
          doorStatusClass: doorClassNow,
          roast_led_status: data.roast_led_status || "未运行",
          fan_status: data.fan_status || "未运行",
          night_led_status: data.night_led_status || "未运行"
        });

      } catch (err) {
        console.log('❌ 数据解析失败', err);
        // 解析出错也强制显示柜门状态，永不空白
        that.setData({
          door_status: "关闭",
          doorStatusClass: "door-close"
        });
      }
    });

    socketTask.onClose(() => {
      that.setData({ wsConnected: false });
      wx.showToast({ title: '连接断开', icon: 'none', duration: 2000 });
      setTimeout(() => { that.initWebSocket(); }, 3000);
    });

    socketTask.onError((err) => {
      that.setData({ wsConnected: false });
      console.log('❌ WebSocket错误', err);
    });
  },

  // 发送指令通用方法
  sendWsCmd(cmdObj) {
    if (!this.data.wsConnected) {
      wx.showToast({ title: '未连接设备', icon: 'none', duration: 1500 });
      return;
    }
    this.data.webSocket.send({
      data: JSON.stringify(cmdObj),
      fail: () => wx.showToast({ title: '指令发送失败', icon: 'none' })
    });
  },

  // 烤灯控制
  roastLedOn() { this.sendWsCmd({ roast_led: true }); },
  roastLedOff() { this.sendWsCmd({ roast_led: false }); },

  // 风扇控制
  fanOn() { this.sendWsCmd({ fan: true }); },
  fanOff() { this.sendWsCmd({ fan: false }); },

  // 紫外线消毒灯控制
  nightLedOn() { this.sendWsCmd({ night_led: true }); },
  nightLedOff() { this.sendWsCmd({ night_led: false }); }
})