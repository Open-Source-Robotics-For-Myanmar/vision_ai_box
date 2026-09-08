#include "Toggles.hpp"
#include "Logger.hpp"
#include "WebServer.hpp"

#ifdef CAMERA_USB
#include "UsbCamera.hpp"
#elif defined(CAMERA_REALSENSE)
#include "RealSenseCamera.hpp"
#endif

#include <chrono>
#include <csignal>
#include <cstdlib>
#include <stdexcept>
#include <thread>

namespace
{
ServiceToggles* active_toggles = nullptr;

void handle_signal(int)
{
    if (active_toggles) active_toggles->running = false;
}
}

int main()
{
    ServiceToggles toggles;
    Logger logger;
    active_toggles = &toggles;
    std::signal(SIGINT, handle_signal);
    std::signal(SIGTERM, handle_signal);

#ifdef CAMERA_USB
    UsbCamera camera(logger);

#elif defined(CAMERA_REALSENSE)
    RealSenseCamera camera(logger);

#else

    logger.log(LogLevel::ERROR, "SYSTEM", "No camera backend was selected");
    return 1;

#endif

    logger.log(LogLevel::INFO, "SYSTEM", "vision_ai_box starting up");

    if (!camera.start()) {
        logger.log(
            LogLevel::ERROR,
            "CAMERA",
            "Camera initialization failed"
        );

        return 1;
    }

    toggles.processing_enabled = true;
    camera.set_processing_enabled(true);

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
    while (toggles.running.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
    }

    web_server.stop();
    camera.set_processing_enabled(false);
    camera.stop();
    active_toggles = nullptr;
    logger.log(LogLevel::INFO, "SYSTEM", "vision_ai_box stopped cleanly");

    return 0;
}