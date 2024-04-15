cmake_minimum_required(VERSION 3.9)

project(rf24c VERSION 1.0.0 DESCRIPTION "C wrapper for RF24.")

# detect the CPU make and type
include(./cmake/detectCPU.cmake) # sets the variable SOC accordingly

# auto-detect what driver to use
# auto-detect can be overriden using `cmake .. -D RF24_DRIVER=<supported driver>`
include(./cmake/AutoConfig_RF24_DRIVER.cmake)

find_library(RF24 rf24 REQUIRED)
message(STATUS "using RF24 library: ${RF24}")

# conditionally append "interruptConfigure" to the EXAMPLES_LIST
if("${RF24_DRIVER}" STREQUAL "MRAA" OR "${RF24_DRIVER}" STREQUAL "wiringPi" OR "${LibPIGPIO}" STREQUAL "LibPIGPIO-NOTFOUND")
    message(STATUS "Skipping interruptConfigure.cpp example as it is incompatible with selected driver library")
else() # not using MRAA or wiringPi drivers (or pigpio lib was found)
    list(APPEND EXAMPLES_LIST interruptConfigure)
endif()

  

add_library(rf24c SHARED rf24c.cpp)
target_link_libraries(rf24c PRIVATE ${RF24} pthread)
target_compile_definitions(rf24c PRIVATE RF24_NO_INTERRUPT)

set_target_properties(rf24c PROPERTIES VERSION ${PROJECT_VERSION})
set_target_properties(rf24c PROPERTIES PUBLIC_HEADER rf24c.h)

target_include_directories(rf24c PRIVATE . /usr/local/include/RF24)

include(GNUInstallDirs)

install(TARGETS rf24c
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    PUBLIC_HEADER DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
