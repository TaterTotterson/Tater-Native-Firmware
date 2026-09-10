#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Music sessions remain wakeable so a user can issue playback commands.
 * Replies and announcements require barge-in because their own speech is the
 * most likely playback source to resemble a configured wake word.
 */
bool tater_playback_wake_allowed(
    bool playback_active,
    bool uninterrupted_media_active,
    bool barge_in_enabled,
    bool wake_during_media_supported
);

#ifdef __cplusplus
}
#endif
