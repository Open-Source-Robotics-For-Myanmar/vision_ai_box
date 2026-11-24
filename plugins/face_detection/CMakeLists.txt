find_package(OpenCV REQUIRED COMPONENTS core imgproc imgcodecs)
find_package(nlohmann_json REQUIRED)
find_path(ONNXRUNTIME_INCLUDE_DIR onnxruntime_cxx_api.h
    HINTS "${ONNXRUNTIME_ROOT}/include" PATH_SUFFIXES onnxruntime onnxruntime/core/session)
find_library(ONNXRUNTIME_LIBRARY NAMES onnxruntime
    HINTS "${ONNXRUNTIME_ROOT}/lib" "${ONNXRUNTIME_ROOT}/lib64")
if(NOT ONNXRUNTIME_INCLUDE_DIR OR NOT ONNXRUNTIME_LIBRARY)
    message(FATAL_ERROR "face_detection requires ONNX Runtime SDK; set -DONNXRUNTIME_ROOT=/path/to/onnxruntime")
endif()
add_library(face_detection SHARED RetinaFacePlugin.cpp)
target_include_directories(face_detection PRIVATE
    "${PROJECT_SOURCE_DIR}/core/include" "${PROJECT_SOURCE_DIR}/core/logging"
    "${ONNXRUNTIME_INCLUDE_DIR}" ${OpenCV_INCLUDE_DIRS})
target_link_libraries(face_detection PRIVATE ${OpenCV_LIBS}
    nlohmann_json::nlohmann_json "${ONNXRUNTIME_LIBRARY}")
target_compile_options(face_detection PRIVATE $<$<CXX_COMPILER_ID:GNU,Clang>:-Wall;-Wextra>)
set_target_properties(face_detection PROPERTIES PREFIX ""
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin/plugins")
add_executable(face_detection_smoke smoke.cpp "${PROJECT_SOURCE_DIR}/core/logging/Logger.cpp")
target_include_directories(face_detection_smoke PRIVATE
    "${PROJECT_SOURCE_DIR}/core/include" "${PROJECT_SOURCE_DIR}/core/logging" ${OpenCV_INCLUDE_DIRS})
target_link_libraries(face_detection_smoke PRIVATE face_detection ${OpenCV_LIBS}
    nlohmann_json::nlohmann_json)
set_target_properties(face_detection_smoke PROPERTIES ENABLE_EXPORTS ON
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
