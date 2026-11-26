set(TATER_USB_REQUIRES "")
if(CONFIG_TATER_USB_ETHERNET)
    list(APPEND TATER_USB_REQUIRES usb esp_driver_usb_serial_jtag iot_usbh_ecm)
endif()

idf_component_register(
    SRCS
        "app_main.c"
        "audio_aec.c"
        "ble_scanner.c"
        "ble_proxy.c"
        "boards/voice_pe/audio_voice_pe.c"
        "boards/voice_pe/voicepe_audio_hints.c"
        "boards/sat1/audio_sat1.c"
        "boards/sat1/sat1_control_protocol.c"
        "boards/sat1/sat1_pd_policy.c"
        "boards/sat1/sat1_power_delivery.c"
        "boards/respeaker_xvf3800/audio_respeaker_xvf3800.c"
        "boards/respeaker_xvf3800/xvf3800_doa.c"
        "boards/s3_box/audio_s3_box.c"
        "boards/s3_box/display_s3_box.c"
        "boards/voice_pe/button_voice_pe.c"
        "boards/voice_pe/led_ring_voice_pe.c"
        "cache_storage.c"
        "compressed_asset.c"
        "display_clock.c"
        "doa_activity_gate.c"
        "doa_direction_memory.c"
        "native_settings.c"
        "network_recovery.c"
        "network_state.c"
        "network_manager.c"
        "usb_ethernet.c"
        "ota_family.c"
        "ota_update.c"
        "playback.c"
        "playback_mix.c"
        "playback_recovery.c"
        "playback_wake_policy.c"
        "provisioning.c"
        "server_url.c"
        "sendspin_player.cpp"
        "task_cleanup.c"
        "tater_config.c"
        "tater_protocol.c"
        "timer_sound_assets.c"
        "wake_engine.cc"
        "wake_detection_policy.c"
        "wake_model_policy.c"
        "wake_model_assets.c"
        "wake_sound_assets.c"
        "wifi_station.c"
        "xmos_update.c"
    INCLUDE_DIRS "."
    REQUIRES
        ${TATER_USB_REQUIRES}
        driver
        app_update
        bt
        esp_event
        esp_http_client
        esp_http_server
        esp_netif
        esp_partition
        esp_timer
        esp_wifi
        esp_audio_codec
        esp_websocket_client
        esp-tflite-micro
        json
        led_strip
        mbedtls
        mdns
        nvs_flash
        pthread
        sendspin-cpp
        spiffs
        tflm_microfrontend
)

target_add_binary_data(${COMPONENT_LIB} "models/hey_tater.tflite" BINARY)
target_add_binary_data(${COMPONENT_LIB} "models/hey_tater.boundary.tflite" BINARY)
target_add_binary_data(${COMPONENT_LIB} "models/stop.tflite" BINARY)
if(TATER_NATIVE_BUILD_ENV STREQUAL "voicepe")
    target_add_binary_data(${COMPONENT_LIB} "boards/voice_pe/xmos/ffva_v1.3.4-vod_upgrade.bin.zlib" BINARY)
elseif(TATER_NATIVE_BUILD_ENV STREQUAL "sat1")
    if(TATER_SAT1_AUDIO_BENCH AND NOT TATER_SAT1_XMOS_VARIANT)
        message(FATAL_ERROR "Audio bench requires an explicit TATER_SAT1_XMOS_VARIANT")
    endif()
    if(NOT TATER_SAT1_XMOS_VARIANT)
        set(TATER_SAT1_XMOS_VARIANT "aec4_track")
    endif()
    if(DEFINED TATER_SAT1_XMOS_VARIANT AND NOT TATER_SAT1_XMOS_VARIANT STREQUAL "")
        if(NOT TATER_SAT1_XMOS_VARIANT MATCHES "^(beam_ic|legacy|aec4|aec4_beam|aec4_track)$")
            message(FATAL_ERROR "Unknown Sat1 XMOS candidate: ${TATER_SAT1_XMOS_VARIANT}")
        endif()
        if(TATER_SAT1_AUDIO_BENCH)
            set(SAT1_CANDIDATE_DIR "bench_candidates")
            set(SAT1_CANDIDATE_SUFFIX "_bench")
        else()
            set(SAT1_CANDIDATE_DIR "candidates")
            set(SAT1_CANDIDATE_SUFFIX "")
        endif()
        if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/boards/sat1/xmos/${SAT1_CANDIDATE_DIR}/${TATER_SAT1_XMOS_VARIANT}.h")
            message(FATAL_ERROR "Build and package the selected XMOS candidate before building its native host")
        endif()
        target_compile_definitions(${COMPONENT_LIB} PRIVATE
            TATER_SAT1_XMOS_CANDIDATE_HEADER="xmos/${SAT1_CANDIDATE_DIR}/${TATER_SAT1_XMOS_VARIANT}.h")
        target_add_binary_data(${COMPONENT_LIB}
            "boards/sat1/xmos/${SAT1_CANDIDATE_DIR}/sat1_xmos_${TATER_SAT1_XMOS_VARIANT}${SAT1_CANDIDATE_SUFFIX}.bin.zlib" BINARY)
    else()
        message(FATAL_ERROR "Satellite1 requires a packaged XMOS profile")
    endif()
elseif(TATER_NATIVE_BUILD_ENV STREQUAL "respeaker_xvf3800")
    target_add_binary_data(${COMPONENT_LIB} "boards/respeaker_xvf3800/xmos/xvf3800_i2s_1_0_7.bin.zlib" BINARY)
elseif(TATER_NATIVE_BUILD_ENV STREQUAL "s3_box")
    message(STATUS "Building S3 Box target without XMOS firmware payload")
else()
    target_add_binary_data(${COMPONENT_LIB} "boards/voice_pe/xmos/ffva_v1.3.4-vod_upgrade.bin.zlib" BINARY)
    target_add_binary_data(${COMPONENT_LIB} "boards/sat1/xmos/candidates/sat1_xmos_aec4_track.bin.zlib" BINARY)
    target_add_binary_data(${COMPONENT_LIB} "boards/respeaker_xvf3800/xmos/xvf3800_i2s_1_0_7.bin.zlib" BINARY)
endif()
if(TATER_NATIVE_BUILD_ENV STREQUAL "respeaker_xvf3800")
    # The 8 MB ReSpeaker has the same 3 MB OTA slot as the other targets plus
    # an 868 KiB XMOS payload. Keep its standard cue and omit novelty sounds.
    target_add_binary_data(${COMPONENT_LIB} "wake_sounds/wake_word_triggered.wav" BINARY)
else()
    target_add_binary_data(${COMPONENT_LIB} "wake_sounds/blip2.wav" BINARY)
    target_add_binary_data(${COMPONENT_LIB} "wake_sounds/message-notification-4.wav" BINARY)
    target_add_binary_data(${COMPONENT_LIB} "wake_sounds/notification-ding.wav" BINARY)
    target_add_binary_data(${COMPONENT_LIB} "wake_sounds/notification-squeak.wav" BINARY)
    target_add_binary_data(${COMPONENT_LIB} "wake_sounds/phone-chime.wav" BINARY)
    target_add_binary_data(${COMPONENT_LIB} "wake_sounds/pop-up-sound.wav" BINARY)
    target_add_binary_data(${COMPONENT_LIB} "wake_sounds/short-definite-fart.wav" BINARY)
    target_add_binary_data(${COMPONENT_LIB} "wake_sounds/star_treck_communications_start_transmission.wav" BINARY)
    target_add_binary_data(${COMPONENT_LIB} "wake_sounds/star_treck_computer_work_beep.wav" BINARY)
    target_add_binary_data(${COMPONENT_LIB} "wake_sounds/tater_notify_digital_blip.wav" BINARY)
    target_add_binary_data(${COMPONENT_LIB} "wake_sounds/turning-off-microphone-percussion-1.wav" BINARY)
    target_add_binary_data(${COMPONENT_LIB} "wake_sounds/wake_word_triggered.wav" BINARY)
    target_add_binary_data(${COMPONENT_LIB} "wake_sounds/waterdrop.wav" BINARY)
endif()
target_add_binary_data(${COMPONENT_LIB} "timer_sounds/zen_timer_alarm.wav" BINARY)
