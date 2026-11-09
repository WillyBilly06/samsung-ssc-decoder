# SSC decoder library.
#  - ESP-IDF: this folder is a component named "ssc_decoder" (add it with EXTRA_COMPONENT_DIRS or the component
#    manager, then REQUIRES ssc_decoder).
#  - Plain CMake: add_subdirectory(ssc_decoder) defines the static library "sscdec".
set(SSC_DECODER_SOURCES
    ssc_alloc.c ssc_bands.c ssc_cwrs.c ssc_decoder.c ssc_ec.c
    ssc_energy.c ssc_synth.c ssc_tables.c ssc_twiddle.c)

if(ESP_PLATFORM)
    idf_component_register(SRCS ${SSC_DECODER_SOURCES} INCLUDE_DIRS ".")
    # The ESP32 FPU is single precision: keep constants float, and skip errno handling in libm.
    target_compile_options(${COMPONENT_LIB} PRIVATE -O2 -fsingle-precision-constant -fno-math-errno)
    return()
endif()

add_library(sscdec ${SSC_DECODER_SOURCES})
target_include_directories(sscdec PUBLIC ${CMAKE_CURRENT_LIST_DIR})
if(NOT MSVC)
    target_compile_options(sscdec PRIVATE -O2 -fno-math-errno)
    target_link_libraries(sscdec PUBLIC m)
endif()
