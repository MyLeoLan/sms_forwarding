#pragma once

#include "esp_err.h"

// IMB 发件轮询：定时向 IMB 拉取待发短信，发送后回报结果。
// 无需单独配置：从已启用的推送通道里找 IMB webhook 地址，复用其中的卡令牌。
esp_err_t idf_imb_start(void);
