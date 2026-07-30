#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "../server_url.h"

typedef struct {
    const char *input;
    const char *base_url;
    const char *websocket_url;
} valid_url_case_t;

static void test_valid_urls(void)
{
    static const valid_url_case_t cases[] = {
        {
            "192.168.1.70:8501",
            "http://192.168.1.70:8501",
            "ws://192.168.1.70:8501/api/tater/satellite/v1/ws",
        },
        {
            "  HTTP://tater.local:8501/  ",
            "http://tater.local:8501",
            "ws://tater.local:8501/api/tater/satellite/v1/ws",
        },
        {
            "ws://tater.local:8501",
            "http://tater.local:8501",
            "ws://tater.local:8501/api/tater/satellite/v1/ws",
        },
        {
            "http://192.168.1.70:8501/api/tater/satellite/v1/ws",
            "http://192.168.1.70:8501",
            "ws://192.168.1.70:8501/api/tater/satellite/v1/ws",
        },
        {
            "ws://192.168.1.70:8501/api/tater/satellite/v1/ws/",
            "http://192.168.1.70:8501",
            "ws://192.168.1.70:8501/api/tater/satellite/v1/ws",
        },
        {
            "https://tater.example.com",
            "https://tater.example.com",
            "wss://tater.example.com/api/tater/satellite/v1/ws",
        },
        {
            "wss://tater.example.com/api/tater/satellite/v1/ws",
            "https://tater.example.com",
            "wss://tater.example.com/api/tater/satellite/v1/ws",
        },
        {
            "https://tater.example.com/tater/",
            "https://tater.example.com/tater",
            "wss://tater.example.com/tater/api/tater/satellite/v1/ws",
        },
        {
            "https://tater.example.com/tater/api/tater/satellite/v1/ws?unused=1",
            "https://tater.example.com/tater",
            "wss://tater.example.com/tater/api/tater/satellite/v1/ws",
        },
        {
            "[2001:db8::20]:8501",
            "http://[2001:db8::20]:8501",
            "ws://[2001:db8::20]:8501/api/tater/satellite/v1/ws",
        },
    };

    for (size_t index = 0; index < sizeof(cases) / sizeof(cases[0]); index++) {
        char base_url[192];
        char websocket_url[224];
        assert(tater_server_normalize_base_url(cases[index].input, base_url, sizeof(base_url)));
        assert(strcmp(base_url, cases[index].base_url) == 0);
        assert(tater_server_build_ws_url(cases[index].input, websocket_url, sizeof(websocket_url)));
        assert(strcmp(websocket_url, cases[index].websocket_url) == 0);
    }
}

static void test_invalid_urls(void)
{
    static const char *cases[] = {
        "",
        "   ",
        "http://",
        "https://",
        "ftp://tater.example.com",
        "http://http://tater.example.com",
        "http://bad host:8501",
        "://tater.example.com",
        "[2001:db8::20",
    };

    for (size_t index = 0; index < sizeof(cases) / sizeof(cases[0]); index++) {
        char output[192] = "unchanged";
        assert(!tater_server_normalize_base_url(cases[index], output, sizeof(output)));
        assert(output[0] == '\0');
        assert(!tater_server_build_ws_url(cases[index], output, sizeof(output)));
        assert(output[0] == '\0');
    }
}

static void test_small_output_buffer_is_rejected(void)
{
    char output[12];
    assert(!tater_server_normalize_base_url("http://tater.local:8501", output, sizeof(output)));
    assert(output[0] == '\0');
    assert(!tater_server_build_ws_url("http://tater.local:8501", output, sizeof(output)));
    assert(output[0] == '\0');
}

int main(void)
{
    test_valid_urls();
    test_invalid_urls();
    test_small_output_buffer_is_rejected();
    puts("Server URL host tests passed.");
    return 0;
}
