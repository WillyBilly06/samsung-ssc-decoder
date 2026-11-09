idf_component_register(
    SRCS "playback.c"
    INCLUDE_DIRS "."
    PRIV_REQUIRES ssc_decoder esp_driver_i2s esp_timer
    # Three streams from Samsung's encoder (see test_vectors/README.md): 44.1 kHz, HiFi 48 kHz/24-bit, UHQ 96 kHz.
    EMBED_FILES "../../../test_vectors/ssc/mix_192000.ssc"
                "../../../test_vectors/ssc/hifi48_192000.ssc"
                "../../../test_vectors/ssc/uhq96_584000.ssc")
