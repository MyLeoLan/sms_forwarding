// 自动硬重启退避：从 idf_modem.cpp 抽出真实函数在主机上编译。
#include <atomic>
#include <cassert>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <iostream>
#include <string>

int64_t fake_us = 1000000;
int64_t esp_timer_get_time() { return fake_us; }
int log_count = 0;
void idf_logf(const char*, ...) { ++log_count; }
static std::atomic<int> s_reset_request{0};

// @真实函数@

static bool fire(const char* why = "测试")
{
    s_reset_request.store(0);
    bool ok = request_auto_hard_reset(why);
    assert(ok == (s_reset_request.load() == 2));
    return ok;
}

int main()
{
    // 第一次立即允许
    assert(fire());
    // 第二次需等 2 分钟
    fake_us += 60LL * 1000000;
    assert(!fire());
    fake_us += 61LL * 1000000;
    assert(fire());
    // 第三次需等 5 分钟
    fake_us += 4LL * 60 * 1000000;
    assert(!fire());
    fake_us += 61LL * 1000000;
    assert(fire());
    // 之后逐级拉长，最终封顶 60 分钟
    const int64_t minutes[] = {15, 30, 60, 60, 60};
    for (int64_t m : minutes) {
        fake_us += (m * 60 - 1) * 1000000;
        assert(!fire());
        fake_us += 2LL * 1000000;
        assert(fire());
    }
    // 注册成功清零：下一次立即允许
    note_registration_ok_for_auto_reset();
    fake_us += 1000000;
    assert(fire());
    // 退避期日志限流：一分钟内多次跳过只记一条
    fake_us += 61LL * 1000000;  // 越过上一条跳过日志的限流窗口，仍在 2 分钟退避内
    int before = log_count;
    for (int i = 0; i < 10; ++i) { fake_us += 1000000; assert(!fire()); }
    assert(log_count - before == 1);
    // 软重启与硬重启共用退避：清零后第一次软重启立即允许，紧接着的硬重启要等 2 分钟
    note_registration_ok_for_auto_reset();
    fake_us += 1000000;
    s_reset_request.store(0);
    assert(request_auto_reset("掉卡", 1) && s_reset_request.load() == 1);
    fake_us += 1000000;
    assert(!fire());
    fake_us += 121LL * 1000000;
    assert(fire());
    std::cout << "modem reset backoff tests passed" << std::endl;
    return 0;
}
