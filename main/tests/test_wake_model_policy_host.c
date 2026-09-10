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

    puts("Wake model policy host tests passed.");
    return 0;
}
