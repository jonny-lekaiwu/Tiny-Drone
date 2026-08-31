# JPEG decoding is only used by the camera build. Keep it out of the MINI
# (ESP32-C3) build completely to save build and firmware resources.
if(IDF_TARGET STREQUAL "esp32c3")
    idf_component_register()
    return()
endif()

set(sources "jpeg_decoder.c")
set(includes "include")

# Compile only when cannot use ROM code
if(NOT CONFIG_JD_USE_ROM)
    list(APPEND sources "tjpgd/tjpgd.c")
    list(APPEND includes "tjpgd")
endif()

if(CONFIG_JD_DEFAULT_HUFFMAN)
    list(APPEND sources "jpeg_default_huffman_table.c")
endif()

idf_component_register(SRCS ${sources} INCLUDE_DIRS ${includes})
