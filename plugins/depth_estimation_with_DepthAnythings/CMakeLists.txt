find_package(OpenCV REQUIRED)

add_library(depth_estimation_plugin SHARED
    DepthEstimationPlugin.cpp
)

target_link_libraries(face_recognition_plugin PRIVATE
    core_sdk
    ${OpenCV_LIBS}
)

# Shared Library Prefix ("lib") ကို ဖြုတ်ပြီး clean name ရရှိစေရန်
set_target_properties(face_recognition_plugin PROPERTIES
    PREFIX ""
)