#pragma once

#include <cstdint>
#include <vector>

#include <opencv2/core.hpp>

struct AVCodecContext;
struct AVFormatContext;
struct AVStream;
struct AVFrame;
struct AVPacket;
struct SwsContext;
struct AVIOContext;

// Encodes BGR frames to H.264 and muxes them as fragmented MP4 for MSE.
// One instance belongs to one HTTP client. Changing output size requires a
// new instance and a new MediaSource on the browser.
class H264Stream
{
public:
    H264Stream() = default;
    ~H264Stream();

    H264Stream(const H264Stream&) = delete;
    H264Stream& operator=(const H264Stream&) = delete;

    bool open(int width, int height, int fps, int bitrate);
    void close();

    bool encode(const cv::Mat& bgr, std::vector<std::uint8_t>& fragment);

    const std::vector<std::uint8_t>& init_segment() const { return init_segment_; }
    int width() const { return width_; }
    int height() const { return height_; }
    int fps() const { return fps_; }

    static int even(int value);
    static int bitrate_for(int width, int height, int fps, int quality);

private:
    bool write_header();
    bool drain_packets(std::vector<std::uint8_t>& out);

    int width_{0};
    int height_{0};
    int fps_{0};
    std::int64_t pts_{0};

    AVCodecContext* codec_{nullptr};
    AVFormatContext* format_{nullptr};
    AVStream* stream_{nullptr};
    AVFrame* frame_{nullptr};
    AVPacket* packet_{nullptr};
    SwsContext* scaler_{nullptr};
    AVIOContext* io_{nullptr};
    std::vector<std::uint8_t> mux_buffer_;
    std::vector<std::uint8_t> init_segment_;
};
