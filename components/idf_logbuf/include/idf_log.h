#pragma once

#include <stdint.h>

#include <string>
#include <vector>

void idf_log_init(void);
void idf_log_line(const char* line);
void idf_logf(const char* fmt, ...);
std::string idf_log_json_since(uint32_t since);
std::string idf_log_text_dump(void);

// 日志时间前缀使用的时区（分钟，默认 UTC+8）；由上层在读到配置后设置
void idf_log_set_tz_offset_min(int minutes);

// 取序号大于 since 的日志行（按时间顺序），总字节不超过 max_bytes（至少返回 1 行）。
// last_seq 返回最后一行的序号；没有新日志时返回 since。since 超过当前序号视为设备已重启，从头取。
struct IdfLogEntry {
    uint32_t seq;
    std::string text;
};
uint32_t idf_log_collect_since(uint32_t since, size_t max_bytes, std::vector<IdfLogEntry>& out);
uint32_t idf_log_current_seq(void);

// 上次运行日志：开机时只捕获 noinit RAM 镜像，异常复位后延迟保存到
// smsdata NVS 分区；Flash 里的兜底日志只在用户请求 prev dump 时读取。
bool idf_log_has_prev(void);
std::string idf_log_prev_dump(void);
