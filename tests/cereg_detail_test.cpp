// CEREG=3 拒绝原因解析：从 idf_modem.cpp 抽出真实函数在主机上编译。
#include <cassert>
#include <cctype>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>

static std::string idf_util_trim_copy(const std::string& s)
{
    size_t b = 0, e = s.size();
    while (b < e && isspace(static_cast<unsigned char>(s[b]))) ++b;
    while (e > b && isspace(static_cast<unsigned char>(s[e - 1]))) --e;
    return s.substr(b, e - b);
}

// @真实函数@

int main()
{
    CeregDetail d;
    assert(parse_cereg_detail("\r\n+CEREG: 3,3,\"1A2B\",\"0ABCDEF1\",7,0,15\r\n\r\nOK\r\n", d));
    assert(d.n == 3 && d.stat == 3 && d.act == 7 && d.cause_type == 0 && d.reject_cause == 15);
    assert(std::string(emm_cause_text(15)) == "跟踪区内无合适小区");
    assert(parse_cereg_detail("+CEREG: 3,2\r\nOK", d) && d.stat == 2 && d.act == -1 && d.reject_cause == -1);
    assert(parse_cereg_detail("+CEREG: 3,0,,,,0,7\r\nOK", d) && d.stat == 0 && d.reject_cause == 7);
    assert(parse_cereg_detail("+CEREG: 2,1,\"1A2B\",\"0ABCDEF1\",7\r\nOK", d) && d.stat == 1 && d.act == 7 && d.reject_cause == -1);
    assert(!parse_cereg_detail("OK", d));
    assert(!parse_cereg_detail("+CEREG: x,1", d));
    assert(!parse_cereg_detail("+CEREG: 3", d));
    assert(std::string(cereg_stat_text(3)) == "注册被拒绝" && std::string(cereg_stat_text(12)) == "非标准值");
    // 27.007 合法状态 0..11 都要接受（11 = RLOS 受限附着，读不到 SIM 时常见）
    int stat = -1;
    assert(parse_cereg("+CEREG: 2,11,\"7C1D\",\"0054D504\",7\r\nOK", stat) && stat == 11);
    assert(parse_cereg("+CEREG: 2,1\r\nOK", stat) && stat == 1);
    assert(parse_cereg("+CEREG: 8\r\n", stat) && stat == 8);
    assert(!parse_cereg("+CEREG: 2,12\r\nOK", stat));
    assert(std::string(cereg_stat_text(11)).find("RLOS") != std::string::npos);
    std::cout << "cereg detail tests passed" << std::endl;
    return 0;
}
