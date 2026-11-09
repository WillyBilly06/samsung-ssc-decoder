# SSC playback example: decodes SSC streams stored in flash and plays them through an I2S DAC (PCM5102A).
cmake_minimum_required(VERSION 3.16)

# The decoder is an ESP-IDF component: point EXTRA_COMPONENT_DIRS at the repository's ssc_decoder/ folder.
# (In your own project you can instead add it with the component manager, see docs/integration.md.)
set(EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_LIST_DIR}/../../ssc_decoder")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(ssc_playback)
