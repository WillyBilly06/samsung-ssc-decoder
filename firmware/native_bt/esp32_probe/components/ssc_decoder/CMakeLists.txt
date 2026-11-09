# Samsung Scalable Codec (SSC) decoder: native codec-worker adapter + portable C99 decoder library.
# Tables are const (flash rodata); decoder state is allocated only while an SSC stream is open.
if(CMAKE_BUILD_EARLY_EXPANSION)
    idf_component_register(REQUIRES native_bt codec_core PRIV_REQUIRES heap)
    return()
endif()
if(NOT DBT_PROBE_SSC_BUILD)
    idf_component_register()
    return()
endif()
get_filename_component(workspace "${CMAKE_CURRENT_LIST_DIR}/../../../.." ABSOLUTE)
set(owned "${workspace}/native_bt/codecs/ssc")
# The decoder library lives at the repository root (decoder/), shared with the examples.
get_filename_component(decoder "${workspace}/../decoder" ABSOLUTE)
set(ssc_units
    "${decoder}/ssc_alloc.c"
    "${decoder}/ssc_bands.c"
    "${decoder}/ssc_cwrs.c"
    "${decoder}/ssc_decoder.c"
    "${decoder}/ssc_ec.c"
    "${decoder}/ssc_energy.c"
    "${decoder}/ssc_synth.c"
    "${decoder}/ssc_tables.c"
    "${decoder}/ssc_twiddle.c"
)
idf_component_register(SRCS "${owned}/dbt_ssc_codec.c" ${ssc_units}
    INCLUDE_DIRS "${owned}" "${decoder}"
    REQUIRES native_bt codec_core PRIV_REQUIRES heap)
target_compile_options(${COMPONENT_LIB} PRIVATE -std=gnu11 -O2 -fwrapv -fsingle-precision-constant -fno-math-errno)
set_source_files_properties(${ssc_units} PROPERTIES COMPILE_OPTIONS "-Wno-error")
