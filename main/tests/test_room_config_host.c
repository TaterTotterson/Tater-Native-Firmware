#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "../room_config.h"

#define ROOM_LEN 64

static void fill(char *buffer, char value, size_t count)
{
    memset(buffer, value, count);
    buffer[count] = '\0';
}

static void assert_room_ok(const char *input, const char *expected)
{
    char output[ROOM_LEN];
    memset(output, 'x', sizeof(output));
    assert(tater_room_config_normalize(input, output, sizeof(output)) == TATER_ROOM_CONFIG_OK);
    assert(strcmp(output, expected) == 0);
}

static void assert_room_rejected(const char *input, tater_room_config_result_t expected)
{
    char output[ROOM_LEN];
    memset(output, 'x', sizeof(output));
    assert(tater_room_config_normalize(input, output, sizeof(output)) == expected);
    assert(output[0] == '\0');
}

static void test_trimming_and_clear(void)
{
    assert_room_ok("Kitchen", "Kitchen");
    assert_room_ok("  Living Room  ", "Living Room");
    assert_room_ok("\t\r\n Office \n\t", "Office");
    assert_room_ok("Guest  Bedroom", "Guest  Bedroom");
    assert_room_ok("", "");
    assert_room_ok("   ", "");
    assert_room_ok(" \t\r\n\v\f ", "");
}

static void test_length_limits(void)
{
    char input[256];
    char expected[ROOM_LEN];

    fill(input, 'a', ROOM_LEN - 1);
    assert_room_ok(input, input);

    fill(input, 'a', ROOM_LEN);
    assert_room_rejected(input, TATER_ROOM_CONFIG_TOO_LONG);

    fill(input, 'a', 200);
    assert_room_rejected(input, TATER_ROOM_CONFIG_TOO_LONG);

    // 63 payload bytes plus surrounding whitespace is 73 bytes raw but fits.
    fill(expected, 'b', ROOM_LEN - 1);
    snprintf(input, sizeof(input), "     %s     ", expected);
    assert(strlen(input) > ROOM_LEN);
    assert_room_ok(input, expected);

    // 64 payload bytes still overflows once trimmed.
    char long_room[ROOM_LEN + 1];
    fill(long_room, 'c', ROOM_LEN);
    snprintf(input, sizeof(input), "  %s  ", long_room);
    assert_room_rejected(input, TATER_ROOM_CONFIG_TOO_LONG);

    // A multibyte character that would straddle the limit is rejected whole.
    fill(input, 'd', ROOM_LEN - 2);
    strcat(input, "\xC3\xA9");
    assert(strlen(input) == ROOM_LEN);
    assert_room_rejected(input, TATER_ROOM_CONFIG_TOO_LONG);

    // Smaller destinations are honoured.
    char small[4];
    assert(tater_room_config_normalize("abc", small, sizeof(small)) == TATER_ROOM_CONFIG_OK);
    assert(strcmp(small, "abc") == 0);
    assert(tater_room_config_normalize("abcd", small, sizeof(small)) == TATER_ROOM_CONFIG_TOO_LONG);
    assert(small[0] == '\0');
}

static void test_multibyte_utf8(void)
{
    assert_room_ok("Caf\xC3\xA9", "Caf\xC3\xA9");
    assert_room_ok("  \xC3\x85rsta  ", "\xC3\x85rsta");
    assert_room_ok("\xE5\xAE\xA2\xE5\x8E\x85", "\xE5\xAE\xA2\xE5\x8E\x85");
    assert_room_ok("Game Room \xF0\x9F\x8E\xAE", "Game Room \xF0\x9F\x8E\xAE");
    // Boundary code points: U+0080, U+07FF, U+0800, U+D7FF, U+E000, U+FFFF,
    // U+10000, and U+10FFFF.
    assert_room_ok("\xC2\x80", "\xC2\x80");
    assert_room_ok("\xDF\xBF", "\xDF\xBF");
    assert_room_ok("\xE0\xA0\x80", "\xE0\xA0\x80");
    assert_room_ok("\xED\x9F\xBF", "\xED\x9F\xBF");
    assert_room_ok("\xEE\x80\x80", "\xEE\x80\x80");
    assert_room_ok("\xEF\xBF\xBF", "\xEF\xBF\xBF");
    assert_room_ok("\xF0\x90\x80\x80", "\xF0\x90\x80\x80");
    assert_room_ok("\xF4\x8F\xBF\xBF", "\xF4\x8F\xBF\xBF");

    // Exactly 63 bytes made of 21 three-byte characters.
    char input[ROOM_LEN];
    input[0] = '\0';
    for (int i = 0; i < 21; i++) {
        strcat(input, "\xE5\xAE\xA2");
    }
    assert(strlen(input) == ROOM_LEN - 1);
    assert_room_ok(input, input);
}

static void test_invalid_utf8(void)
{
    static const char *cases[] = {
        "\x80",                       // lone continuation byte
        "Den\xBF",                    // trailing lone continuation byte
        "\xC3",                       // truncated 2-byte sequence
        "Caf\xC3",                    // truncated at end of string
        "\xE5\xAE",                   // truncated 3-byte sequence
        "\xF0\x9F\x8E",               // truncated 4-byte sequence
        "\xC3\x28",                   // bad continuation byte
        "\xE5\x28\xA2",               // bad continuation in 3-byte sequence
        "\xF0\x9F\x28\xAE",           // bad continuation in 4-byte sequence
        "\xC0\xAF",                   // overlong '/'
        "\xC1\xBF",                   // overlong 2-byte
        "\xE0\x80\xAF",               // overlong 3-byte
        "\xE0\x9F\xBF",               // overlong 3-byte (U+07FF)
        "\xF0\x80\x80\xAF",           // overlong 4-byte
        "\xF0\x8F\xBF\xBF",           // overlong 4-byte (U+FFFF)
        "\xED\xA0\x80",               // high surrogate U+D800
        "\xED\xBF\xBF",               // low surrogate U+DFFF
        "\xF4\x90\x80\x80",           // U+110000
        "\xF5\x80\x80\x80",           // lead byte above F4
        "\xFF",                       // invalid lead byte
        "\xFE",                       // invalid lead byte
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        assert_room_rejected(cases[i], TATER_ROOM_CONFIG_INVALID_UTF8);
    }
    // Trimming must not hide a truncated sequence before trailing spaces.
    assert_room_rejected("Caf\xC3   ", TATER_ROOM_CONFIG_INVALID_UTF8);
}

static void test_control_characters(void)
{
    assert_room_rejected("Living\tRoom", TATER_ROOM_CONFIG_CONTROL_CHARACTER);
    assert_room_rejected("Living\nRoom", TATER_ROOM_CONFIG_CONTROL_CHARACTER);
    assert_room_rejected("Living\rRoom", TATER_ROOM_CONFIG_CONTROL_CHARACTER);
    assert_room_rejected("Bell\x07", TATER_ROOM_CONFIG_CONTROL_CHARACTER);
    assert_room_rejected("\x1B[31mRed", TATER_ROOM_CONFIG_CONTROL_CHARACTER);
    assert_room_rejected("Del\x7F", TATER_ROOM_CONFIG_CONTROL_CHARACTER);
    assert_room_rejected("\x7F", TATER_ROOM_CONFIG_CONTROL_CHARACTER);
    assert_room_rejected(" Den\x01 ", TATER_ROOM_CONFIG_CONTROL_CHARACTER);
}

static void test_code_strings(void)
{
    assert(strcmp(tater_room_config_result_code(TATER_ROOM_CONFIG_OK), "ok") == 0);
    assert(strcmp(tater_room_config_result_code(TATER_ROOM_CONFIG_INVALID_ARGUMENT), "invalid_argument") == 0);
    assert(strcmp(tater_room_config_result_code(TATER_ROOM_CONFIG_INVALID_TYPE), "invalid_type") == 0);
    assert(strcmp(tater_room_config_result_code(TATER_ROOM_CONFIG_TOO_LONG), "too_long") == 0);
    assert(strcmp(tater_room_config_result_code(TATER_ROOM_CONFIG_CONTROL_CHARACTER), "control_character") == 0);
    assert(strcmp(tater_room_config_result_code(TATER_ROOM_CONFIG_INVALID_UTF8), "invalid_utf8") == 0);
    assert(strcmp(tater_room_config_result_code((tater_room_config_result_t)99), "unknown_error") == 0);

    assert(tater_room_config_result_message(TATER_ROOM_CONFIG_OK)[0] == '\0');
    assert(tater_room_config_result_message(TATER_ROOM_CONFIG_TOO_LONG)[0] != '\0');
    assert(tater_room_config_result_message(TATER_ROOM_CONFIG_INVALID_UTF8)[0] != '\0');
    assert(tater_room_config_result_message((tater_room_config_result_t)99)[0] != '\0');
}

static void test_null_arguments(void)
{
    char output[ROOM_LEN];
    memset(output, 'x', sizeof(output));
    assert(tater_room_config_normalize(NULL, output, sizeof(output)) == TATER_ROOM_CONFIG_INVALID_TYPE);
    assert(output[0] == '\0');
    assert(tater_room_config_normalize("Kitchen", NULL, sizeof(output)) == TATER_ROOM_CONFIG_INVALID_ARGUMENT);
    assert(tater_room_config_normalize("Kitchen", output, 0) == TATER_ROOM_CONFIG_INVALID_ARGUMENT);
    assert(tater_room_config_normalize(NULL, NULL, 0) == TATER_ROOM_CONFIG_INVALID_ARGUMENT);

    // A one-byte destination only fits the empty room.
    char tiny[1] = {'x'};
    assert(tater_room_config_normalize("  ", tiny, sizeof(tiny)) == TATER_ROOM_CONFIG_OK);
    assert(tiny[0] == '\0');
    assert(tater_room_config_normalize("a", tiny, sizeof(tiny)) == TATER_ROOM_CONFIG_TOO_LONG);
    assert(tiny[0] == '\0');
}

static bool has_nul_escape(const char *json)
{
    return tater_room_config_json_has_nul_escape(json, strlen(json));
}

static void test_json_nul_escape(void)
{
    // cJSON decodes \u0000 to an embedded NUL, so it is caught on the raw text.
    assert(has_nul_escape("{\"room\":\"Kitchen\\u0000\\u0007junk\"}"));
    assert(has_nul_escape("{\"room\":\"\\u0000\"}"));
    assert(has_nul_escape("\\u0000"));
    // Three backslashes: an escaped backslash followed by a real \u0000.
    assert(has_nul_escape("\"a\\\\\\u0000\""));

    assert(!has_nul_escape("{\"room\":\"Kitchen\"}"));
    assert(!has_nul_escape("{\"room\":\"Caf\\u00e9\"}"));
    assert(!has_nul_escape("{\"room\":\"\\u0001\"}"));
    // An escaped backslash then literal "u0000" is not a NUL escape.
    assert(!has_nul_escape("{\"room\":\"a\\\\u0000\"}"));
    assert(!has_nul_escape("u0000"));
    assert(!has_nul_escape(""));

    // A truncated escape at the end of the buffer is not read past len.
    const char partial[] = "\"\\u0000\"";
    assert(!tater_room_config_json_has_nul_escape(partial, 5));
    assert(tater_room_config_json_has_nul_escape(partial, 7));
    assert(!tater_room_config_json_has_nul_escape(NULL, 10));
}

int main(void)
{
    test_trimming_and_clear();
    test_length_limits();
    test_multibyte_utf8();
    test_invalid_utf8();
    test_control_characters();
    test_code_strings();
    test_null_arguments();
    test_json_nul_escape();
    puts("Room config host tests passed.");
    return 0;
}
