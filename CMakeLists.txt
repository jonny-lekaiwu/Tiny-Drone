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
