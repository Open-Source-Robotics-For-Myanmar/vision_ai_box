find_package(OpenCV REQUIRED)

add_library(face_recognition SHARED
    FaceRecognitionPlugin.cpp
)

target_include_directories(face_recognition PRIVATE
    ${PROJECT_SOURCE_DIR}/core/include
    ${PROJECT_SOURCE_DIR}/core/logging
)

target_link_libraries(face_recognition PRIVATE
    ${OpenCV_LIBS}
)

set_target_properties(face_recognition PROPERTIES
    PREFIX ""
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin/plugins"
)