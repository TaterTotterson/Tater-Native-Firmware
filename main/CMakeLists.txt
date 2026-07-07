idf_component_register(
    SRCS
        "app_main.c"
        "boards/voice_pe/audio_voice_pe.c"
        "boards/voice_pe/button_voice_pe.c"
        "boards/voice_pe/led_ring_voice_pe.c"
        "cache_storage.c"
        "native_settings.c"
        "ota_update.c"
        "playback.c"
        "provisioning.c"
        "tater_config.c"
        "tater_protocol.c"
        "wake_engine.cc"
        "wake_model_assets.c"
        "wake_sound_assets.c"
        "wifi_station.c"
    INCLUDE_DIRS "."
    REQUIRES
        driver
        app_update
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
        nvs_flash
        spiffs
        tflm_microfrontend
)

target_add_binary_data(${COMPONENT_LIB} "models/hey_tater.tflite" BINARY)
target_add_binary_data(${COMPONENT_LIB} "boards/voice_pe/xmos/ffva_v1.3.2-vod_upgrade.bin" BINARY)
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
