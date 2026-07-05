#include "wake_sound_assets.h"

#include <string.h>

extern const uint8_t _binary_blip2_wav_start[] asm("_binary_blip2_wav_start");
extern const uint8_t _binary_blip2_wav_end[] asm("_binary_blip2_wav_end");
extern const uint8_t _binary_message_notification_4_wav_start[] asm("_binary_message_notification_4_wav_start");
extern const uint8_t _binary_message_notification_4_wav_end[] asm("_binary_message_notification_4_wav_end");
extern const uint8_t _binary_notification_ding_wav_start[] asm("_binary_notification_ding_wav_start");
extern const uint8_t _binary_notification_ding_wav_end[] asm("_binary_notification_ding_wav_end");
extern const uint8_t _binary_notification_squeak_wav_start[] asm("_binary_notification_squeak_wav_start");
extern const uint8_t _binary_notification_squeak_wav_end[] asm("_binary_notification_squeak_wav_end");
extern const uint8_t _binary_phone_chime_wav_start[] asm("_binary_phone_chime_wav_start");
extern const uint8_t _binary_phone_chime_wav_end[] asm("_binary_phone_chime_wav_end");
extern const uint8_t _binary_pop_up_sound_wav_start[] asm("_binary_pop_up_sound_wav_start");
extern const uint8_t _binary_pop_up_sound_wav_end[] asm("_binary_pop_up_sound_wav_end");
extern const uint8_t _binary_short_definite_fart_wav_start[] asm("_binary_short_definite_fart_wav_start");
extern const uint8_t _binary_short_definite_fart_wav_end[] asm("_binary_short_definite_fart_wav_end");
extern const uint8_t _binary_star_treck_communications_start_transmission_wav_start[] asm("_binary_star_treck_communications_start_transmission_wav_start");
extern const uint8_t _binary_star_treck_communications_start_transmission_wav_end[] asm("_binary_star_treck_communications_start_transmission_wav_end");
extern const uint8_t _binary_star_treck_computer_work_beep_wav_start[] asm("_binary_star_treck_computer_work_beep_wav_start");
extern const uint8_t _binary_star_treck_computer_work_beep_wav_end[] asm("_binary_star_treck_computer_work_beep_wav_end");
extern const uint8_t _binary_tater_notify_digital_blip_wav_start[] asm("_binary_tater_notify_digital_blip_wav_start");
extern const uint8_t _binary_tater_notify_digital_blip_wav_end[] asm("_binary_tater_notify_digital_blip_wav_end");
extern const uint8_t _binary_turning_off_microphone_percussion_1_wav_start[] asm("_binary_turning_off_microphone_percussion_1_wav_start");
extern const uint8_t _binary_turning_off_microphone_percussion_1_wav_end[] asm("_binary_turning_off_microphone_percussion_1_wav_end");
extern const uint8_t _binary_wake_word_triggered_wav_start[] asm("_binary_wake_word_triggered_wav_start");
extern const uint8_t _binary_wake_word_triggered_wav_end[] asm("_binary_wake_word_triggered_wav_end");
extern const uint8_t _binary_waterdrop_wav_start[] asm("_binary_waterdrop_wav_start");
extern const uint8_t _binary_waterdrop_wav_end[] asm("_binary_waterdrop_wav_end");

#define WAKE_SOUND_ASSET(asset_id, symbol_name) \
    { asset_id, _binary_##symbol_name##_wav_start, _binary_##symbol_name##_wav_end }

static const tater_wake_sound_asset_t s_assets[] = {
    WAKE_SOUND_ASSET("blip2", blip2),
    WAKE_SOUND_ASSET("message-notification-4", message_notification_4),
    WAKE_SOUND_ASSET("notification-ding", notification_ding),
    WAKE_SOUND_ASSET("notification-squeak", notification_squeak),
    WAKE_SOUND_ASSET("phone-chime", phone_chime),
    WAKE_SOUND_ASSET("pop-up-sound", pop_up_sound),
    WAKE_SOUND_ASSET("short-definite-fart", short_definite_fart),
    WAKE_SOUND_ASSET("star_treck_communications_start_transmission", star_treck_communications_start_transmission),
    WAKE_SOUND_ASSET("star_treck_computer_work_beep", star_treck_computer_work_beep),
    WAKE_SOUND_ASSET("tater_notify_digital_blip", tater_notify_digital_blip),
    WAKE_SOUND_ASSET("turning-off-microphone-percussion-1", turning_off_microphone_percussion_1),
    WAKE_SOUND_ASSET("wake_word_triggered", wake_word_triggered),
    WAKE_SOUND_ASSET("waterdrop", waterdrop),
};

const tater_wake_sound_asset_t *tater_wake_sound_asset_lookup(const char *id)
{
    if (!id || id[0] == '\0' || strcmp(id, "no_sound") == 0 || strcmp(id, "custom") == 0) {
        return NULL;
    }
    if (strcmp(id, "default") == 0) {
        id = "wake_word_triggered";
    }
    for (size_t i = 0; i < (sizeof(s_assets) / sizeof(s_assets[0])); i++) {
        if (strcmp(id, s_assets[i].id) == 0) {
            return &s_assets[i];
        }
    }
    return NULL;
}
