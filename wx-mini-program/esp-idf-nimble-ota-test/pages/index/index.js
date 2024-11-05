import bleManager from '../../utils/ble'


const OTA_STATE = {
  IDLE: 0,
  READY: 1,
  IN_PROGRESS: 2,
  VERIFYING: 3,
  COMPLETE: 4,
  ERROR: 5
}

Page({
  data: {
    devices: [],
    connected: false,
    uploading: false,
    progress: 0,
    state: 'IDLE'
  },
  
  onLoad() {
    this.initBle()
    
    bleManager.onDeviceFound = (device) => {
      const devices = this.data.devices
      const idx = devices.findIndex(d => d.deviceId === device.deviceId)
      
      if (idx === -1) {
        devices.push(device)
        this.setData({ devices })
      }
    }
    
    bleManager.onStateChange = (state) => {
      const stateStr = Object.keys(OTA_STATE)[state]
      this.setData({ state: stateStr })
      
      if (state === OTA_STATE.COMPLETE) {
        this.setData({ uploading: false })
        wx.showToast({
          title: '更新完成',
          icon: 'success'
        })
      } else if (state === OTA_STATE.ERROR) {
        this.setData({ uploading: false })
        wx.showToast({
          title: '更新失败',
          icon: 'none'
        })
      }
    }
    
    bleManager.onProgress = (progress) => {
      this.setData({ progress })
    }
  },
  
  async initBle() {
    const res = await bleManager.init()
    if (res) {
      console.log("Init Success")
      this.startScan()
    }
  },
  
  startScan() {
    this.setData({ devices: [] })
    bleManager.startScan()
  },
  
  stopScan() {
    bleManager.stopScan()
  },
  
  async connectDevice(e) {
    const deviceId = e.currentTarget.dataset.deviceId
    const res = await bleManager.connect(deviceId)
    
    if (res) {
      this.setData({ connected: true })
      this.stopScan()
    }
  },
  
  disconnect() {
    bleManager.disconnect()
    this.setData({ 
      connected: false,
      uploading: false,
      progress: 0
    })
  },
  
  async chooseFile() {
    try {
      const res = await wx.chooseMessageFile({
        count: 1,
        type: 'file',
        extension: ['bin']
      })
      
      this.setData({ uploading: true })
      const success = await bleManager.startOTA(res.tempFiles[0])
      
      if (!success) {
        this.setData({ uploading: false })
        wx.showToast({
          title: '开始更新失败',
          icon: 'none'
        })
      }
    } catch (err) {
      console.error('选择文件失败', err)
    }
  },
  
  onUnload() {
    this.disconnect()
    bleManager.stopScan()
  }
})