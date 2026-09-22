find_package(OpenCV REQUIRED)

add_library(example_plugin SHARED
    examplePlugin.cpp
)

target_include_directories(example_plugin PRIVATE
    ${PROJECT_SOURCE_DIR}/core/include
    ${PROJECT_SOURCE_DIR}/core/logging
)

target_link_libraries(example_plugin PRIVATE
    ${OpenCV_LIBS}
)

set_target_properties(example_plugin PROPERTIES
    PREFIX ""
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin/plugins"
)
