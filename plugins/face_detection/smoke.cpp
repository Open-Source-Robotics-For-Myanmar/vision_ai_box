#include "IPlugin.hpp"
#include <opencv2/imgcodecs.hpp>
#include <cmath>
#include <iostream>
#include <memory>
#include <stdexcept>
extern "C" IPlugin* create_plugin(Logger*);
extern "C" void destroy_plugin(IPlugin*);
int main(int argc, char** argv)
{
    if (argc != 3) {
        std::cerr << "Usage: face_detection_smoke model.onnx image.jpg\n";
        return 2;
    }
    try {
        std::unique_ptr<IPlugin, decltype(&destroy_plugin)> plugin(create_plugin(nullptr), destroy_plugin);
        if (!plugin) throw std::runtime_error("Plugin creation failed");
        plugin->init({});
        plugin->update_settings({{"model_path", argv[1]}});
        auto status = plugin->get_settings();
        if (!status.at("ready").get<bool>()) throw std::runtime_error(status.at("last_error").get<std::string>());
        FrameContext frame;
        frame.sequence = 7;
        frame.color = std::make_shared<cv::Mat>(cv::imread(argv[2]));
        if (frame.color->empty()) throw std::runtime_error("Cannot read image");
        auto result = plugin->process(frame);
        status = plugin->get_settings();
        if (!status.at("ready").get<bool>()) throw std::runtime_error(status.at("last_error").get<std::string>());
        for (const auto& box : result.detections) {
            if (!std::isfinite(box.x) || !std::isfinite(box.y) ||
                !std::isfinite(box.width) || !std::isfinite(box.height) ||
                box.x < 0 || box.y < 0 || box.width <= 0 || box.height <= 0 ||
                box.x + box.width > 1.00001f || box.y + box.height > 1.00001f ||
                box.confidence <= 0 || box.confidence > 1 || box.class_id != 0)
                throw std::runtime_error("Invalid normalized face detection");
        }
        plugin->set_enabled(false);
        if (!plugin->process(frame).detections.empty())
            throw std::runtime_error("Disabled plugin emitted detections");
        std::cout << nlohmann::json(result).dump(2) << '\n';
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
