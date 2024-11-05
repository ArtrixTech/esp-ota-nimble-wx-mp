const UUID = {
  SERVICE: "0192FA61-3A6F-7278-9C88-293869284C63",
  CONTROL: "0192FA61-6877-7864-8506-20D94DCB9538",
  DATA: "0192FA61-6877-7D9A-AFA5-3B58F345EA41",
  STATUS: "0192FA61-6877-79E4-A88A-2E99FA9C548D"
}

const OTA_STATE = {
  IDLE: 0,
  READY: 1,
  IN_PROGRESS: 2,
  VERIFYING: 3,
  COMPLETE: 4,
  ERROR: 5
}

const TextEncoderLib = require('../lib/encoding.js');

class BLEManager {
  constructor() {
    this.isScanning = false;
    this.isConnected = false;
    this.deviceId = null;
    this.serviceId = null;
    this.file = null;
    this.onDeviceFound = null;
    this.onStateChange = null;
    this.onProgress = null;
    this.currMTU = null;
    this.fileChunkLength = null;
  }

  // 初始化蓝牙
  async init() {
    try {
      const res = await wx.openBluetoothAdapter()
      console.log('蓝牙初始化成功')




      return true
    } catch (err) {
      console.error('蓝牙初始化失败', err)
      wx.showToast({
        title: '请打开蓝牙',
        icon: 'none'
      })
      return false
    }
  }

  // 开始扫描
  startScan() {
    if (this.isScanning) return

    this.isScanning = true
    wx.startBluetoothDevicesDiscovery({
      services: [UUID.SERVICE],
      allowDuplicatesKey: false,
      success: () => {
        console.log('开始扫描')
        wx.onBluetoothDeviceFound((res) => {
          if (this.onDeviceFound) {
            this.onDeviceFound(res.devices[0])
          }
        })
      },
      fail: (err) => {
        console.error('扫描失败', err)
      }
    })
  }

  // 停止扫描
  stopScan() {
    if (!this.isScanning) return

    wx.stopBluetoothDevicesDiscovery({
      success: () => {
        this.isScanning = false
        console.log('停止扫描')
      }
    })
  }

  // 连接设备
  async connect(deviceId) {
    try {
      await wx.createBLEConnection({
        deviceId: deviceId
      })



      this.deviceId = deviceId
      this.isConnected = true

      let that=this;

      await wx.getBLEMTU({
        deviceId: this.deviceId,
        writeType: 'write',
        success(res) {
          console.log("MTU:", res)
          that.currMTU = parseInt(res.mtu);
          that.fileChunkLength = that.currMTU - 5;
        }
      })
      console.log("After MTU")

      const services = await wx.getBLEDeviceServices({
        deviceId: deviceId
      })

      this.serviceId = services.services.find(s => s.uuid.toUpperCase() === UUID.SERVICE).uuid

      const chars = await wx.getBLEDeviceCharacteristics({
        deviceId: deviceId,
        serviceId: this.serviceId
      })

      // 订阅状态特征值
      await wx.notifyBLECharacteristicValueChange({
        deviceId: deviceId,
        serviceId: this.serviceId,
        characteristicId: UUID.STATUS,
        state: true
      })

      wx.onBLECharacteristicValueChange((res) => {
        if (res.characteristicId === UUID.STATUS) {
          const state = new Uint8Array(res.value)[0]
          const progress = new Uint8Array(res.value)[1]

          if (this.onStateChange) {
            this.onStateChange(state)
          }
          if (this.onProgress) {
            this.onProgress(progress)
          }
        }
      })

      return true
    } catch (err) {
      console.error('连接失败', err)
      return false
    }
  }

  // 断开连接
  disconnect() {
    if (!this.isConnected) return

    wx.closeBLEConnection({
      deviceId: this.deviceId,
      success: () => {
        this.isConnected = false
        this.deviceId = null
        this.serviceId = null
        console.log('断开连接')
      }
    })
  }

  // 写入控制命令
  async writeControl(command) {
    try {
      const buffer = new TextEncoderLib.TextEncoder().encode(command).buffer
      await wx.writeBLECharacteristicValue({
        deviceId: this.deviceId,
        serviceId: this.serviceId,
        characteristicId: UUID.CONTROL,
        value: buffer
      })
      return true
    } catch (err) {
      console.error('写入控制命令失败', err)
      return false
    }
  }

  // 写入数据
  async writeData(buffer) {
    try {
      await wx.writeBLECharacteristicValue({
        deviceId: this.deviceId,
        serviceId: this.serviceId,
        characteristicId: UUID.DATA,
        value: buffer
      })
      return true
    } catch (err) {
      console.error('写入数据失败', err)
      return false
    }
  }

  // 开始OTA更新
  async startOTA(file) {
    if (!this.isConnected) return false

    this.file = file

    // 发送开始命令
    await this.writeControl('start')



    // 准备文件头
    const header = new ArrayBuffer(20)
    const headerView = new DataView(header)
    headerView.setUint32(0, 0x12345678, true) // magic
    headerView.setUint32(4, 1, true) // version
    headerView.setUint32(8, file.size, true) // file_size
    headerView.setUint32(12, this.fileChunkLength, true) // chunk_size
    headerView.setUint32(16, 0, true) // checksum

    // 发送文件头
    await this.writeData(header)

    // 开始发送文件数据
    return this.sendFileData()
  }

  // 发送文件数据
  async sendFileData() {
    const total = this.file.size
    let offset = 0
    let sequence = 0

    while (offset < total) {

      console.log(total - offset, " vs ", this.fileChunkLength)
      let actualFileChunkLength = Math.min(total - offset, this.fileChunkLength); // Ending part handling
      const chunk = await this.readFileChunk(offset, actualFileChunkLength)
      const size = chunk.byteLength

      // 准备数据块头
      const header = new ArrayBuffer(8)
      const headerView = new DataView(header)
      headerView.setUint32(0, sequence, true) // sequence
      headerView.setUint32(4, size, true) // size

      // 合并头和数据
      const buffer = new Uint8Array(header.byteLength + chunk.byteLength)
      buffer.set(new Uint8Array(header), 0)
      buffer.set(new Uint8Array(chunk), header.byteLength)

      // 发送数据块
      await this.writeData(buffer.buffer)

      offset += size
      sequence++
    }

    return true
  }

  // 读取文件块
  readFileChunk(offset, size) {
    return new Promise((resolve) => {
      const fs = wx.getFileSystemManager()

      console.log("sending, offset=", offset, ", length=", size);
      fs.readFile({
        filePath: this.file.path,
        position: offset,
        length: size,
        success: (res) => {
          resolve(res.data)
        },
        fail(res) {
          console.error(res)
        }
      })
    })
  }
}

export default new BLEManager()