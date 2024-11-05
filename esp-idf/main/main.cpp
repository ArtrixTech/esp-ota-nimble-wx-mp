#include <NimBLEDevice.h>
#include <NimBLEServer.h>
#include <esp_ota_ops.h>
// #include "esp_https_ota.h"
#include <esp_system.h>
#include "esp_log.h"
#include "nvs_flash.h"

static const char* TAG = "BLE_OTA";

// OTA状态枚举
enum class OtaState {
    IDLE,
    READY,
    IN_PROGRESS,
    VERIFYING,
    COMPLETE,
    ERROR
};

// OTA文件头结构
struct __attribute__((packed)) ota_header_t {
    uint32_t magic;          // 魔数：0x12345678
    uint32_t version;        // 固件版本
    uint32_t file_size;      // 文件大小
    uint32_t chunk_size;     // 数据块大小
    uint32_t checksum;       // 校验和
};

// OTA数据块结构
struct __attribute__((packed)) ota_chunk_t {
    uint32_t sequence;       // 序列号
    uint32_t size;          // 实际数据大小
    uint8_t data[];         // 数据内容
};

// OTA管理类
class OtaManager {
private:
    static OtaManager* instance;
    OtaState state;
    esp_ota_handle_t update_handle;
    const esp_partition_t* update_partition;
    uint32_t received_size;
    uint32_t total_size;
    uint32_t current_sequence;
    ota_header_t header;

    OtaManager() : state(OtaState::IDLE), update_handle(0),
        received_size(0), total_size(0), current_sequence(0) {
    }

public:
    static OtaManager* getInstance() {
        if (instance == nullptr) {
            instance = new OtaManager();
        }
        return instance;
    }

    esp_err_t begin() {
        if (state != OtaState::IDLE) {
            return ESP_ERR_INVALID_STATE;
        }

        // 获取下一个可用的OTA分区
        update_partition = esp_ota_get_next_update_partition(nullptr);
        if (update_partition == nullptr) {
            ESP_LOGE(TAG, "Failed to get next update partition");
            return ESP_ERR_NOT_FOUND;
        }

        state = OtaState::READY;
        return ESP_OK;
    }

    esp_err_t handleHeader(const uint8_t* data, size_t length) {
        if (state != OtaState::READY || length != sizeof(ota_header_t)) {
            return ESP_ERR_INVALID_STATE;
        }

        memcpy(&header, data, sizeof(ota_header_t));

        // 验证魔数
        if (header.magic != 0x12345678) {
            ESP_LOGE(TAG, "Invalid header magic");
            state = OtaState::ERROR;
            return ESP_ERR_INVALID_VERSION;
        }

        // 开始OTA更新
        esp_err_t err = esp_ota_begin(update_partition, header.file_size, &update_handle);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "esp_ota_begin failed: %s", esp_err_to_name(err));
            state = OtaState::ERROR;
            return err;
        }

        total_size = header.file_size;
        received_size = 0;
        current_sequence = 0;
        state = OtaState::IN_PROGRESS;

        return ESP_OK;
    }

    esp_err_t handleData(const uint8_t* data, size_t length) {
        if (state != OtaState::IN_PROGRESS) {
            return ESP_ERR_INVALID_STATE;
        }

        if (length < sizeof(ota_chunk_t)) {
            return ESP_ERR_INVALID_SIZE;
        }

        ota_chunk_t* chunk = (ota_chunk_t*)data;

        // 验证序列号
        if (chunk->sequence != current_sequence) {
            ESP_LOGE(TAG, "Invalid sequence number");
            state = OtaState::ERROR;
            return ESP_ERR_INVALID_VERSION;
        }

        // 写入数据
        esp_err_t err = esp_ota_write(update_handle, chunk->data, chunk->size);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "esp_ota_write failed: %s", esp_err_to_name(err));
            state = OtaState::ERROR;
            return err;
        }

        received_size += chunk->size;
        current_sequence++;

        ESP_LOGI(TAG, "Received data: %lu/%lu", received_size, total_size);

        // 检查是否接收完成
        if (received_size >= total_size) {
            ESP_LOGI(TAG, "All data received, finalizing OTA update");
            state = OtaState::VERIFYING;
            // 调用finish()完成OTA过程
            err = finish();
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "Failed to finish OTA: %s", esp_err_to_name(err));
                state = OtaState::ERROR;
                return err;
            }
        }

        return ESP_OK;
    }

    esp_err_t finish() {
        if (state != OtaState::VERIFYING) {
            return ESP_ERR_INVALID_STATE;
        }

        // 结束OTA
        esp_err_t err = esp_ota_end(update_handle);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "esp_ota_end failed: %s", esp_err_to_name(err));
            state = OtaState::ERROR;
            return err;
        }

        // 设置启动分区
        err = esp_ota_set_boot_partition(update_partition);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "esp_ota_set_boot_partition failed: %s", esp_err_to_name(err));
            state = OtaState::ERROR;
            return err;
        }

        state = OtaState::COMPLETE;
        return ESP_OK;
    }

    void reset() {
        if (state == OtaState::IN_PROGRESS) {
            esp_ota_abort(update_handle);
        }
        state = OtaState::IDLE;
        received_size = 0;
        total_size = 0;
        current_sequence = 0;
    }

    OtaState getState() const { return state; }
    uint32_t getProgress() const {
        return total_size > 0 ? (received_size * 100 / total_size) : 0;
    }
};

OtaManager* OtaManager::instance = nullptr;

// BLE服务和特征值UUID（请替换为你的UUID）
#define SERVICE_UUID        "0192fa61-3a6f-7278-9c88-293869284c63"
#define CONTROL_CHAR_UUID   "0192fa61-6877-7864-8506-20d94dcb9538"
#define DATA_CHAR_UUID      "0192fa61-6877-7d9a-afa5-3b58f345ea41"
#define STATUS_CHAR_UUID    "0192fa61-6877-79e4-a88a-2e99fa9c548d"

class OtaCallbacks : public NimBLECharacteristicCallbacks {
    void onWrite(NimBLECharacteristic* pCharacteristic, NimBLEConnInfo& connInfo) {
        std::string value = pCharacteristic->getValue();

        if (pCharacteristic->getUUID().toString() == CONTROL_CHAR_UUID) {
            // 控制命令处理
            if (value == "start") {
                ESP_LOGI(TAG, "OTA start command received");
                OtaManager::getInstance()->begin();
            }
            else if (value == "abort") {
                ESP_LOGI(TAG, "OTA abort command received");
                OtaManager::getInstance()->reset();
            }
        }
        else if (pCharacteristic->getUUID().toString() == DATA_CHAR_UUID) {
            // 数据处理
            const uint8_t* data = (const uint8_t*)value.data();
            size_t length = value.length();

            if (OtaManager::getInstance()->getState() == OtaState::READY) {
                // 处理文件头
                ESP_LOGI(TAG, "Receiving OTA header");
                OtaManager::getInstance()->handleHeader(data, length);
            }
            else if (OtaManager::getInstance()->getState() == OtaState::IN_PROGRESS) {
                // 处理数据块
                ESP_LOGI(TAG, "Receiving OTA data chunk");
                OtaManager::getInstance()->handleData(data, length);
            }
        }

        // 更新状态特征值
        NimBLEService* pService = pCharacteristic->getService();
        NimBLECharacteristic* pStatusChar = pService->getCharacteristic(STATUS_CHAR_UUID);
        if (pStatusChar != nullptr) {
            uint8_t status = (uint8_t)OtaManager::getInstance()->getState();
            uint8_t progress = OtaManager::getInstance()->getProgress();
            uint8_t statusData[2] = { status, progress };
            pStatusChar->setValue(statusData, 2);
            pStatusChar->notify();
        }
    }
};

class OtaServerCallbacks : public NimBLEServerCallbacks {
    void onConnect(NimBLEServer* pServer, NimBLEConnInfo& connInfo, int reason) {
        ESP_LOGI(TAG, "Client connected");
        pServer->startAdvertising();
    }

    void onDisconnect(NimBLEServer* pServer, NimBLEConnInfo& connInfo, int reason) {
        ESP_LOGI(TAG, "Client disconnected");
        OtaManager::getInstance()->reset();
    }
};

// BLE OTA服务初始化
void init_ble_ota() {
    // 初始化BLE
    NimBLEDevice::init("ESP32_OTA");
    NimBLEDevice::setPower(ESP_PWR_LVL_P9);

    // 创建BLE服务器
    NimBLEServer* pServer = NimBLEDevice::createServer();
    pServer->setCallbacks(new OtaServerCallbacks());

    // 创建OTA服务
    NimBLEService* pService = pServer->createService(SERVICE_UUID);

    // 创建特征值
    NimBLECharacteristic* pControlChar = pService->createCharacteristic(
        CONTROL_CHAR_UUID,
        NIMBLE_PROPERTY::WRITE
    );

    NimBLECharacteristic* pDataChar = pService->createCharacteristic(
        DATA_CHAR_UUID,
        NIMBLE_PROPERTY::WRITE
    );

    NimBLECharacteristic* pStatusChar = pService->createCharacteristic(
        STATUS_CHAR_UUID,
        NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::NOTIFY
    );

    // 设置回调
    OtaCallbacks* callbacks = new OtaCallbacks();
    pControlChar->setCallbacks(callbacks);
    pDataChar->setCallbacks(callbacks);

    // 启动服务
    pService->start();

    // 开始广播
    NimBLEAdvertising* pAdvertising = NimBLEDevice::getAdvertising();
    pAdvertising->addServiceUUID(SERVICE_UUID);
    pAdvertising->setScanResponse(true);
    pAdvertising->start();

    ESP_LOGI(TAG, "BLE OTA service started");
}

// 主程序入口
extern "C" void app_main() {
    // 初始化NVS
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    // 初始化BLE OTA服务
    init_ble_ota();

    // 主循环
    while (1) {
        if (OtaManager::getInstance()->getState() == OtaState::COMPLETE) {
            ESP_LOGI(TAG, "OTA update successful, restarting...");
            vTaskDelay(pdMS_TO_TICKS(1000));
            esp_restart();
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}