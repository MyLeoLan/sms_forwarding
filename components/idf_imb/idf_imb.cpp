#include "idf_imb.h"

#include <string>

#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
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

// 只走 WiFi：轮询频繁，走蜂窝会持续消耗卡上流量
static bool http_get(const std::string& url, std::string& body, int& status)
{
    esp_http_client_config_t cfg = {};
    cfg.url = url.c_str();
    cfg.timeout_ms = HTTP_TIMEOUT_MS;
    cfg.crt_bundle_attach = esp_crt_bundle_attach;
    cfg.keep_alive_enable = false;
    cfg.buffer_size_tx = 1024;
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) return false;

    bool ok = false;
    body.clear();
    if (esp_http_client_open(client, 0) == ESP_OK && esp_http_client_fetch_headers(client) >= 0) {
        status = esp_http_client_get_status_code(client);
        char buf[256];
        ok = true;
        while (body.size() < MAX_RESPONSE_BYTES) {
            int read = esp_http_client_read(client, buf, sizeof(buf));
            if (read < 0) {
                ok = false;
                break;
            }
            if (read == 0) break;
            body.append(buf, static_cast<size_t>(read));
        }
    }
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    return ok;
}

static bool http_post_json(const std::string& url, const std::string& json, int& status)
{
    esp_http_client_config_t cfg = {};
    cfg.url = url.c_str();
    cfg.timeout_ms = HTTP_TIMEOUT_MS;
    cfg.crt_bundle_attach = esp_crt_bundle_attach;
    cfg.keep_alive_enable = false;
    cfg.buffer_size_tx = 1024;
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) return false;
    esp_http_client_set_method(client, HTTP_METHOD_POST);
    esp_http_client_set_header(client, "Content-Type", "application/json");
    esp_http_client_set_post_field(client, json.c_str(), json.size());
    esp_err_t err = esp_http_client_perform(client);
    status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);
    return err == ESP_OK;
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
    std::string url = ep.base + "/api/v1/sim-sms/outbox/" + task_id + "/result?token=" + ep.token;
    std::string json = std::string("{\"success\":") + (success ? "true" : "false") +
                       ",\"message\":\"" + idf_imb_json_escape(message) + "\"}";
    for (int attempt = 1; attempt <= RESULT_REPORT_ATTEMPTS; ++attempt) {
        int status = 0;
        if (http_post_json(url, json, status) && status >= 200 && status < 300) return;
        vTaskDelay(pdMS_TO_TICKS(2000 * attempt));
    }
    idf_log_line("IMB 发件结果回报失败，IMB 上该任务将按超时处理");
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
    while (true) {
        vTaskDelay(pdMS_TO_TICKS(delay_ms));
        delay_ms = POLL_INTERVAL_MS;

        if (!idf_wifi_get_status().staConnected) continue;
        // 未注册网络时不取任务，留在 IMB 排队，比取走后立即失败更好
        if (!idf_modem_get_status().modemReady) continue;

        IdfImbEndpoint ep;
        if (!find_endpoint(ep)) {
            delay_ms = NOT_CONFIGURED_RECHECK_MS;
            continue;
        }
        if (heap_caps_get_largest_free_block(MALLOC_CAP_8BIT) < MIN_LARGEST_FREE_BLOCK) continue;
        if (!logged_active) {
            logged_active = true;
            idf_logf("IMB 发件轮询已启用: %s", ep.base.c_str());
        }

        std::string url = ep.base + "/api/v1/sim-sms/outbox/poll?token=" + ep.token;
        std::string body;
        int status = 0;
        if (!http_get(url, body, status) || status >= 500) {
            ESP_LOGW(TAG, "poll failed, status=%d", status);
            backoff_ms = backoff_ms * 2 > POLL_MAX_BACKOFF_MS ? POLL_MAX_BACKOFF_MS : backoff_ms * 2;
            delay_ms = backoff_ms;
            continue;
        }
        backoff_ms = POLL_INTERVAL_MS;
        if (status == 401) {
            idf_log_line("IMB 发件轮询: 令牌无效或卡已停用，5 分钟后重试");
            delay_ms = UNAUTHORIZED_RECHECK_MS;
            continue;
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
