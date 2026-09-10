#pragma once

#if defined(CAMERA_REALSENSE)
#include <librealsense2/rs.hpp>

struct CameraSettings {
    int color_width{640};
    int color_height{480};
    int color_fps{30};
    rs2_format color_format{RS2_FORMAT_BGR8};

    bool depth_enabled{false};
    int depth_width{640};
    int depth_height{480};
    int depth_fps{30};
    rs2_format depth_format{RS2_FORMAT_Z16};

    bool auto_exposure{true};
    float auto_exposure_val{1.0f};
    bool auto_white_balance{true};
    float auto_white_balance_val{1.0f};
};

#elif defined(CAMERA_USB)

struct CameraSettings {
    int color_width{640};
    int color_height{480};
    int color_fps{30};
    int usb_device_index{-1};
    bool auto_exposure{true};
    bool auto_white_balance{true};
};

#endif

