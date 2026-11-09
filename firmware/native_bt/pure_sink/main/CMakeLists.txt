# Pure playback sink. Sources are listed explicitly (no source globbing, see
# native_bt/tools/check_workspace.py). Stack and codecs come from the shared
# components in ../esp32_probe/components (added by the project CMakeLists).
idf_component_register(
    SRCS "main.c" "bt_security.c" "bt_sdp.c" "a2dp_sink.c" "sink_avrcp.c" "audio_out.c" "button.c"
    INCLUDE_DIRS "."
    PRIV_REQUIRES native_bt codec_core sbc_decoder opus_decoder aac_decoder aptx_decoder ldac_decoder ssc_decoder
                  bt nvs_flash esp_timer esp_driver_i2s esp_driver_gpio heap log freertos)
target_compile_options(${COMPONENT_LIB} PRIVATE -Wall -Wextra -Werror)
