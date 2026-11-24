#**********************
# Gather Sources
#**********************

set(SAT1_XMOS_PIPELINE "legacy" CACHE STRING "legacy, aec4, or aec4_beam")
set_property(CACHE SAT1_XMOS_PIPELINE PROPERTY STRINGS legacy aec4 aec4_beam)
if(NOT SAT1_XMOS_PIPELINE MATCHES "^(legacy|aec4|aec4_beam)$")
    message(FATAL_ERROR "Invalid SAT1_XMOS_PIPELINE: ${SAT1_XMOS_PIPELINE}")
endif()
set(SAT1_XMOS_VERSION "v1.2.0-dev.1" CACHE STRING "Candidate firmware identity")

file(GLOB_RECURSE APP_SOURCES   ${CMAKE_CURRENT_LIST_DIR}/src/*.c)

set(APP_INCLUDES
    ${CMAKE_CURRENT_LIST_DIR}/src
    ${CMAKE_CURRENT_LIST_DIR}/src/control
    ${CMAKE_CURRENT_LIST_DIR}/audio_pipelines
    ${CMAKE_CURRENT_LIST_DIR}/src/dfu_int
    ${CMAKE_CURRENT_LIST_DIR}/src/usb
)

include(${CMAKE_CURRENT_LIST_DIR}/bsp_config/bsp_config.cmake)
add_subdirectory(${CMAKE_CURRENT_LIST_DIR}/audio_pipelines)

set(VERSIONING_SCRIPT ${CMAKE_CURRENT_LIST_DIR}/versioning.py)
option(USE_DEV_TRACKING "Enable dev-build tracking" OFF)

#**********************
# Flags
#**********************
set(APP_COMPILER_FLAGS
    -Os
    -g
    -report
    -mcmodel=large
    -Wno-xcore-fptrgroup
)

set(APP_COMPILE_DEFINITIONS
    DEBUG_PRINT_ENABLE=0
    PLATFORM_USES_TILE_0=1
    PLATFORM_USES_TILE_1=1
    XUD_CORE_CLOCK=600

    CFG_TUSB_DEBUG_PRINTF=rtos_printf
    CFG_TUSB_DEBUG=0
)

set(APP_LINK_OPTIONS
    -lquadspi
    -report
    -lotp3
)

set(APP_COMMON_LINK_LIBRARIES
    rtos::freertos_usb
    fph::device_control
    fph::qspi_flash_ext
    lib_src
    lib_sw_pll
)


#**********************
# Pipeline Options
# By default only these targets are created:
#  example_ffva_int_fixed_delay
#  example_ffva_ua_adec_altarch
#**********************
option(ENABLE_ALL_FFVA_PIPELINES  "Create all FFVA pipeline configurations"  OFF)

if(ENABLE_ALL_FFVA_PIPELINES)
    set(FFVA_PIPELINES_INT
        bypass
        fixed_delay
        adec
        adec_altarch
        empty
    )

    set(FFVA_PIPELINES_UA
        fixed_delay
        adec
        adec_altarch
        empty
    )
else()
    set(FFVA_PIPELINES_INT
        adec
        fixed_delay
        bypass
    )

    set(FFVA_PIPELINES_UA
        adec_altarch
    )
endif()

#**********************
# XMOS Firmware Targets
#**********************
include(${CMAKE_CURRENT_LIST_DIR}/satellite1.cmake)
include(${CMAKE_CURRENT_LIST_DIR}/satellite1-usb.cmake)
include(${CMAKE_CURRENT_LIST_DIR}/explorer_devboard.cmake)

