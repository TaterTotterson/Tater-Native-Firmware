#pragma once
static inline void xmos_test_log(const char *format, ...)
{
    (void)format;
}
#define ESP_LOGE(tag, ...) ((void)(tag), xmos_test_log(__VA_ARGS__))
#define ESP_LOGW(tag, ...) ((void)(tag), xmos_test_log(__VA_ARGS__))
#define ESP_LOGI(tag, ...) ((void)(tag), xmos_test_log(__VA_ARGS__))
