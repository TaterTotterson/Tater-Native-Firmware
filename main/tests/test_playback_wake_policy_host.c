#include <assert.h>
#include <stdio.h>

#include "../playback_wake_policy.h"

int main(void)
{
    /* Idle satellites always listen. */
    assert(tater_playback_wake_allowed(false, false, false, false));

    /* Replies and announcements require barge-in. */
    assert(!tater_playback_wake_allowed(true, false, false, true));
    assert(tater_playback_wake_allowed(true, false, true, true));

    /* Uninterrupted music remains wakeable on hardware-AEC boards. */
    assert(tater_playback_wake_allowed(true, true, false, true));

    /* An active reply overlay temporarily suppresses ordinary music wake. */
    assert(!tater_playback_wake_allowed(true, false, false, true));

    /* Boards without playback-time wake support still require barge-in. */
    assert(!tater_playback_wake_allowed(true, true, false, false));
    assert(tater_playback_wake_allowed(true, true, true, false));

    puts("Playback wake policy host tests passed.");
    return 0;
}
