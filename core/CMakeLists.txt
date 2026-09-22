cmake_minimum_required(VERSION 3.16)

project(vision_ai_box LANGUAGES CXX)

# ============================================================
# Basic Configuration
# ============================================================

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

# ============================================================
# Project & Thirdparty Paths
# ============================================================

set(PROJECT_ROOT ${CMAKE_CURRENT_SOURCE_DIR})
set(INCLUDE_DIR ${PROJECT_ROOT}/include)
set(THIRDPARTY_ROOT ${PROJECT_ROOT}/thirdparty_libraries)

# External Library Root Paths (Configurable via Cache)
set(LIBREALSENSE_DIR "${PROJECT_ROOT}/../../librealsense" CACHE PATH "Path to librealsense root")


# ============================================================
# Flexible Build Options (Cache Variables)
# ============================================================

# 1. Camera Backend Option
set(CAMERA "USB" CACHE STRING "Camera backend selection")
set_property(CACHE CAMERA PROPERTY STRINGS USB REALSENSE)
string(TOUPPER "${CAMERA}" CAMERA)


message(STATUS "====================================")
message(STATUS " AI Vision Box Configuration        ")
message(STATUS "====================================")
message(STATUS "Camera    : ${CAMERA}")
message(STATUS "====================================")


# ============================================================
# Dependencies Finding
# ============================================================

find_package(OpenCV REQUIRED COMPONENTS
    core
    imgproc
    imgcodecs
    dnn
    calib3d
    videoio
)

find_package(nlohmann_json REQUIRED)
find_package(Threads REQUIRED)
find_package(PkgConfig REQUIRED)

pkg_check_modules(FFMPEG REQUIRED IMPORTED_TARGET
    libavcodec
    libavutil
    libswscale
)




# ============================================================
# Camera Backend Configuration
# ============================================================

if(CAMERA STREQUAL "USB")
    message(STATUS "Configuring USB Camera Backend...")
    set(CAMERA_SOURCES camera/UsbCamera.cpp)
    set(CAMERA_DEFINITION CAMERA_USB)
elseif(CAMERA STREQUAL "REALSENSE")
    message(STATUS "Configuring RealSense Camera Backend...")
    
    set(LIBREALSENSE_LIBRARY
        ${LIBREALSENSE_DIR}/build/Release/librealsense2.so.2.58.4
    )
    if(NOT EXISTS "${LIBREALSENSE_LIBRARY}")
        message(FATAL_ERROR "librealsense not found at: ${LIBREALSENSE_LIBRARY}")
    endif()

    add_library(realsense2::realsense2 SHARED IMPORTED)
    set_target_properties(realsense2::realsense2 PROPERTIES
        IMPORTED_LOCATION ${LIBREALSENSE_LIBRARY}
        INTERFACE_INCLUDE_DIRECTORIES ${LIBREALSENSE_DIR}/include
    )
    set(CAMERA_SOURCES camera/RealSenseCamera.cpp)
    set(CAMERA_DEFINITION CAMERA_REALSENSE)
else()
    message(FATAL_ERROR "Unknown CAMERA option: ${CAMERA}")
endif()


# ============================================================
# Application Sources
# ============================================================

set(VISION_AI_BOX_SOURCES
    main.cpp
    logging/Logger.cpp
    base_system/Recording.cpp
    web_server/WebServer.cpp
    plugin_manager/PluginManager.cpp
)
list(APPEND VISION_AI_BOX_SOURCES ${CAMERA_SOURCES})


# ============================================================
# Executable & Include Directories
# ============================================================

add_executable(vision_ai_box ${VISION_AI_BOX_SOURCES})
target_compile_definitions(vision_ai_box PRIVATE ${CAMERA_DEFINITION})

set_target_properties(vision_ai_box PROPERTIES ENABLE_EXPORTS ON) ## Export Permit For Plugins

target_include_directories(vision_ai_box
    PRIVATE
    ${INCLUDE_DIR}
    ${PROJECT_ROOT}
    ${PROJECT_ROOT}/base_system
    ${PROJECT_ROOT}/camera
    ${PROJECT_ROOT}/logging
    ${PROJECT_ROOT}/web_server
    ${PROJECT_ROOT}/plugin_manager 
)

target_compile_definitions(vision_ai_box PRIVATE
    VISION_WEB_ROOT="${PROJECT_ROOT}/web_server"
)


# ============================================================
# Linking Libraries
# ============================================================

target_link_libraries(vision_ai_box
    PRIVATE
    ${OpenCV_LIBS}
    nlohmann_json::nlohmann_json
    PkgConfig::FFMPEG
    Threads::Threads
    ${CMAKE_DL_LIBS}    # <-- Dynamic Loader Library (libdl) Link လုပ်ထားသည်
)

# Conditional Linking based on Selection
if(CAMERA STREQUAL "REALSENSE")
    target_link_libraries(vision_ai_box PRIVATE realsense2::realsense2)
endif()


# ============================================================
# Compiler Warnings & Optimization
# ============================================================

target_compile_options(vision_ai_box
    PRIVATE
    $<$<CXX_COMPILER_ID:GNU,Clang>:-Wall -Wextra>
)