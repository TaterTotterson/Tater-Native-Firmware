#pragma once

#if !defined(TATER_BOARD_VOICE_PE) && !defined(TATER_BOARD_SAT1) && !defined(TATER_BOARD_RESPEAKER_XVF3800) && !defined(TATER_BOARD_S3_BOX)
#define TATER_BOARD_VOICE_PE 1
#endif

#if !defined(TATER_BOARD_VOICE_PE)
#define TATER_BOARD_VOICE_PE 0
#endif

#if !defined(TATER_BOARD_SAT1)
#define TATER_BOARD_SAT1 0
#endif

#if !defined(TATER_BOARD_RESPEAKER_XVF3800)
#define TATER_BOARD_RESPEAKER_XVF3800 0
#endif

#if !defined(TATER_BOARD_S3_BOX)
#define TATER_BOARD_S3_BOX 0
#endif

#if TATER_BOARD_VOICE_PE
#include "boards/voice_pe/board_voice_pe.h"
#elif TATER_BOARD_SAT1
#include "boards/sat1/board_sat1.h"
#elif TATER_BOARD_RESPEAKER_XVF3800
#include "boards/respeaker_xvf3800/board_respeaker_xvf3800.h"
#elif TATER_BOARD_S3_BOX
#include "boards/s3_box/board_s3_box.h"
#else
#error "No supported Tater native satellite board selected."
#endif

#if !defined(TATER_HAS_CENTER_BUTTON)
#define TATER_HAS_CENTER_BUTTON 1
#endif
