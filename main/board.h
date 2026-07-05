#pragma once

#if !defined(TATER_BOARD_VOICE_PE)
#define TATER_BOARD_VOICE_PE 1
#endif

#if TATER_BOARD_VOICE_PE
#include "boards/voice_pe/board_voice_pe.h"
#else
#error "No supported Tater native satellite board selected."
#endif
