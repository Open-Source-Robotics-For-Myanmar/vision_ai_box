#include "H264Stream.hpp"

#include <algorithm>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
#include <libswscale/swscale.h>
}

namespace
{
struct MemoryIO
{
    std::vector<std::uint8_t>* buffer{nullptr};
    std::size_t position{0};
};

int io_write(void* opaque, const std::uint8_t* data, int size)
{
    auto* io = static_cast<MemoryIO*>(opaque);
    if (!io || !io->buffer || size <= 0) {
        return 0;
    }

    const std::size_t needed = io->position + static_cast<std::size_t>(size);
    if (needed > io->buffer->size()) {
        io->buffer->resize(needed);
    }
    std::copy(data, data + size, io->buffer->begin() + static_cast<std::ptrdiff_t>(io->position));
    io->position += static_cast<std::size_t>(size);
    return size;
}

#if LIBAVFORMAT_VERSION_MAJOR < 61
int io_write_legacy(void* opaque, std::uint8_t* data, int size)
{
    return io_write(opaque, data, size);
}
#endif

std::int64_t io_seek(void* opaque, std::int64_t offset, int whence)
{
    auto* io = static_cast<MemoryIO*>(opaque);
    if (!io || !io->buffer) {
        return -1;
    }

    if (whence == AVSEEK_SIZE) {
        return static_cast<std::int64_t>(io->buffer->size());
    }

    std::int64_t next = 0;
    if (whence == SEEK_SET) {
        next = offset;
    } else if (whence == SEEK_CUR) {
        next = static_cast<std::int64_t>(io->position) + offset;
    } else if (whence == SEEK_END) {
        next = static_cast<std::int64_t>(io->buffer->size()) + offset;
    } else {
        return -1;
    }

    if (next < 0) {
        return -1;
    }
    io->position = static_cast<std::size_t>(next);
    return next;
}

MemoryIO* io_from_context(AVIOContext* context)
{
    return context ? static_cast<MemoryIO*>(context->opaque) : nullptr;
}
}

int H264Stream::even(int value)
{
    return std::max(2, value & ~1);
}

int H264Stream::bitrate_for(int width, int height, int fps, int quality)
{
    const int clamped = std::clamp(quality, 30, 90);
    const int pixels = std::max(1, width * height);
    return std::max(80'000, pixels * std::max(1, fps) * clamped / 900);
}

H264Stream::~H264Stream()
{
    close();
}

void H264Stream::close()
{
    if (format_ && format_->pb) {
        av_write_trailer(format_);
    }
    if (scaler_) {
        sws_freeContext(scaler_);
        scaler_ = nullptr;
    }
    if (frame_) {
        av_frame_free(&frame_);
    }
    if (packet_) {
        av_packet_free(&packet_);
    }
    if (codec_) {
        avcodec_free_context(&codec_);
    }
    if (format_) {
        if (format_->pb) {
            auto* io = io_from_context(format_->pb);
            avio_context_free(&format_->pb);
            delete io;
        }
        avformat_free_context(format_);
        format_ = nullptr;
        stream_ = nullptr;
    }
    io_ = nullptr;
    pts_ = 0;
}

bool H264Stream::write_header()
{
    AVDictionary* options = nullptr;
    av_dict_set(&options, "movflags", "frag_every_frame+empty_moov+default_base_moof+omit_tfhd_offset", 0);
    const int written = avformat_write_header(format_, &options);
    av_dict_free(&options);
    if (written < 0) {
        return false;
    }
    avio_flush(format_->pb);
    auto* io = io_from_context(format_->pb);
    if (!io || !io->buffer) {
        return false;
    }
    init_segment_ = *io->buffer;
    io->buffer->clear();
    io->position = 0;
    return !init_segment_.empty();
}

bool H264Stream::open(int width, int height, int fps, int bitrate)
{
    close();

    width_ = even(width);
    height_ = even(height);
    fps_ = std::clamp(fps, 1, 60);
    mux_buffer_.clear();
    init_segment_.clear();

    const AVCodec* codec = avcodec_find_encoder(AV_CODEC_ID_H264);
    if (!codec) {
        return false;
    }

    codec_ = avcodec_alloc_context3(codec);
    packet_ = av_packet_alloc();
    frame_ = av_frame_alloc();
    if (!codec_ || !packet_ || !frame_) {
        close();
        return false;
    }

    codec_->codec_id = AV_CODEC_ID_H264;
    codec_->codec_type = AVMEDIA_TYPE_VIDEO;
    codec_->width = width_;
    codec_->height = height_;
    codec_->pix_fmt = AV_PIX_FMT_YUV420P;
    codec_->time_base = AVRational{1, fps_};
    codec_->framerate = AVRational{fps_, 1};
    codec_->gop_size = fps_;
    codec_->max_b_frames = 0;
    codec_->bit_rate = std::max(80'000, bitrate);
    codec_->rc_min_rate = codec_->bit_rate / 2;
    codec_->rc_max_rate = codec_->bit_rate * 2;
    codec_->rc_buffer_size = codec_->bit_rate;
    codec_->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
    codec_->profile = FF_PROFILE_H264_BASELINE;
    codec_->level = 40;
    av_opt_set(codec_->priv_data, "preset", "ultrafast", 0);
    av_opt_set(codec_->priv_data, "tune", "zerolatency", 0);
    av_opt_set(codec_->priv_data, "profile", "baseline", 0);
    av_opt_set(codec_->priv_data, "level", "4.0", 0);

    if (avcodec_open2(codec_, codec, nullptr) < 0) {
        close();
        return false;
    }

    frame_->format = codec_->pix_fmt;
    frame_->width = codec_->width;
    frame_->height = codec_->height;
    if (av_frame_get_buffer(frame_, 32) < 0) {
        close();
        return false;
    }

    scaler_ = sws_getContext(width_, height_, AV_PIX_FMT_BGR24,
                             width_, height_, AV_PIX_FMT_YUV420P,
                             SWS_FAST_BILINEAR, nullptr, nullptr, nullptr);
    if (!scaler_) {
        close();
        return false;
    }

    if (avformat_alloc_output_context2(&format_, nullptr, "mp4", nullptr) < 0 || !format_) {
        close();
        return false;
    }

    stream_ = avformat_new_stream(format_, nullptr);
    if (!stream_ || avcodec_parameters_from_context(stream_->codecpar, codec_) < 0) {
        close();
        return false;
    }
    stream_->time_base = codec_->time_base;
    stream_->avg_frame_rate = codec_->framerate;

    constexpr int io_buffer_size = 32 * 1024;
    auto* avio_buffer = static_cast<unsigned char*>(av_malloc(io_buffer_size));
    auto* memory = new MemoryIO{&mux_buffer_, 0};
#if LIBAVFORMAT_VERSION_MAJOR >= 61
    io_ = avio_alloc_context(avio_buffer, io_buffer_size, 1, memory, nullptr, io_write, io_seek);
#else
    io_ = avio_alloc_context(avio_buffer, io_buffer_size, 1, memory, nullptr, io_write_legacy, io_seek);
#endif
    if (!io_) {
        av_free(avio_buffer);
        delete memory;
        close();
        return false;
    }

    format_->pb = io_;
    format_->flags |= AVFMT_FLAG_CUSTOM_IO;
    if (!write_header()) {
        close();
        return false;
    }
    return true;
}

bool H264Stream::drain_packets(std::vector<std::uint8_t>& out)
{
    while (true) {
        const int received = avcodec_receive_packet(codec_, packet_);
        if (received == AVERROR(EAGAIN) || received == AVERROR_EOF) {
            break;
        }
        if (received < 0) {
            return false;
        }

        av_packet_rescale_ts(packet_, codec_->time_base, stream_->time_base);
        packet_->stream_index = stream_->index;
        if (av_interleaved_write_frame(format_, packet_) < 0) {
            av_packet_unref(packet_);
            return false;
        }
        av_packet_unref(packet_);
    }

    avio_flush(format_->pb);
    auto* io = io_from_context(format_->pb);
    if (!io || !io->buffer) {
        return false;
    }
    out.swap(*io->buffer);
    io->buffer->clear();
    io->position = 0;
    return true;
}

bool H264Stream::encode(const cv::Mat& bgr, std::vector<std::uint8_t>& fragment)
{
    fragment.clear();
    if (!codec_ || !frame_ || !scaler_ || bgr.empty()) {
        return false;
    }

    if (bgr.cols != width_ || bgr.rows != height_) {
        return false;
    }
    const cv::Mat packed = bgr.isContinuous() ? bgr : bgr.clone();

    const uint8_t* src_slices[] = { packed.data };
    const int src_stride[] = { static_cast<int>(packed.step) };
    sws_scale(scaler_, src_slices, src_stride, 0, height_, frame_->data, frame_->linesize);

    frame_->pts = pts_++;
    if (avcodec_send_frame(codec_, frame_) < 0) {
        return false;
    }
    return drain_packets(fragment);
}
