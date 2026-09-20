#pragma once

#include <string>
#include <vector>

#if defined(CAMERA_REALSENSE)
#include <librealsense2/rs.hpp>

struct ResolutionOption {
    int width{0};
    int height{0};
    std::string label{};
};

struct AvailableCameraSettings {
    static const std::vector<ResolutionOption>& color_resolutions()
    {
        static const std::vector<ResolutionOption> options = {
            {640, 480, "640x480"},
            {1280, 720, "1280x720"},
            {1920, 1080, "1920x1080"}
        };
        return options;
    }

    static const std::vector<int>& color_fps_options()
    {
        static const std::vector<int> options = {30, 15};
        return options;
    }

    static const std::vector<ResolutionOption>& depth_resolutions()
    {
        static const std::vector<ResolutionOption> options = {
            {424, 240, "424x240"},
            {640, 480, "640x480"},
            {1280, 720, "1280x720"}
        };
        return options;
    }

    static const std::vector<int>& depth_fps_options()
    {
        static const std::vector<int> options = {30, 15};
        return options;
    }
};

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
    bool auto_white_balance{true};
};

#elif defined(CAMERA_USB)

struct ResolutionOption {
    int width{0};
    int height{0};
    std::string label{};
};

struct AvailableCameraSettings {
    static const std::vector<ResolutionOption>& color_resolutions()
    {
        static const std::vector<ResolutionOption> options = {
            {640, 480, "640x480"},
            {1280, 720, "1280x720"},
            {1920, 1080, "1920x1080"},
            {3840, 2160, "3840x2160"}
        };
        return options;
    }

    static const std::vector<int>& color_fps_options()
    {
        static const std::vector<int> options = {30, 15};
        return options;
    }
};

struct CameraSettings {
    int color_width{640};
    int color_height{480};
    int color_fps{30};
    int usb_device_index{-1};
    bool auto_exposure{true};
    bool auto_white_balance{true};
};

#endif

