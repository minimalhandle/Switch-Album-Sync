#include "import_windows.hpp"
#include "share_windows.hpp"
#include "nso_album_sync/path.hpp"
#include "nso_album_sync/sync.hpp"

#include <gdiplus.h>
#include <algorithm>
#include <chrono>
#include <fstream>
#include <iostream>
#include <stdexcept>

namespace {
using Path = std::filesystem::path;
const auto running = [] { return false; };
constexpr CLSID png{0x557cf406, 0x1a04, 0x11d3, {0x9a, 0x73, 0, 0, 0xf8, 0x1e, 0xf3, 0x2e}};

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

void screenshot(const Path& path, int width, int height) {
    std::filesystem::create_directories(path.parent_path());
    Gdiplus::Bitmap bitmap(width, height, PixelFormat32bppARGB);
    {
        Gdiplus::Graphics graphics(&bitmap);
        require(graphics.Clear(Gdiplus::Color(255, 230, 0, 18)) == Gdiplus::Ok, "Draw fixture");
    }
    require(bitmap.Save(path.c_str(), &png) == Gdiplus::Ok, "Save fixture");
}

void game_suggestions(const Path& root) {
    const auto album = root / L"Suggestions" / L"Album";
    const std::wstring signature(32, L'a');
    const auto known = album / L"Zelda" / L"2026111012000000_c.jpg";
    std::filesystem::create_directories(known.parent_path());
    std::ofstream(known) << "capture";
    nso::ImportPlan plan;
    plan.album = album;
    plan.captures.push_back({{}, {}, L"2026111012000000-" + signature + L".jpg", L"Imported captures"});
    plan.captures.push_back({{}, {}, L"2026111012010000-" + signature + L".jpg", L"Imported captures"});
    plan.captures.push_back({{}, {}, L"2026111012020000-" + std::wstring(32, L'b') + L".jpg", L"Imported captures"});
    const auto library = nso::scan_capture_library(album);
    nso::suggest_import_games(plan, library);
    require(plan.captures[0].suggested_game == L"Zelda", "Exact timestamp suggestion");
    require(plan.captures[1].suggested_game == L"Zelda", "Game signature suggestion");
    require(plan.captures[2].suggested_game.empty(), "Unknown game remains unassigned");
    require(plan.captures[0].game == L"Imported captures", "Suggestions require confirmation");
    auto conflicting = plan;
    conflicting.captures.push_back({{}, {}, L"2026111012030000-" + signature + L".jpg", L"Mario"});
    conflicting.captures[1].suggested_game.clear();
    nso::suggest_import_games(conflicting, library);
    require(conflicting.captures[1].suggested_game.empty(), "Ambiguous signatures are not suggested");
}

void watch_folder(const Path& root) {
    const auto source = root / L"Incoming";
    const auto album = root / L"Library" / L"Album";
    const auto receipts = root / L"receipts.json";
    std::filesystem::create_directories(source);
    std::filesystem::create_directories(album);
    require(nso::valid_watch_folder(source, album), "Separate watch source");
    require(!nso::valid_watch_folder(source, source / L"Album"), "Reject source containing album");
    require(!nso::valid_watch_folder(album, album), "Reject album as source");
    require(!nso::valid_watch_folder(album / L"Game", album), "Reject source inside album");
    const auto incoming = source / L"2026111012100000-test.png";
    screenshot(incoming, 160, 90);
    std::ofstream(source / L"unfinished.png") << "";
    std::ofstream(source / L"ignored.tmp") << "not media";
    nso::WatchedFolder watcher;
    require(watcher.poll(source, album, receipts, running).captures.empty(), "First observation waits");
    std::ofstream(incoming, std::ios::binary | std::ios::app) << "changed";
    require(watcher.poll(source, album, receipts, running).captures.empty(), "Changing file waits");
    auto plan = watcher.poll(source, album, receipts, running);
    require(plan.captures.size() == 1, "Stable file queued, non-media and empty files ignored");
    HANDLE writer = CreateFileW(incoming.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr,
        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    require(writer != INVALID_HANDLE_VALUE, "Open active writer");
    auto blocked = nso::import_windows_captures(plan, album, running);
    CloseHandle(writer);
    require(blocked.completed_sources.empty(), "Active writes defer importing");
    const auto imported = nso::import_windows_captures(plan, album, running);
    require(imported.copied == 1 && imported.completed_sources.size() == 1, "Watched file imports");
    require(std::filesystem::exists(incoming), "Original file retained");
    watcher.complete(imported.completed_sources);
    require(watcher.poll(source, album, receipts, running).captures.empty(), "Processed stamp not requeued");
    nso::WatchedFolder restarted;
    restarted.poll(source, album, receipts, running);
    require(restarted.poll(source, album, receipts, running).captures.empty(), "Receipts survive restart");

    // A different filename sharing a Nintendo timestamp must enter review once.
    const auto collision = source / L"2026111012100000_c.png";
    screenshot(collision, 120, 80);
    restarted.poll(source, album, receipts, running);
    plan = restarted.poll(source, album, receipts, running);
    const auto duplicate = nso::import_windows_captures(plan, album, running);
    require(duplicate.review == 1 && duplicate.completed_sources.size() == 1, "Watch uses duplicate review");
    restarted.complete(duplicate.completed_sources);
    require(nso::pending_import_matches(album).size() == 1, "Review entry retained");
    require(restarted.poll(source, album, receipts, running).captures.empty(), "Review not repeatedly queued");
    require(!nso::resolve_import_match(album, {incoming, incoming}, false), "Reject invalid review paths");
}

void sharing(const Path& root) {
    const auto landscape = root / L"Landscape.png";
    const auto portrait = root / L"Portrait.png";
    screenshot(landscape, 1600, 900);
    screenshot(portrait, 90, 160);
    const auto video = root / L"Clip.mp4";
    std::ofstream(video) << "video fixture";
    const std::vector<Path> captures{landscape, portrait, video};
    const auto sheet = nso::export_captures(captures, root, nso::ShareFormat::contact_sheet, false);
    {
        Gdiplus::Bitmap image(sheet.c_str());
        require(image.GetLastStatus() == Gdiplus::Ok && image.GetWidth() == 836 &&
            image.GetHeight() == 249, "Grid includes two screenshots");
        Gdiplus::Color pixel;
        image.GetPixel(40, 40, &pixel);
        require(pixel.GetR() == 230 && pixel.GetG() == 0 && pixel.GetB() == 18, "Grid contains source pixels");
        image.GetPixel(430, 40, &pixel);
        require(pixel.GetR() == 24, "Portrait is letterboxed");
    }
    const auto second = nso::export_captures(captures, root, nso::ShareFormat::contact_sheet, false);
    require(sheet != second && std::filesystem::exists(sheet), "Exports never overwrite");
    const auto folder = nso::export_captures(captures, root, nso::ShareFormat::folder, true);
    {
        Gdiplus::Bitmap resized((folder / landscape.filename()).c_str());
        require(resized.GetWidth() == 1280 && resized.GetHeight() == 720, "Folder resize preserved");
    }
    const auto zip = nso::export_captures(captures, root, nso::ShareFormat::zip, false);
    require(std::filesystem::file_size(zip) > std::filesystem::file_size(landscape), "ZIP export preserved");
    bool rejected = false;
    try { nso::export_captures({video}, root, nso::ShareFormat::contact_sheet, false); }
    catch (const std::exception&) { rejected = true; }
    require(rejected, "Video-only grid rejected");
    rejected = false;
    try { nso::export_captures(std::vector<Path>(49, landscape), root, nso::ShareFormat::contact_sheet, false); }
    catch (const std::exception&) { rejected = true; }
    require(rejected, "Grid count bounded");
    for (const auto& entry : std::filesystem::directory_iterator(root)) {
        require(!entry.path().filename().wstring().starts_with(L".switch-album-sync-share-"), "Export staging cleaned");
    }
}
}

int main() {
    const auto root = std::filesystem::temp_directory_path() /
        ("switch-album-sync-tests-" + std::to_string(GetCurrentProcessId()));
    Gdiplus::GdiplusStartupInput input;
    ULONG_PTR token = 0;
    if (Gdiplus::GdiplusStartup(&token, &input, nullptr) != Gdiplus::Ok) return 2;
    int result = 0;
    bool created = false;
    try {
        require(!std::filesystem::exists(root), "Unique test workspace");
        created = std::filesystem::create_directory(root);
        require(created, "Create test workspace");
        require(nso::album_directory({}) == nso::path_from_utf8(nso::default_album_folder()), "Flat default album");
        require(nso::album_directory(root) == root / "Album", "Custom album behavior preserved");
        game_suggestions(root);
        watch_folder(root);
        sharing(root);
        std::cout << "Windows capture feature tests passed\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        result = 1;
    }
    Gdiplus::GdiplusShutdown(token);
    std::error_code error;
    if (created) std::filesystem::remove_all(root, error);
    return result;
}
