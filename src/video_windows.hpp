#pragma once

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <optional>

namespace nso {

// Video editing with Windows' built-in Media Foundation codecs. Everything
// runs on this PC; the original file is never changed.

struct VideoInfo {
    double seconds = 0;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
};

std::optional<VideoInfo> probe_video(const std::filesystem::path& video);

// Saves [start, end) seconds of `video` as an H.264/AAC MP4, keeping its sound.
void export_video_clip(const std::filesystem::path& video, const std::filesystem::path& output,
                       double start, double end, const std::atomic<bool>& cancelled);

// Saves [start, end) seconds of `video` as a looping animated GIF, scaled to
// `width` pixels wide at `fps` frames per second.
void export_video_gif(const std::filesystem::path& video, const std::filesystem::path& output,
                      double start, double end, std::uint32_t width, double fps, const std::atomic<bool>& cancelled);

}  // namespace nso
