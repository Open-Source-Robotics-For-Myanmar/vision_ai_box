#include "FramePreprocessor.hpp"
#include "FrameQueue.hpp"

#include <chrono>
#include <future>
#include <iostream>
#include <stdexcept>

using namespace std::chrono_literals;

void check(bool value, const char* message)
{
    if (!value) throw std::runtime_error(message);
}

FrameContext frame(std::uint64_t sequence)
{
    return {sequence, std::make_shared<cv::Mat>(720, 1280, CV_8UC3, cv::Scalar(10, 20, 30)),
            std::make_shared<cv::Mat>(720, 1280, CV_16UC1, cv::Scalar(1234))};
}

int main()
{
    try {
        bool rejected = false;
        try { FrameQueue invalid(0); } catch (const std::invalid_argument&) { rejected = true; }
        check(rejected, "zero-capacity queue must be rejected");

        FrameQueue queue(1);
        check(!queue.push({}), "empty frame must be rejected");
        check(queue.push(frame(1)), "first frame accepted");
        check(!queue.push(frame(2)), "best effort reports eviction");
        FrameContext received;
        check(queue.pop(received) && received.sequence == 2, "best effort keeps newest frame");

        queue.configure(1, FrameDelivery::Reliable);
        check(queue.push(frame(3)), "reliable first frame accepted");
        auto pending = std::async(std::launch::async, [&] { return queue.push(frame(4)); });
        const auto before = pending.wait_for(50ms);
        const bool first = queue.pop(received) && received.sequence == 3;
        const auto after = pending.wait_for(2s);
        if (after != std::future_status::ready) queue.close();
        check(first && before == std::future_status::timeout && after == std::future_status::ready,
              "reliable delivery must wait for capacity without replacing queued frame");
        check(pending.get(), "waiting producer accepted");
        check(queue.pop(received) && received.sequence == 4, "reliable FIFO ordering");

        queue.push(frame(5));
        auto closing = std::async(std::launch::async, [&] { return queue.push(frame(6)); });
        queue.close();
        check(closing.wait_for(2s) == std::future_status::ready && !closing.get(),
              "close must release blocked producers");
        check(!queue.pop(received), "close discards pending frames");

        FrameQueue changing(1);
        changing.configure(1, FrameDelivery::Reliable);
        changing.push(frame(7));
        auto waiting = std::async(std::launch::async, [&] { return changing.push(frame(8)); });
        changing.configure(1, FrameDelivery::BestEffort);
        const auto changed = waiting.wait_for(2s);
        if (changed != std::future_status::ready) changing.close();
        check(changed == std::future_status::ready && waiting.get(), "configuration releases producers");

        PipelineSettings settings;
        const auto raw = frame(42);
        auto bypass = preprocess_frame(raw, settings);
        bypass.color->setTo(cv::Scalar(0, 0, 0));
        bypass.depth->setTo(0);
        check(raw.color->at<cv::Vec3b>(0, 0)[0] == 10 && raw.depth->at<unsigned short>(0, 0) == 1234,
              "model buffers must not modify raw frame");
        settings = settings.updated({{"enabled", true}, {"quality", "low"}});
        const auto scaled = preprocess_frame(raw, settings);
        check(scaled.sequence == 42 && scaled.color->size() == cv::Size(640, 360),
              "downscale preserves aspect ratio and sequence");
        check(scaled.depth->size() == scaled.color->size() && scaled.depth->type() == CV_16UC1 &&
              scaled.depth->at<unsigned short>(0, 0) == 1234, "depth preserves units and alignment");
        settings.mode = "filter";
        const auto filtered = preprocess_frame(raw, settings);
        check(filtered.color->size() == scaled.color->size(), "filter supports downscaled frames");
        settings.quality = "high";
        check(preprocess_frame(raw, settings).color->size() == raw.color->size(), "high preserves size");
        auto impulse = frame(43);
        impulse.color->setTo(cv::Scalar(0, 0, 0));
        impulse.color->at<cv::Vec3b>(100, 100) = cv::Vec3b(255, 255, 255);
        const auto blurred = preprocess_frame(impulse, settings);
        check(blurred.color->at<cv::Vec3b>(100, 100)[0] < 255 &&
              blurred.color->at<cv::Vec3b>(100, 99)[0] > 0 &&
              impulse.color->at<cv::Vec3b>(100, 99)[0] == 0,
              "filter blurs the model copy without altering raw pixels");
        settings.quality = "low";
        FrameContext small{44, std::make_shared<cv::Mat>(10, 20, CV_8UC3, cv::Scalar(0, 0, 0)), {}};
        check(preprocess_frame(small, settings).color->size() == small.color->size(), "never upscale");
        check(!preprocess_frame({}, settings).color, "empty preprocessing input is safe");

        for (const auto& patch : {nlohmann::json{{"queue_capacity", 0}},
                                 nlohmann::json{{"queue_capacity", 1.5}},
                                 nlohmann::json{{"delivery", "unknown"}},
                                 nlohmann::json{{"mode", "compressed"}},
                                 nlohmann::json{{"quality", "unknown"}},
                                 nlohmann::json{{"enabled", 1}},
                                 nlohmann::json{{"typo", true}}}) {
            rejected = false;
            try { settings.updated(patch); } catch (const std::exception&) { rejected = true; }
            check(rejected, "invalid configuration must be rejected");
        }
        std::cout << "Pipeline tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
