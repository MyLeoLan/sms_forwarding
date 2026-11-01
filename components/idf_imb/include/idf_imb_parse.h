#pragma once

// IMB 发件协议的纯文本解析，不依赖 ESP-IDF，便于在主机上测试。

#include <stdio.h>

#include <string>

struct IdfImbEndpoint {
    std::string base;   // 如 https://imb.leolan.top
    std::string token;  // 卡设备令牌
};

struct IdfImbTask {
    std::string id;
    std::string phone;
    std::string content;
};

// 从推送通道 URL 解析 IMB 地址与令牌；不是 IMB webhook 地址时返回 false。
inline bool idf_imb_parse_webhook_url(const std::string& url, IdfImbEndpoint& out)
{
    static const std::string kMarker = "/api/v1/sim-sms/webhook?";
    size_t marker = url.find(kMarker);
    if (marker == std::string::npos || marker == 0) return false;
    size_t query = marker + kMarker.size();
    size_t token_pos = std::string::npos;
    for (size_t pos = query; pos < url.size();) {
        size_t next = url.find('&', pos);
        if (url.compare(pos, 6, "token=") == 0) {
            token_pos = pos + 6;
            break;
        }
        if (next == std::string::npos) break;
        pos = next + 1;
    }
    if (token_pos == std::string::npos) return false;
    size_t token_end = url.find('&', token_pos);
    std::string token = url.substr(token_pos, token_end == std::string::npos ? std::string::npos
                                                                             : token_end - token_pos);
    if (token.empty()) return false;
    out.base = url.substr(0, marker);
    out.token = token;
    return true;
}

// 响应体格式：第一行任务 ID，第二行目标号码，其余为短信内容（可含换行）。
inline bool idf_imb_parse_task(const std::string& body, IdfImbTask& out)
{
    size_t first = body.find('\n');
    if (first == std::string::npos || first == 0) return false;
    size_t second = body.find('\n', first + 1);
    if (second == std::string::npos || second == first + 1) return false;
    if (second + 1 >= body.size()) return false;
    out.id = body.substr(0, first);
    out.phone = body.substr(first + 1, second - first - 1);
    out.content = body.substr(second + 1);
    return true;
}

// 任务 ID 会拼进 URL，只接受 UUID 字符，防止异常响应改写请求路径。
inline bool idf_imb_task_id_valid(const std::string& id)
{
    if (id.empty() || id.size() > 64) return false;
    for (char ch : id) {
        bool ok = (ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'z') ||
                  (ch >= 'A' && ch <= 'Z') || ch == '-';
        if (!ok) return false;
    }
    return true;
}

inline std::string idf_imb_json_escape(const std::string& text)
{
    std::string out;
    out.reserve(text.size() + 8);
    for (char ch : text) {
        unsigned char c = static_cast<unsigned char>(ch);
        if (ch == '"' || ch == '\\') {
            out.push_back('\\');
            out.push_back(ch);
        } else if (ch == '\n') {
            out += "\\n";
        } else if (ch == '\r') {
            out += "\\r";
        } else if (ch == '\t') {
            out += "\\t";
        } else if (c < 0x20) {
            char buf[8];
            snprintf(buf, sizeof(buf), "\\u%04x", c);
            out += buf;
        } else {
            out.push_back(ch);
        }
    }
    return out;
}
