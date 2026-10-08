# The following five lines of boilerplate have to be in your project's
# CMakeLists in this exact order for cmake to work correctly
cmake_minimum_required(VERSION 3.5)
  
set(__PROJECT_GROUP_LINK_COMPONENTS ON)

set(PLANE_COMPONENT_DIRS "./components/core"
                        "./components/drivers"
                        "./components/drivers/general"
                        "./components/drivers/i2c_devices"
                        "./components/drivers/spi_devices"
                         "./components/lib" ) 
 
list(APPEND EXTRA_COMPONENT_DIRS ${PLANE_COMPONENT_DIRS})

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
if(NOT IDF_TARGET STREQUAL "esp32s3" AND NOT IDF_TARGET STREQUAL "esp32c3")
    message(FATAL_ERROR "Tiny-Drone requires esp32s3 (Mini: esp32c3), not ${IDF_TARGET}. Use idf.py set-target esp32s3 or build.bat tiny-drone.")
endif()
project(Tiny-Drone)

# Always rebuild the complete flash image as part of `idf.py build`.  This is
# deliberately an ALL target (instead of an ELF POST_BUILD hook), so deleting
# only flash_all.bin still recreates it even when no source file changed.
add_custom_target(flash_all ALL
    COMMAND cmd.exe /d /c "${CMAKE_SOURCE_DIR}/combine.bat" "${CMAKE_BINARY_DIR}"
    DEPENDS gen_project_binary bootloader partition_table_bin
    COMMENT "Merging flash_all.bin"
    VERBATIM
    USES_TERMINAL
)
