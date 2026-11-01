#include "idf_imb.h"

#include <string>

#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "idf_config.h"
#include "idf_imb_parse.h"
#include "idf_log.h"
#include "idf_modem.h"
#include "idf_sms.h"
#include "idf_wifi.h"

static const char* TAG = "idf_imb";

static constexpr uint32_t POLL_INTERVAL_MS = 15000;
static constexpr uint32_t POLL_MAX_BACKOFF_MS = 120000;
static constexpr uint32_t NOT_CONFIGURED_RECHECK_MS = 60000;
static constexpr uint32_t UNAUTHORIZED_RECHECK_MS = 300000;
static constexpr int HTTP_TIMEOUT_MS = 10000;
static constexpr size_t MAX_RESPONSE_BYTES = 1400;
// 一次 TLS 握手约需 40KB 连续堆，余量不足时跳过本轮，不和短信转发抢内存
static constexpr size_t MIN_LARGEST_FREE_BLOCK = 36 * 1024;
static constexpr int RESULT_REPORT_ATTEMPTS = 3;
// 每 20 次轮询（约 5 分钟）上报一次设备状态
static constexpr int HEARTBEAT_EVERY_POLLS = 20;
static constexpr int MODEM_VALUE_UNKNOWN = 999;

// 复用同一条 TLS 连接：每 15 秒重新握手既耗 CPU，又会持续消耗和打碎堆，
// 把为低频推送设计的内存余量拖垮。只走 WiFi，轮询走蜂窝会持续消耗卡上流量。
static esp_http_client_handle_t s_client = nullptr;
static std::string s_response;

static esp_err_t on_http_event(esp_http_client_event_t* event)
{
    if (event->event_id == HTTP_EVENT_ON_DATA && s_response.size() < MAX_RESPONSE_BYTES) {
        s_response.append(static_cast<const char*>(event->data), event->data_len);
    }
    return ESP_OK;
}

static void close_client(void)
{
    if (!s_client) return;
    esp_http_client_cleanup(s_client);
    s_client = nullptr;
}

static bool open_client(const std::string& url)
{
    if (s_client) return true;
    esp_http_client_config_t cfg = {};
    cfg.url = url.c_str();
    cfg.timeout_ms = HTTP_TIMEOUT_MS;
    cfg.crt_bundle_attach = esp_crt_bundle_attach;
    cfg.keep_alive_enable = true;
    cfg.buffer_size_tx = 1024;
    cfg.event_handler = on_http_event;
    s_client = esp_http_client_init(&cfg);
    return s_client != nullptr;
}

// 请求失败时丢弃连接，下一轮重新建立（服务器关闭空闲连接属于正常情况）
static bool http_request(const std::string& url, const std::string& token, const std::string* json,
                         std::string& body, int& status)
{
    status = 0;
    body.clear();
    if (!open_client(url)) return false;
    s_response.clear();
    esp_http_client_set_url(s_client, url.c_str());
    esp_http_client_set_header(s_client, "X-SIM-Device-Token", token.c_str());
    if (json) {
        esp_http_client_set_method(s_client, HTTP_METHOD_POST);
        esp_http_client_set_header(s_client, "Content-Type", "application/json");
        esp_http_client_set_post_field(s_client, json->c_str(), json->size());
    } else {
        esp_http_client_set_method(s_client, HTTP_METHOD_GET);
        esp_http_client_delete_header(s_client, "Content-Type");
        esp_http_client_set_post_field(s_client, nullptr, 0);
    }
    esp_err_t err = esp_http_client_perform(s_client);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "request failed: %s", esp_err_to_name(err));
        close_client();
        return false;
    }
    status = esp_http_client_get_status_code(s_client);
    body.swap(s_response);
    return true;
}

// 暂停原因只在变化时写日志，避免每 15 秒刷屏；原因为空表示正常运行。
static void note_pause(std::string& current, const std::string& reason)
{
    if (current == reason) return;
    size_t free_heap = heap_caps_get_free_size(MALLOC_CAP_8BIT);
    size_t largest = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
    if (reason.empty()) {
        idf_logf("IMB 发件轮询恢复（空闲堆 %u，最大块 %u）", static_cast<unsigned>(free_heap),
                 static_cast<unsigned>(largest));
    } else {
        idf_logf("IMB 发件轮询暂停: %s（空闲堆 %u，最大块 %u）", reason.c_str(),
                 static_cast<unsigned>(free_heap), static_cast<unsigned>(largest));
    }
    current = reason;
}

static bool find_endpoint(IdfImbEndpoint& out)
{
    IdfConfig cfg = idf_config_get();
    if (!cfg.pushEnabled) return false;
    for (int i = 0; i < IDF_MAX_PUSH_CHANNELS; ++i) {
        const IdfPushChannel& channel = cfg.pushChannels[i];
        if (channel.enabled && idf_imb_parse_webhook_url(channel.url, out)) return true;
    }
    return false;
}

// 回报失败只影响 IMB 上的状态显示，绝不重发短信，避免重复扣费
static void report_result(const IdfImbEndpoint& ep, const std::string& task_id, bool success,
                          const std::string& message)
{
    std::string url = ep.base + "/api/v1/sim-sms/outbox/" + task_id + "/result";
    std::string json = std::string("{\"success\":") + (success ? "true" : "false") +
                       ",\"message\":\"" + idf_imb_json_escape(message) + "\"}";
    for (int attempt = 1; attempt <= RESULT_REPORT_ATTEMPTS; ++attempt) {
        int status = 0;
        std::string body;
        if (http_request(url, ep.token, &json, body, status) && status >= 200 && status < 300) return;
        vTaskDelay(pdMS_TO_TICKS(2000 * attempt));
    }
    idf_log_line("IMB 发件结果回报失败，IMB 上该任务将按超时处理");
}

static void add_text(std::string& json, const char* key, const std::string& value)
{
    if (value.empty()) return;
    if (json.back() != '{') json += ",";
    json += std::string("\"") + key + "\":\"" + idf_imb_json_escape(value) + "\"";
}

static void add_number(std::string& json, const char* key, long long value)
{
    if (json.back() != '{') json += ",";
    json += std::string("\"") + key + "\":" + std::to_string(value);
}

static std::string heartbeat_json(void)
{
    const IdfModemStatus modem = idf_modem_get_status();
    const IdfWifiStatus wifi = idf_wifi_get_status();

    std::string status = "{";
    if (modem.csq >= 0) add_number(status, "csq", modem.csq);
    if (modem.rsrp != MODEM_VALUE_UNKNOWN) add_number(status, "rsrp", modem.rsrp);
    if (modem.rsrq != MODEM_VALUE_UNKNOWN) add_number(status, "rsrq", modem.rsrq);
    if (modem.sinr != MODEM_VALUE_UNKNOWN) add_number(status, "sinr", modem.sinr);
    if (modem.ceregStat >= 0) add_number(status, "registration", modem.ceregStat);
    add_text(status, "model", modem.model);
    add_text(status, "modem_firmware", modem.fwver);
    add_text(status, "firmware", IDF_FW_VERSION);
    add_number(status, "uptime_seconds", esp_timer_get_time() / 1000000);
    add_number(status, "free_heap", static_cast<long long>(heap_caps_get_free_size(MALLOC_CAP_8BIT)));
    add_number(status, "largest_block", static_cast<long long>(heap_caps_get_largest_free_block(MALLOC_CAP_8BIT)));
    add_number(status, "wifi_rssi", wifi.rssi);
    add_text(status, "wifi_ip", wifi.ip);
    status += "}";

    std::string signal;
    if (modem.rsrp != MODEM_VALUE_UNKNOWN) {
        signal = std::to_string(modem.rsrp) + " dBm";
    } else if (modem.csq >= 0) {
        signal = "CSQ " + std::to_string(modem.csq);
    }

    std::string json = "{";
    add_text(json, "signal", signal);
    add_text(json, "carrier", modem.operatorName);
    add_text(json, "phone_number", modem.phone);
    add_text(json, "iccid", modem.iccid);
    add_text(json, "imsi", modem.imsi);
    add_text(json, "imei", modem.imei);
    add_text(json, "apn", modem.apnSim);
    if (json.back() != '{') json += ",";
    json += "\"status\":" + status + "}";
    return json;
}

// 心跳失败不影响发短信，也不重试，下个周期再报
static void send_heartbeat(const IdfImbEndpoint& ep)
{
    const std::string json = heartbeat_json();
    std::string body;
    int status = 0;
    if (!http_request(ep.base + "/api/v1/sim-sms/heartbeat", ep.token, &json, body, status) ||
        status < 200 || status >= 300) {
        ESP_LOGW(TAG, "heartbeat failed, status=%d", status);
    }
}

static void handle_task(const IdfImbEndpoint& ep, const IdfImbTask& task)
{
    idf_logf("IMB 发件任务: 目标 %s，长度 %u", task.phone.c_str(),
             static_cast<unsigned>(task.content.size()));
    std::string message;
    esp_err_t err = idf_sms_send_text(task.phone, task.content, message);
    idf_logf("IMB 发件结果: %s", message.c_str());
    report_result(ep, task.id, err == ESP_OK, message);
}

static void imb_task(void*)
{
    uint32_t delay_ms = POLL_INTERVAL_MS;
    uint32_t backoff_ms = POLL_INTERVAL_MS;
    bool logged_active = false;
    int polls_until_heartbeat = 2;
    std::string pause_reason;
    while (true) {
        vTaskDelay(pdMS_TO_TICKS(delay_ms));
        delay_ms = POLL_INTERVAL_MS;

        if (!idf_wifi_get_status().staConnected) {
            close_client();
            note_pause(pause_reason, "WiFi 未连接");
            continue;
        }
        // 未注册网络时不取任务，留在 IMB 排队，比取走后立即失败更好
        if (!idf_modem_get_status().modemReady) {
            note_pause(pause_reason, "模组未注册网络");
            continue;
        }

        IdfImbEndpoint ep;
        if (!find_endpoint(ep)) {
            close_client();
            note_pause(pause_reason, "未启用 IMB 推送通道");
            delay_ms = NOT_CONFIGURED_RECHECK_MS;
            continue;
        }
        // 已有连接时不需要再握手，只有新建连接才要求内存余量
        if (!s_client && heap_caps_get_largest_free_block(MALLOC_CAP_8BIT) < MIN_LARGEST_FREE_BLOCK) {
            note_pause(pause_reason, "可用内存不足，推迟建立连接");
            continue;
        }
        if (!logged_active) {
            logged_active = true;
            idf_logf("IMB 发件轮询已启用: %s", ep.base.c_str());
        }

        std::string url = ep.base + "/api/v1/sim-sms/outbox/poll";
        std::string body;
        int status = 0;
        if (!http_request(url, ep.token, nullptr, body, status) || status >= 500) {
            note_pause(pause_reason, status ? "IMB 返回 HTTP " + std::to_string(status) : "连接 IMB 失败");
            backoff_ms = backoff_ms * 2 > POLL_MAX_BACKOFF_MS ? POLL_MAX_BACKOFF_MS : backoff_ms * 2;
            delay_ms = backoff_ms;
            continue;
        }
        backoff_ms = POLL_INTERVAL_MS;
        note_pause(pause_reason, "");
        if (status == 401) {
            idf_log_line("IMB 发件轮询: 令牌无效或卡已停用，5 分钟后重试");
            delay_ms = UNAUTHORIZED_RECHECK_MS;
            continue;
        }

        // 信号平时不采样：提前一轮请求刷新，下一轮再发，上报的才是新值
        if (polls_until_heartbeat == 1) idf_modem_note_status_interest();
        if (polls_until_heartbeat == 0) {
            send_heartbeat(ep);
            polls_until_heartbeat = HEARTBEAT_EVERY_POLLS;
        } else {
            --polls_until_heartbeat;
        }

        if (status != 200 || body.empty()) continue;

        IdfImbTask task;
        if (!idf_imb_parse_task(body, task) || !idf_imb_task_id_valid(task.id)) {
            idf_log_line("IMB 发件轮询: 响应格式无效，已忽略");
            continue;
        }
        handle_task(ep, task);
        delay_ms = 2000;  // 连发多条时不必再等完整周期
    }
}

esp_err_t idf_imb_start(void)
{
    BaseType_t ok = xTaskCreate(imb_task, "idf_imb", 8192, nullptr, 2, nullptr);
    return ok == pdPASS ? ESP_OK : ESP_ERR_NO_MEM;
}
