#pragma once

#if !defined(TATER_BOARD_VOICE_PE) && !defined(TATER_BOARD_SAT1)
#define TATER_BOARD_VOICE_PE 1
#endif

#if !defined(TATER_BOARD_VOICE_PE)
#define TATER_BOARD_VOICE_PE 0
#endif

#if !defined(TATER_BOARD_SAT1)
#define TATER_BOARD_SAT1 0
#endif

#if TATER_BOARD_VOICE_PE
#include "boards/voice_pe/board_voice_pe.h"
#elif TATER_BOARD_SAT1
#include "boards/sat1/board_sat1.h"
#else
#error "No supported Tater native satellite board selected."
#endif
