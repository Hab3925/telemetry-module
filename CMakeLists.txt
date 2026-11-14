cmake_minimum_required(VERSION 3.22)

#
# This file is generated only once,
# and is not re-generated if converter is called multiple times.
#
# User is free to modify the file as much as necessary
#

# Setup compiler settings
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS ON)


# Define the build type
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE "Debug")
endif()

# Set the project name
set(CMAKE_PROJECT_NAME telemetry)

# Enable compile command to ease indexing with e.g. clangd
set(CMAKE_EXPORT_COMPILE_COMMANDS TRUE)

# Core project settings
project(${CMAKE_PROJECT_NAME})
message("Build type: " ${CMAKE_BUILD_TYPE})

# Enable CMake support for ASM and C languages
enable_language(C ASM)

# Create an executable object type
add_executable(${CMAKE_PROJECT_NAME})

# Add STM32CubeMX generated sources
add_subdirectory(cmake/stm32cubemx)

# Link directories setup
target_link_directories(${CMAKE_PROJECT_NAME} PRIVATE
    # Add user defined library search paths
)

# Link role of this board: MASTER owns the hop clock, SLAVE follows it
set(TELEMETRY_ROLE "MASTER" CACHE STRING "Radio link role of this board (MASTER or SLAVE)")
set_property(CACHE TELEMETRY_ROLE PROPERTY STRINGS MASTER SLAVE)
if(TELEMETRY_ROLE STREQUAL "MASTER")
    set(APP_ROLE_MASTER 1)
elseif(TELEMETRY_ROLE STREQUAL "SLAVE")
    set(APP_ROLE_MASTER 0)
else()
    message(FATAL_ERROR "TELEMETRY_ROLE must be MASTER or SLAVE")
endif()
message("Telemetry role: " ${TELEMETRY_ROLE})

file(GLOB APPLICATION_SOURCES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/application/src/*.c)
set_source_files_properties(${APPLICATION_SOURCES} PROPERTIES COMPILE_OPTIONS "-Wall;-Wextra")

# Add sources to executable
target_sources(${CMAKE_PROJECT_NAME} PRIVATE
    # Add user sources here
    ${APPLICATION_SOURCES}
)

# Add include paths
target_include_directories(${CMAKE_PROJECT_NAME} PRIVATE
    # Add user defined include paths
    ${CMAKE_CURRENT_SOURCE_DIR}/application/inc
)

# Add project symbols (macros)
target_compile_definitions(${CMAKE_PROJECT_NAME} PRIVATE
    # Add user defined symbols
    APP_ROLE_MASTER=${APP_ROLE_MASTER}
)

# Remove wrong libob.a library dependency when using cpp files
list(REMOVE_ITEM CMAKE_C_IMPLICIT_LINK_LIBRARIES ob)

# Add linked libraries
target_link_libraries(${CMAKE_PROJECT_NAME}
    stm32cubemx

    # Add user defined libraries
    m
)
