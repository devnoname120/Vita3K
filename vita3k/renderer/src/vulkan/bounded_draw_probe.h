#pragma once

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <utility>

namespace renderer::vulkan::diagnostics {

class BoundedDrawProbe {
public:
    enum class Result { idle, recorded, completed, failed };
    using Clock = std::chrono::steady_clock;

    explicit BoundedDrawProbe(std::filesystem::path directory, unsigned limit = 512,
        bool start_on_next_frame = false)
        : directory_(std::move(directory))
        , limit_(limit)
        , start_on_next_frame_(start_on_next_frame) {}

    bool enabled() const { return !directory_.empty(); }

    template <class Writer>
    Result visit(uint64_t frame, Writer &&writer, Clock::time_point now = Clock::now()) {
        if (!enabled())
            return Result::idle;
        const std::lock_guard<std::mutex> lock(mutex_);
        if (finished_)
            return Result::idle;
        if (!active_) {
            if (armed_) {
                if (frame == armed_frame_)
                    return Result::idle;
                if (!start_capture(frame))
                    return fail();
            } else {
                if (polled_ && now - last_poll_ < std::chrono::milliseconds(500))
                    return Result::idle;
                last_poll_ = now;
                polled_ = true;
                std::error_code error;
                if (!std::filesystem::exists(directory_ / "request", error))
                    return error ? fail() : Result::idle;
                if (!limit_ || !directory_.is_absolute()
                    || std::filesystem::exists(directory_ / "capture.log", error)
                    || std::filesystem::exists(directory_ / "capture.log.part", error) || error)
                    return fail();
                if (start_on_next_frame_) {
                    armed_ = true;
                    armed_frame_ = frame;
                    return Result::idle;
                }
                if (!start_capture(frame))
                    return fail();
            }
        }
        if (frame != frame_) {
            if (start_on_next_frame_)
                return finish("frame_boundary");
            if (++frame_groups_ > 2)
                return finish("frame_boundary");
            frame_ = frame;
        }
        writer(output_, count_);
        if (!output_)
            return fail();
        if (++count_ >= limit_)
            return finish("draw_limit");
        return Result::recorded;
    }

private:
    bool start_capture(uint64_t frame) {
        output_.open(directory_ / "capture.log.part", std::ios::out | std::ios::binary);
        if (!output_)
            return false;
        output_ << "DRAW_CAPTURE_V1\n";
        if (!output_)
            return false;
        active_ = true;
        armed_ = false;
        frame_ = frame;
        frame_groups_ = 1;
        return true;
    }

    Result fail() {
        finished_ = true;
        output_.close();
        return Result::failed;
    }

    Result finish(const char *reason) {
        output_ << "END draws=" << count_ << " reason=" << reason << '\n';
        output_.flush();
        if (!output_)
            return fail();
        output_.close();
        if (!output_)
            return fail();
        std::error_code error;
        std::filesystem::rename(directory_ / "capture.log.part", directory_ / "capture.log", error);
        finished_ = true;
        return error ? Result::failed : Result::completed;
    }

    std::filesystem::path directory_;
    unsigned limit_;
    bool start_on_next_frame_;
    std::mutex mutex_;
    std::ofstream output_;
    Clock::time_point last_poll_{};
    uint64_t frame_ = 0;
    unsigned count_ = 0;
    unsigned frame_groups_ = 0;
    bool polled_ = false;
    bool armed_ = false;
    bool active_ = false;
    bool finished_ = false;
    uint64_t armed_frame_ = 0;
};

}
