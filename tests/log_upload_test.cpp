// 日志时间前缀与增量收集：从 idf_log.cpp 抽出真实函数在主机上编译。
#include <array>
#include <atomic>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <iostream>
#include <string>
#include <vector>

using SemaphoreHandle_t = void*;
constexpr int pdTRUE = 1;
#define pdMS_TO_TICKS(ms) (ms)
static int xSemaphoreTake(SemaphoreHandle_t, int) { return pdTRUE; }
static void xSemaphoreGive(SemaphoreHandle_t) {}
static int64_t fake_us = 42LL * 1000000;
static int64_t esp_timer_get_time() { return fake_us; }
static time_t fake_now = 0;
static time_t time(std::nullptr_t) { return fake_now; }

struct IdfLogEntry {
    uint32_t seq;
    std::string text;
};
static constexpr size_t LOG_RING_SIZE = 4;
static SemaphoreHandle_t s_log_mutex = reinterpret_cast<void*>(1);
static std::array<std::string, LOG_RING_SIZE> s_lines;
static uint32_t s_seq = 0;
static std::atomic<uint32_t> s_seq_mirror{0};
static size_t s_count = 0;
static size_t s_next = 0;
static void ensure_init() {}
static void push(const std::string& line)
{
    s_lines[s_next] = line;
    s_next = (s_next + 1) % LOG_RING_SIZE;
    if (s_count < LOG_RING_SIZE) ++s_count;
    s_seq_mirror.store(++s_seq);
}

// @真实函数@

int main()
{
    char buf[24];
    format_log_time(buf, sizeof(buf));
    assert(std::string(buf) == "+42s ");
    fake_now = 1763303405;  // 2025-11-16 14:30:05 UTC
    format_log_time(buf, sizeof(buf));
    assert(std::string(buf) == "11-16 22:30:05 ");
    idf_log_set_tz_offset_min(0);
    format_log_time(buf, sizeof(buf));
    assert(std::string(buf) == "11-16 14:30:05 ");
    idf_log_set_tz_offset_min(99999);  // 非法值忽略
    format_log_time(buf, sizeof(buf));
    assert(std::string(buf) == "11-16 14:30:05 ");

    std::vector<IdfLogEntry> out;
    assert(idf_log_collect_since(0, 100, out) == 0 && out.empty());
    push("a"); push("bb"); push("ccc");
    assert(idf_log_collect_since(0, 100, out) == 3 && out.size() == 3 && out[0].seq == 1 && out[2].text == "ccc");
    assert(idf_log_collect_since(1, 100, out) == 3 && out.size() == 2 && out[0].text == "bb");
    // 字节上限：至少一行，超出即停
    assert(idf_log_collect_since(0, 2, out) == 1 && out.size() == 1);
    assert(idf_log_collect_since(0, 3, out) == 2 && out.size() == 2);
    assert(idf_log_collect_since(3, 100, out) == 3 && out.empty());
    // 环形覆盖：旧行已丢，只给还在的
    push("d"); push("e"); push("f");  // seq 4..6，环里剩 3..6
    assert(idf_log_collect_since(1, 100, out) == 6 && out.size() == 4 && out[0].seq == 3 && out[0].text == "ccc");
    // 游标比当前序号大：从头取
    assert(idf_log_collect_since(99, 100, out) == 6 && out.size() == 4);
    assert(idf_log_current_seq() == 6);
    std::cout << "log upload tests passed" << std::endl;
    return 0;
}
