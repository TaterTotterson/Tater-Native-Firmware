#include "playback_wake_policy.h"

bool tater_playback_wake_allowed(
    bool playback_active,
    bool uninterrupted_media_active,
    bool barge_in_enabled,
    bool wake_during_media_supported
)
{
    if (!playback_active) {
        return true;
    }
    if (barge_in_enabled) {
        return true;
    }
    return wake_during_media_supported && uninterrupted_media_active;
}
