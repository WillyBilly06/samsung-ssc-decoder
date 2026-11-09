# Benchmarks the SSC decoder on the ESP32 with real frames from Samsung's encoder.
set(dec "${CMAKE_CURRENT_LIST_DIR}/../../../decoder")
get_filename_component(dec "${dec}" ABSOLUTE)
idf_component_register(SRCS "bench.c"
    "${dec}/ssc_alloc.c" "${dec}/ssc_bands.c" "${dec}/ssc_cwrs.c" "${dec}/ssc_decoder.c" "${dec}/ssc_ec.c"
    "${dec}/ssc_energy.c" "${dec}/ssc_synth.c" "${dec}/ssc_tables.c" "${dec}/ssc_twiddle.c"
    INCLUDE_DIRS "${dec}"
    EMBED_FILES "f44_192k.ssc" "hifi48_192k.ssc" "uhq96_584k.ssc")
target_compile_options(${COMPONENT_LIB} PRIVATE -O2 -fsingle-precision-constant -fno-math-errno -Wno-error -DSSC_PROFILE)
