#pragma once

#include "nso_album_sync/windows_compat.hpp"

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace nso {

// Reads the text in screenshots with Windows' built-in text recognition
// (Windows.Media.Ocr), which runs entirely on this PC. Results are cached in
// a file so each screenshot is only read once.
class ScreenshotText {
public:
    struct Item {
        std::string key;  // Album-relative path, as used for labels
        std::filesystem::path file;
        std::int64_t modified = 0;
    };

    ScreenshotText() = default;
    ~ScreenshotText();
    ScreenshotText(const ScreenshotText&) = delete;
    ScreenshotText& operator=(const ScreenshotText&) = delete;

    // Starts (or restarts) reading `items` in the background. Posts `message`
    // to `notify` as progress is made.
    void start(const std::filesystem::path& cache, std::vector<Item> items, HWND notify, UINT message);
    void stop();

    // Lower-case text found in a screenshot, or empty.
    std::wstring text(const std::string& key) const;
    std::size_t done() const { return done_.load(); }
    std::size_t total() const { return total_.load(); }
    bool running() const { return running_.load(); }
    // False if Windows has no text recognition language installed.
    bool available() const { return available_.load(); }

private:
    void run(std::filesystem::path cache, std::vector<Item> items, HWND notify, UINT message);
    void load(const std::filesystem::path& cache);
    void save(const std::filesystem::path& cache) const;

    mutable std::mutex mutex_;
    struct Entry {
        std::int64_t modified = 0;
        std::wstring text;
    };
    std::map<std::string, Entry> entries_;
    std::thread thread_;
    std::atomic<bool> stopping_{false};
    std::atomic<bool> running_{false};
    std::atomic<bool> available_{true};
    std::atomic<std::size_t> done_{0};
    std::atomic<std::size_t> total_{0};
};

}  // namespace nso
