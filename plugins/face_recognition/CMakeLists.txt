find_package(OpenCV REQUIRED COMPONENTS core)

add_library(face_recognition SHARED
    FaceRecognitionPlugin.cpp
)

target_include_directories(face_recognition PRIVATE
    ${PROJECT_SOURCE_DIR}/core/include
    ${PROJECT_SOURCE_DIR}/core/logging
)

# Only cv::Mat is touched, and only through inline accessors on FrameContext.
target_link_libraries(face_recognition PRIVATE
    opencv_core
)

if(VISION_OPTIMIZE_SIZE)
    target_compile_options(face_recognition PRIVATE
        $<$<CXX_COMPILER_ID:GNU,Clang>:-Os -ffunction-sections -fdata-sections>
    )
    target_link_options(face_recognition PRIVATE
        $<$<CXX_COMPILER_ID:GNU,Clang>:-Wl,--gc-sections -Wl,--as-needed>
    )
endif()

set_target_properties(face_recognition PROPERTIES
    PREFIX ""
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin/plugins"
)