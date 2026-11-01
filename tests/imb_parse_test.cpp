#include <assert.h>

#include <string>

#include "../components/idf_imb/include/idf_imb_parse.h"

int main()
{
    IdfImbEndpoint ep;
    assert(idf_imb_parse_webhook_url("https://imb.leolan.top/api/v1/sim-sms/webhook?token=imb_sim_abc", ep));
    assert(ep.base == "https://imb.leolan.top" && ep.token == "imb_sim_abc");
    assert(idf_imb_parse_webhook_url("https://x.test/api/v1/sim-sms/webhook?a=1&token=t1&b=2", ep));
    assert(ep.base == "https://x.test" && ep.token == "t1");
    assert(!idf_imb_parse_webhook_url("https://sctapi.ftqq.com/key.send", ep));
    assert(!idf_imb_parse_webhook_url("https://x.test/api/v1/sim-sms/webhook", ep));
    assert(!idf_imb_parse_webhook_url("https://x.test/api/v1/sim-sms/webhook?token=", ep));

    IdfImbTask task;
    assert(idf_imb_parse_task("id-1\n10086\nhello\nworld", task));
    assert(task.id == "id-1" && task.phone == "10086" && task.content == "hello\nworld");
    assert(!idf_imb_parse_task("", task));
    assert(!idf_imb_parse_task("id-1\n10086", task));
    assert(!idf_imb_parse_task("id-1\n10086\n", task));
    assert(!idf_imb_parse_task("id-1\n\ncontent", task));

    assert(idf_imb_task_id_valid("7b916d88-7500-4d89-a537-b004f38c2c1d"));
    assert(!idf_imb_task_id_valid("../../admin"));
    assert(!idf_imb_task_id_valid(""));

    assert(idf_imb_json_escape("a\"b\\c\nd") == "a\\\"b\\\\c\\nd");
    assert(idf_imb_json_escape(std::string("\x01")) == "\\u0001");
    assert(idf_imb_json_escape("短信发送成功") == "短信发送成功");
    return 0;
}
