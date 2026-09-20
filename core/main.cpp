#include "CameraSettings.hpp"
#include "Toggles.hpp"
#include "Logger.hpp"
#include "WebServer.hpp"
#include "PluginManager.hpp"

#ifdef CAMERA_USB
#include "UsbCamera.hpp"
#elif defined(CAMERA_REALSENSE)
#include "RealSenseCamera.hpp"
#endif

#include <chrono>
#include <csignal>
#include <cstdlib>
#include <stdexcept>
#include <iostream>
#include <thread>
#include <filesystem>


namespace
{
ServiceToggles* active_toggles = nullptr;

void handle_signal(int)
{
    if (active_toggles) {
        active_toggles->request_shutdown();
    }
}


void camera_reconnect_loop(Logger& logger, SelectedCamera& camera, ServiceToggles& toggles)
{
    while (toggles.running.load(std::memory_order_acquire)) {
        const bool enabled = toggles.camera_enabled.load(std::memory_order_acquire);
        const bool connected = camera.check_device_state();
        const bool running = camera.is_running();

        if (!enabled) {
            if (running) {
                logger.log(LogLevel::INFO, "CAMERA", "Camera turned off");
                camera.stop();
            }
            toggles.camera_error = false;
            toggles.processing_enabled = false;
            camera.set_processing_enabled(false);
            std::this_thread::sleep_for(std::chrono::milliseconds(250));
            continue;
        }

        if (running && !connected) {
            logger.log(LogLevel::WARN, "CAMERA", "Camera Unplugged");
            camera.stop();
            toggles.camera_error = true;
            toggles.processing_enabled = false;
            camera.set_processing_enabled(false);
            std::this_thread::sleep_for(std::chrono::seconds(2));
            continue;
        }

        if (!running && connected) {
            toggles.camera_error = false;
            if (camera.start()) {
                toggles.processing_enabled = true;
                camera.set_processing_enabled(true);
                logger.log(LogLevel::INFO, "CAMERA", "Camera Connected");
            } else {
                toggles.camera_error = true;
                toggles.processing_enabled = false;
                camera.set_processing_enabled(false);
                logger.log(LogLevel::WARN, "CAMERA", "Camera Unplugged");
            }
            std::this_thread::sleep_for(std::chrono::seconds(2));
            continue;
        }

        if (!connected) {
            toggles.camera_error = true;
            toggles.processing_enabled = false;
            camera.set_processing_enabled(false);
            logger.log(LogLevel::WARN, "CAMERA", "Camera Unplugged");
            std::this_thread::sleep_for(std::chrono::seconds(2));
            continue;
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }
}
}

int main()
{
    ServiceToggles toggles;
    toggles.camera_enabled = true;
    Logger logger;
    active_toggles = &toggles;
    std::signal(SIGINT, handle_signal);
    std::signal(SIGTERM, handle_signal);

    CameraSettings settings;
#ifdef CAMERA_REALSENSE
    settings.depth_enabled = true;
#endif
    settings.auto_exposure = true;
    settings.auto_white_balance = true;

#ifdef CAMERA_USB
    UsbCamera camera(logger, settings);
#elif defined(CAMERA_REALSENSE)
    RealSenseCamera camera(logger, settings);
#else
    logger.log(LogLevel::ERROR, "SYSTEM", "No camera backend was selected");
    return 1;
#endif

    logger.log(LogLevel::INFO, "SYSTEM", "vision_ai_box starting up");

    // Plugins Loading
    PluginManager plugin_manager(logger);
    const std::filesystem::path plugin_dir = "bin/plugins";
    if (std::filesystem::exists(plugin_dir)) {
        plugin_manager.load_all_plugins(plugin_dir);
    } else {
        logger.log(LogLevel::WARN, "PLUGIN_MGR", "Plugin directory not found: " + plugin_dir.string());
    }

    camera.register_frame_callback([&plugin_manager](const FrameContext& frame) {
        plugin_manager.process_frame(frame);
    });

    // Start the web server
    std::uint16_t web_port = 8080;
    if (const char* configured_port = std::getenv("VISION_AI_BOX_PORT")) {
        try {
            const unsigned long parsed_port = std::stoul(configured_port);
            if (parsed_port > 0 && parsed_port <= 65535) web_port = static_cast<std::uint16_t>(parsed_port);
        } catch (const std::exception&) {
            logger.log(LogLevel::WARN, "WEB_SERVER", "Invalid VISION_AI_BOX_PORT; using 8080");
        }
    }

    WebServer web_server(logger, camera, toggles);
    if (!web_server.start(web_port)) {
        logger.log(LogLevel::ERROR, "WEB_SERVER", "Web server failed to start");
        camera.stop();
        return 1;
    }

    logger.log(LogLevel::INFO, "SYSTEM", "vision_ai_box is ready at http://<device-ip>:" + std::to_string(web_port));

    // Start the camera and monitor its state
    if (camera.check_device_state()) {
        if (camera.start()) {
            toggles.camera_error = false;
            toggles.processing_enabled = true;
            camera.set_processing_enabled(true);
            logger.log(LogLevel::INFO, "CAMERA", "Camera Connected");
        } else {
            toggles.camera_error = true;
            toggles.processing_enabled = false;
            camera.set_processing_enabled(false);
            logger.log(LogLevel::WARN, "CAMERA", "Camera Unplugged");
        }
    } else {
        toggles.camera_error = true;
        toggles.processing_enabled = false;
        camera.set_processing_enabled(false);
        logger.log(LogLevel::WARN, "CAMERA", "Camera Unplugged");
    }

    std::thread camera_monitor_thread(camera_reconnect_loop, std::ref(logger), std::ref(camera), std::ref(toggles));

    while (toggles.running.load(std::memory_order_acquire)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
    }

    toggles.request_stop_camera();
    toggles.running.store(false, std::memory_order_release);
    toggles.camera_error.store(false, std::memory_order_release);

    web_server.stop();
    camera.set_processing_enabled(false);
    camera.stop();
    if (camera_monitor_thread.joinable()) camera_monitor_thread.join();
    active_toggles = nullptr;
    logger.log(LogLevel::INFO, "SYSTEM", "vision_ai_box stopped cleanly");

    return 0;
}
