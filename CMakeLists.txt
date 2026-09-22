cmake_minimum_required(VERSION 3.16)
project(vision_ai_box_workspace LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

option(VISION_OPTIMIZE_SIZE "Optimize the binary for size rather than speed" ON)

add_subdirectory(core)
add_subdirectory(plugins)