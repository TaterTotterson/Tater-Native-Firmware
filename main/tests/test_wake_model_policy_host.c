#include <assert.h>
#include <stdio.h>

#include "../wake_model_policy.h"

int main(void)
{
    assert(!tater_wake_model_config_changed(
        "custom_url",
        "https://example.test/hey.json",
        "revision-1",
        "custom_url",
        "https://example.test/hey.json",
        "revision-1"
    ));
    assert(tater_wake_model_config_changed(
        "hey_tater",
        "",
        "",
        "custom_url",
        "https://example.test/hey.json",
        "revision-1"
    ));
    assert(tater_wake_model_config_changed(
        "custom_url",
        "https://example.test/hey.json",
        "revision-1",
        "custom_url",
        "https://example.test/updated.json",
        "revision-1"
    ));
    assert(tater_wake_model_config_changed(
        "custom_url",
        "https://example.test/hey.json",
        "revision-1",
        "custom_url",
        "https://example.test/hey.json",
        "revision-2"
    ));
    assert(!tater_wake_model_config_changed(NULL, NULL, NULL, "", "", ""));

    assert(tater_background_download_allowed(false));
    assert(!tater_background_download_allowed(true));

    assert(tater_download_allocation_size(137984, 512 * 1024, false) == 137984);
    assert(tater_download_allocation_size(137984, 512 * 1024, true) == 137985);
    assert(tater_download_allocation_size(-1, 512 * 1024, false) == 512 * 1024);
    assert(tater_download_allocation_size(0, 16 * 1024, true) == (16 * 1024) + 1);
    assert(tater_download_allocation_size((512 * 1024) + 1, 512 * 1024, false) == 0);
    assert(tater_download_allocation_size(10, 0, false) == 0);

    assert(tater_download_internal_fallback_allowed(1024));
    assert(tater_download_internal_fallback_allowed(32 * 1024));
    assert(!tater_download_internal_fallback_allowed((32 * 1024) + 1));
    assert(!tater_download_internal_fallback_allowed(512 * 1024));

    puts("Wake model policy host tests passed.");
    return 0;
}
