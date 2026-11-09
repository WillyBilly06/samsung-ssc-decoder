# BDK SPEAKER - pure A2DP playback sink on the native_bt host stack.
# Reuses (does not copy) the stack/codec components of esp32_probe.
cmake_minimum_required(VERSION 3.22)

get_filename_component(native_bt_root "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)
set(shared "${native_bt_root}/esp32_probe/components")

# Options read by the shared codec components (see their CMakeLists).
set(DBT_PROBE_SSC_BUILD ON CACHE BOOL "Build the SSC decoder component" FORCE)
set(DBT_PROBE_LHDC_BUILD OFF CACHE BOOL "LHDC is not part of pure_sink" FORCE)
set(DBT_PROBE_APTX_SNAPSHOT OFF CACHE BOOL "No aptX PCM snapshot diagnostics" FORCE)

# Only the components this firmware uses (lhdcv5_decoder is intentionally absent).
set(EXTRA_COMPONENT_DIRS
    "${shared}/native_bt"
    "${shared}/codec_core"
    "${shared}/sbc_decoder"
    "${shared}/aac_decoder"
    "${shared}/aptx_decoder"
    "${shared}/ldac_decoder"
    "${shared}/opus_decoder"
    "${shared}/ssc_decoder")
set(COMPONENTS main)

if(NOT DEFINED ENV{IDF_PATH})
    get_filename_component(local_idf "${native_bt_root}/../esp-idf" ABSOLUTE)
    set(ENV{IDF_PATH} "${local_idf}")
endif()
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(pure_sink)
