# Host build of the SSC decoder library and the example decode tool.
#   cmake -B build && cmake --build build
#   build/ssc_dec_tool test_vectors/ssc/mix_192000.ssc out.s16le 44100
cmake_minimum_required(VERSION 3.16)
project(samsung_ssc_decoder C)

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)

add_library(sscdec
    decoder/ssc_alloc.c decoder/ssc_bands.c decoder/ssc_cwrs.c decoder/ssc_decoder.c decoder/ssc_ec.c
    decoder/ssc_energy.c decoder/ssc_synth.c decoder/ssc_tables.c decoder/ssc_twiddle.c)
target_include_directories(sscdec PUBLIC decoder)
# Per-frame bit position, printed by ssc_dec_tool when SSC_TELL is set in the environment.
target_compile_definitions(sscdec PUBLIC SSC_DEBUG_TELL)
if(NOT MSVC)
    target_compile_options(sscdec PRIVATE -O2 -fno-math-errno)
    target_link_libraries(sscdec PUBLIC m)
endif()

add_executable(ssc_dec_tool examples/host_decode/ssc_dec_tool.c)
target_link_libraries(ssc_dec_tool PRIVATE sscdec)
