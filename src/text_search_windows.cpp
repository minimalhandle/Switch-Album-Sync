// C++/WinRT headers go first so Windows' macros can't clash with them.
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Graphics.Imaging.h>
#include <winrt/Windows.Media.Ocr.h>
#include <winrt/Windows.Storage.h>
#include <winrt/Windows.Storage.Streams.h>

#include "text_search_windows.hpp"

#include "nso_album_sync/json.hpp"
#include "nso_album_sync/local_data.hpp"

namespace nso {
namespace {

constexpr std::size_t kSaveEvery = 20;

std::string utf8(const std::wstring& value) {
    if (value.empty()) return {};
    const int size = WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
    std::string result(static_cast<std::size_t>(size), '\0');
    if (size) WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), result.data(), size, nullptr, nullptr);
    return result;
}

std::wstring wide(const std::string& value) {
    if (value.empty()) return {};
    const int size = MultiByteToWideChar(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), nullptr, 0);
    std::wstring result(static_cast<std::size_t>(size), L'\0');
    if (size) MultiByteToWideChar(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), result.data(), size);
    return result;
}

std::wstring folded(std::wstring value) {
    if (!value.empty()) CharLowerBuffW(value.data(), static_cast<DWORD>(value.size()));
    return value;
}

std::wstring recognize(const winrt::Windows::Media::Ocr::OcrEngine& engine, const std::filesystem::path& file) {
    using namespace winrt::Windows::Graphics::Imaging;
    using namespace winrt::Windows::Storage;
    const auto storage = StorageFile::GetFileFromPathAsync(winrt::hstring(file.wstring())).get();
    const auto stream = storage.OpenAsync(FileAccessMode::Read).get();
    const auto decoder = BitmapDecoder::CreateAsync(stream).get();
    const auto limit = winrt::Windows::Media::Ocr::OcrEngine::MaxImageDimension();
    if (decoder.PixelWidth() > limit || decoder.PixelHeight() > limit) return {};
    const auto bitmap = decoder.GetSoftwareBitmapAsync(BitmapPixelFormat::Bgra8, BitmapAlphaMode::Premultiplied).get();
    return std::wstring(engine.RecognizeAsync(bitmap).get().Text());
}

}  // namespace

ScreenshotText::~ScreenshotText() {
    stop();
}

void ScreenshotText::stop() {
    stopping_.store(true);
    if (thread_.joinable()) thread_.join();
    stopping_.store(false);
    running_.store(false);
}

void ScreenshotText::start(const std::filesystem::path& cache, std::vector<Item> items, HWND notify, UINT message) {
    stop();
    total_.store(items.size());
    done_.store(0);
    running_.store(true);
    thread_ = std::thread([this, cache, items = std::move(items), notify, message]() mutable {
        run(cache, std::move(items), notify, message);
    });
}

std::wstring ScreenshotText::text(const std::string& key) const {
    std::lock_guard lock(mutex_);
    const auto found = entries_.find(key);
    return found == entries_.end() ? std::wstring{} : found->second.text;
}

void ScreenshotText::load(const std::filesystem::path& cache) {
    const auto data = read_local_data(cache);
    std::lock_guard lock(mutex_);
    entries_.clear();
    const auto* files = data.find("files");
    if (!files || !files->is_object()) return;
    for (const auto& [key, value] : files->as_object()) {
        if (!value.is_object()) continue;
        entries_[key] = {value.integer("m"), wide(value.string("t"))};
    }
}

void ScreenshotText::save(const std::filesystem::path& cache) const {
    Json::object files;
    {
        std::lock_guard lock(mutex_);
        for (const auto& [key, entry] : entries_) {
            files.emplace(key, Json(Json::object{{"m", entry.modified}, {"t", utf8(entry.text)}}));
        }
    }
    write_local_data(cache, Json(Json::object{{"version", 1}, {"files", Json(std::move(files))}}));
}

void ScreenshotText::run(std::filesystem::path cache, std::vector<Item> items, HWND notify, UINT message) {
    const auto report = [&] { if (notify) PostMessageW(notify, message, 0, 0); };
    load(cache);
    try {
        winrt::init_apartment(winrt::apartment_type::multi_threaded);
    } catch (...) {
    }
    try {
        const auto engine = winrt::Windows::Media::Ocr::OcrEngine::TryCreateFromUserProfileLanguages();
        if (!engine) {
            available_.store(false);
        } else {
            std::size_t since_save = 0;
            for (const auto& item : items) {
                if (stopping_.load()) break;
                bool current = false;
                {
                    std::lock_guard lock(mutex_);
                    const auto found = entries_.find(item.key);
                    current = found != entries_.end() && found->second.modified == item.modified;
                }
                if (!current) {
                    std::wstring found_text;
                    try {
                        found_text = folded(recognize(engine, item.file));
                    } catch (...) {
                        // Unreadable image: remember it as having no text.
                    }
                    {
                        std::lock_guard lock(mutex_);
                        entries_[item.key] = {item.modified, std::move(found_text)};
                    }
                    if (++since_save >= kSaveEvery) {
                        save(cache);
                        since_save = 0;
                        report();
                    }
                }
                done_.fetch_add(1);
            }
            if (since_save) save(cache);
        }
    } catch (...) {
        available_.store(false);
    }
    winrt::uninit_apartment();
    running_.store(false);
    report();
}

}  // namespace nso
