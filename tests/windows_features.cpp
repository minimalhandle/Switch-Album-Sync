#include "album_tools_windows.hpp"
#include "import_windows.hpp"
#include "share_windows.hpp"
#include "text_search_windows.hpp"
#include "video_windows.hpp"
#include "nso_album_sync/auto_sync_policy.hpp"
#include "nso_album_sync/backup.hpp"
#include "nso_album_sync/auth_callback.hpp"
#include "nso_album_sync/config.hpp"
#include "nso_album_sync/discord.hpp"
#include "nso_album_sync/http.hpp"
#include "nso_album_sync/json.hpp"
#include "nso_album_sync/local_data.hpp"
#include "nso_album_sync/path.hpp"
#include "nso_album_sync/rpc.hpp"
#include "nso_album_sync/sync.hpp"
#include "nso_album_sync/util.hpp"
#include "nso_album_sync/version.hpp"

#include <winsock2.h>
#include <gdiplus.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <wrl/client.h>
#include <bit>
#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <vector>

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

CLSID encoder_for(const wchar_t* mime) {
    UINT count = 0, bytes = 0;
    Gdiplus::GetImageEncodersSize(&count, &bytes);
    std::vector<BYTE> buffer(bytes);
    auto* encoders = reinterpret_cast<Gdiplus::ImageCodecInfo*>(buffer.data());
    Gdiplus::GetImageEncoders(count, bytes, encoders);
    for (UINT i = 0; i < count; ++i) if (std::wstring(encoders[i].MimeType) == mime) return encoders[i].Clsid;
    throw std::runtime_error("Image encoder missing");
}

// A 640x360 picture with diagonal stripes; `seed` changes the pattern.
void pattern_image(const Path& path, int seed, const wchar_t* mime = L"image/png", const wchar_t* text = nullptr) {
    std::filesystem::create_directories(path.parent_path());
    Gdiplus::Bitmap bitmap(640, 360, PixelFormat32bppARGB);
    {
        Gdiplus::Graphics graphics(&bitmap);
        // Wide bands whose brightness order depends on the seed, like distinct scenes.
        for (int band = 0; band < 16; ++band) {
            const int level = (band * 37 + seed * 101 + (band % 3) * seed * 13) % 256;
            Gdiplus::SolidBrush brush(Gdiplus::Color(255, static_cast<BYTE>(level), static_cast<BYTE>((level + seed * 40) % 256), static_cast<BYTE>(255 - level)));
            graphics.FillRectangle(&brush, band * 40, 0, 40, 360);
        }
        for (int i = 0; i < 12; ++i) {
            Gdiplus::SolidBrush brush(Gdiplus::Color(255, (i * 40 + seed * 70) % 256, (i * 90 + seed * 30) % 256, (seed * 50) % 256));
            graphics.FillRectangle(&brush, (i * 53 + seed * 37) % 600, (i * 29 + seed * 17) % 320, 40 + i * 6, 30 + seed % 7 * 5);
        }
        if (text) {
            Gdiplus::FontFamily family(L"Segoe UI");
            Gdiplus::Font font(&family, 54, Gdiplus::FontStyleBold, Gdiplus::UnitPixel);
            Gdiplus::SolidBrush ink(Gdiplus::Color(255, 0, 0, 0));
            Gdiplus::SolidBrush paper(Gdiplus::Color(255, 255, 255, 255));
            graphics.FillRectangle(&paper, 20, 120, 600, 110);
            graphics.DrawString(text, -1, &font, Gdiplus::PointF(40, 140), &ink);
        }
    }
    const auto clsid = encoder_for(mime);
    require(bitmap.Save(path.c_str(), &clsid, nullptr) == Gdiplus::Ok, "Save pattern image");
}

nso::LocalCapture capture_at(const Path& file, const Path& folder, std::int64_t modified, bool video = false) {
    std::error_code error;
    return {file, folder, modified, std::filesystem::file_size(file, error), video, 0};
}

void auto_sync_defaults(const Path& root) {
    const nso::AppConfig defaults;
    require(!defaults.auto_sync && defaults.sync_interval_minutes == 60, "Auto-sync is off and hourly by default");
    // A brand-new settings folder loads the same defaults.
    const auto app_data = root / L"FreshRoaming";
    std::filesystem::create_directories(app_data);
    wchar_t* previous = nullptr;
    std::size_t length = 0;
    _wdupenv_s(&previous, &length, L"APPDATA");
    const std::wstring saved = previous ? previous : L"";
    std::free(previous);
    _wputenv_s(L"APPDATA", app_data.c_str());
    const auto config = nso::ConfigManager().snapshot();
    _wputenv_s(L"APPDATA", saved.c_str());
    require(!config.auto_sync && config.sync_interval_minutes == 60, "Fresh settings: auto-sync off, 60 minutes");
    require(config.pause_on_battery_saver && config.pause_on_metered && !config.quiet_hours, "Quieter auto-sync defaults");

    require(nso::in_quiet_hours(23, 7, 23) && nso::in_quiet_hours(23, 7, 3) && !nso::in_quiet_hours(23, 7, 7) &&
        !nso::in_quiet_hours(23, 7, 12), "Quiet hours wrap past midnight");
    require(nso::in_quiet_hours(1, 5, 2) && !nso::in_quiet_hours(1, 5, 5) && !nso::in_quiet_hours(4, 4, 4), "Quiet hours in one day");
    nso::AppConfig quiet;
    require(nso::auto_sync_pause_reason(quiet, false, false, 12).empty(), "Nothing pauses by default at noon");
    require(!nso::auto_sync_pause_reason(quiet, true, false, 12).empty(), "Battery saver pauses auto-sync");
    require(!nso::auto_sync_pause_reason(quiet, false, true, 12).empty(), "Metered connections pause auto-sync");
    quiet.pause_on_metered = false;
    require(nso::auto_sync_pause_reason(quiet, false, true, 12).empty(), "Metered pause can be turned off");
    quiet.quiet_hours = true;
    require(!nso::auto_sync_pause_reason(quiet, false, false, 2).empty(), "Quiet hours pause auto-sync");
}

void backups(const Path& root) {
    const auto album = root / L"BackupAlbum";
    const auto backup = root / L"BackupCopy";
    std::filesystem::create_directories(backup);
    pattern_image(album / L"Zelda" / L"2026111012000000_c.png", 1);
    pattern_image(album / L"Splatoon 3" / L"2026111012010000_c.png", 2);
    pattern_image(album / L".switch-album-sync-import-review" / L"slot" / L"x.png", 3);
    { std::ofstream(album / L"notes.txt") << "not a capture"; }

    auto result = nso::backup_album(album, backup);
    require(result.copied == 2 && result.failed == 0, "Backup copies captures");
    require(std::filesystem::exists(backup / L"Zelda" / L"2026111012000000_c.png"), "Backup keeps folders");
    require(!std::filesystem::exists(backup / L".switch-album-sync-import-review"), "Backup skips working folders");
    require(!std::filesystem::exists(backup / L"notes.txt"), "Backup copies captures only");
    require(std::filesystem::last_write_time(album / L"Zelda" / L"2026111012000000_c.png") ==
        std::filesystem::last_write_time(backup / L"Zelda" / L"2026111012000000_c.png"), "Backup keeps timestamps");
    result = nso::backup_album(album, backup);
    require(result.copied == 0 && result.unchanged == 2, "Unchanged captures aren't copied again");
    std::filesystem::remove(album / L"Zelda" / L"2026111012000000_c.png");
    nso::backup_album(album, backup);
    require(std::filesystem::exists(backup / L"Zelda" / L"2026111012000000_c.png"), "Backups never delete");
    bool rejected = false;
    try {
        nso::backup_album(album, album / L"Zelda");
    } catch (const std::runtime_error&) {
        rejected = true;
    }
    require(rejected && !nso::valid_backup_folder(root, album), "Backup folder can't overlap the album");
}

void duplicate_finder(const Path& root) {
    const auto album = root / L"DupAlbum";
    const auto original = album / L"Zelda" / L"a.png";
    const auto exact = album / L"Imported captures" / L"a copy.png";
    const auto reencoded = album / L"Zelda" / L"a.jpg";
    const auto different = album / L"Zelda" / L"b.png";
    const auto later = album / L"Zelda" / L"a later.bmp";
    pattern_image(original, 4);
    std::filesystem::create_directories(exact.parent_path());
    std::filesystem::copy_file(original, exact);
    pattern_image(reencoded, 4, L"image/jpeg");
    pattern_image(different, 9);
    pattern_image(later, 4, L"image/bmp");
    const auto a = nso::image_fingerprint(original), b = nso::image_fingerprint(reencoded), c = nso::image_fingerprint(different);
    require(a && b && c, "Fingerprints computed");
    require(std::popcount(a ^ b) <= 6 && std::popcount(a ^ c) > 6, "Fingerprints match look-alikes only");

    const std::vector<nso::LocalCapture> captures{
        capture_at(original, album / L"Zelda", 1000), capture_at(exact, album / L"Imported captures", 1000),
        capture_at(reencoded, album / L"Zelda", 1010), capture_at(different, album / L"Zelda", 1020),
        capture_at(later, album / L"Zelda", 5000)};
    const auto exact_only = nso::find_duplicate_captures(captures, album, false, {});
    require(exact_only.size() == 1 && exact_only[0].exact && exact_only[0].files.size() == 2, "Identical copies found");
    require(exact_only[0].files[0] == original, "The copy in a game folder is kept");
    const auto all = nso::find_duplicate_captures(captures, album, true, {});
    bool similar = false;
    for (const auto& group : all) {
        if (group.exact) continue;
        similar = true;
        require(std::find(group.files.begin(), group.files.end(), reencoded) != group.files.end(), "Re-encoded copy grouped");
        require(std::find(group.files.begin(), group.files.end(), different) == group.files.end(), "Different shot not grouped");
        require(std::find(group.files.begin(), group.files.end(), later) == group.files.end(), "Shots far apart in time not grouped");
    }
    require(similar, "Look-alike screenshots found");
}

void highlights() {
    std::vector<nso::LocalCapture> period;
    std::vector<bool> favourite;
    for (int i = 0; i < 100; ++i) {
        period.push_back({Path(L"shot" + std::to_wstring(i) + L".png"), {}, 1000 + i, 1, false, 0});
        favourite.push_back(i == 97 || i == 3);
    }
    const auto picks = nso::pick_highlights(period, favourite, 48);
    require(picks.size() == 48, "Highlights fill the grid");
    require(std::find(picks.begin(), picks.end(), Path(L"shot97.png")) != picks.end() &&
        std::find(picks.begin(), picks.end(), Path(L"shot3.png")) != picks.end(), "Favourites are always included");
    require(picks.front() == Path(L"shot0.png") || picks.front() == Path(L"shot3.png"), "Highlights are in time order");
    require(nso::pick_highlights(period, favourite, 3).size() == 3, "Highlights respect the limit");
}

// Writes a short H.264 test video with Media Foundation.
void test_video(const Path& file, int seconds) {
    using Microsoft::WRL::ComPtr;
    constexpr UINT32 width = 320, height = 240, fps = 30;
    require(SUCCEEDED(MFStartup(MF_VERSION)), "Start Media Foundation");
    {
        ComPtr<IMFSinkWriter> writer;
        require(SUCCEEDED(MFCreateSinkWriterFromURL(file.c_str(), nullptr, nullptr, &writer)), "Create test video");
        ComPtr<IMFMediaType> out, in;
        MFCreateMediaType(&out);
        out->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
        out->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_H264);
        out->SetUINT32(MF_MT_AVG_BITRATE, 800000);
        out->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
        MFSetAttributeSize(out.Get(), MF_MT_FRAME_SIZE, width, height);
        MFSetAttributeRatio(out.Get(), MF_MT_FRAME_RATE, fps, 1);
        MFSetAttributeRatio(out.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
        DWORD stream = 0;
        require(SUCCEEDED(writer->AddStream(out.Get(), &stream)), "Add test video stream");
        MFCreateMediaType(&in);
        in->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
        in->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_RGB32);
        in->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
        MFSetAttributeSize(in.Get(), MF_MT_FRAME_SIZE, width, height);
        MFSetAttributeRatio(in.Get(), MF_MT_FRAME_RATE, fps, 1);
        MFSetAttributeRatio(in.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
        require(SUCCEEDED(writer->SetInputMediaType(stream, in.Get(), nullptr)), "Set test video input");
        require(SUCCEEDED(writer->BeginWriting()), "Begin test video");
        const LONGLONG duration = 10'000'000 / fps;
        for (UINT32 frame = 0; frame < static_cast<UINT32>(seconds) * fps; ++frame) {
            ComPtr<IMFMediaBuffer> buffer;
            MFCreateMemoryBuffer(width * height * 4, &buffer);
            BYTE* data = nullptr;
            buffer->Lock(&data, nullptr, nullptr);
            for (UINT32 i = 0; i < width * height; ++i) {
                data[i * 4] = static_cast<BYTE>(frame * 8);
                data[i * 4 + 1] = static_cast<BYTE>(i % width);
                data[i * 4 + 2] = static_cast<BYTE>(i / width);
                data[i * 4 + 3] = 255;
            }
            buffer->Unlock();
            buffer->SetCurrentLength(width * height * 4);
            ComPtr<IMFSample> sample;
            MFCreateSample(&sample);
            sample->AddBuffer(buffer.Get());
            sample->SetSampleTime(frame * duration);
            sample->SetSampleDuration(duration);
            require(SUCCEEDED(writer->WriteSample(stream, sample.Get())), "Write test frame");
        }
        require(SUCCEEDED(writer->Finalize()), "Finish test video");
    }
    MFShutdown();
}

void video_editing(const Path& root) {
    const auto source = root / L"Video" / L"2026111012000000_c.mp4";
    std::filesystem::create_directories(source.parent_path());
    test_video(source, 3);
    const auto info = nso::probe_video(source);
    require(info && info->width == 320 && info->height == 240 && info->seconds > 2.5 && info->seconds < 3.5, "Probe video");

    const std::atomic<bool> not_cancelled{false};
    const auto clip = root / L"Video" / L"clip.mp4";
    nso::export_video_clip(source, clip, 1.0, 2.0, not_cancelled);
    const auto clipped = nso::probe_video(clip);
    require(clipped && clipped->seconds > 0.7 && clipped->seconds < 1.3, "Clip has the chosen length");

    const auto gif = root / L"Video" / L"clip.gif";
    nso::export_video_gif(source, gif, 0.5, 1.5, 160, 10, not_cancelled);
    std::ifstream file(gif, std::ios::binary);
    char header[6]{};
    file.read(header, 6);
    require(std::string(header, 6) == "GIF89a", "GIF written");
    Gdiplus::Image image(gif.c_str());
    require(image.GetLastStatus() == Gdiplus::Ok && image.GetWidth() == 160 && image.GetHeight() == 120, "GIF scaled");
    const auto frames = image.GetFrameCount(&Gdiplus::FrameDimensionTime);
    require(frames >= 8 && frames <= 12, "GIF has about one second at 10 frames per second");

    const std::atomic<bool> cancelled{true};
    const auto stopped = root / L"Video" / L"cancelled.mp4";
    bool threw = false;
    try {
        nso::export_video_clip(source, stopped, 0, 2, cancelled);
    } catch (const std::runtime_error&) {
        threw = true;
    }
    require(threw && !std::filesystem::exists(stopped), "Cancelled exports leave no partial file");
}

void screenshot_text(const Path& root) {
    const auto shot = root / L"Text" / L"2026111012000000_c.png";
    pattern_image(shot, 5, L"image/png", L"SWITCH ALBUM");
    nso::ScreenshotText reader;
    reader.start(root / L"Text" / L"capture-text.json", {{"Text/shot.png", shot, 1}}, nullptr, 0);
    for (int i = 0; i < 300 && reader.running(); ++i) Sleep(100);
    require(!reader.running(), "Text reading finishes");
    if (!reader.available()) {
        std::cout << "Note: no Windows text recognition language installed; skipped OCR check\n";
        return;
    }
    require(reader.text("Text/shot.png").find(L"switch") != std::wstring::npos, "Text read from a screenshot");
    require(std::filesystem::exists(root / L"Text" / L"capture-text.json"), "Text cached on disk");
}

void json_nesting_limit() {
    const auto nested = [](int depth) {
        return std::string(static_cast<std::size_t>(depth), '[') + std::string(static_cast<std::size_t>(depth), ']');
    };
    require(nso::Json::parse(nested(128)).is_array(), "JSON accepts 128 nested levels");
    bool rejected = false;
    try {
        nso::Json::parse(nested(129));
    } catch (const std::runtime_error&) {
        rejected = true;
    }
    require(rejected, "JSON rejects deeper nesting");
    require(nso::Json::parse(R"({"a":[{"b":[1,2,{"c":null}]}]})").is_object(), "JSON parses ordinary nesting");
}

bool json_rejects(const std::string& text) {
    try {
        nso::Json::parse(text);
    } catch (const std::runtime_error&) {
        return true;
    }
    return false;
}

void json_parsing() {
    const auto parsed = nso::Json::parse(R"({"s":"a\"b\\cé😀","n":-12.5,"i":4834290508791808,"t":true})");
    require(parsed.string("s") == "a\"b\\c\xC3\xA9\xF0\x9F\x98\x80", "JSON decodes escapes and surrogate pairs");
    require(parsed.number("n") == -12.5, "JSON parses negative decimals");
    require(parsed.integer("i") == 4834290508791808LL, "JSON keeps service IDs exact");
    require(parsed.boolean("t"), "JSON parses booleans");
    require(json_rejects("[1,]"), "JSON rejects trailing commas");
    require(json_rejects("nul"), "JSON rejects bad literals");
    require(json_rejects("{} x"), "JSON rejects trailing input");
    require(json_rejects(R"({"a" 1})"), "JSON rejects a missing colon");
}

void auth_callbacks() {
    require(nso::is_nintendo_auth_callback("npf71b963c1b7b6d119://auth#session_token_code=x"), "Callback with fragment");
    require(nso::is_nintendo_auth_callback("npf71b963c1b7b6d119://auth/#session_token_code=x"), "Callback with Chromium slash");
    require(nso::is_nintendo_auth_callback("NPF71B963C1B7B6D119://auth?state=x"), "Callback scheme is case-insensitive");
    require(!nso::is_nintendo_auth_callback("npf71b963c1b7b6d119://authx#a"), "Callback host must be auth");
    require(!nso::is_nintendo_auth_callback("https://accounts.nintendo.com/#a"), "Web URLs are not callbacks");
}

void media_urls() {
    require(nso::is_safe_media_url("https://cdn.example.com/a.jpg?sig=1"), "Public HTTPS media URL accepted");
    require(nso::is_safe_media_url("https://cdn.example.com:443/a.jpg"), "Explicit port 443 accepted");
    for (const auto* url : {"http://cdn.example.com/a.jpg", "https://127.0.0.1/a.jpg", "https://[::1]/a.jpg",
                            "https://localhost/a.jpg", "https://nas.local/a.jpg", "https://intranet/a.jpg",
                            "https://cdn.example.com:8443/a.jpg", "https://user@cdn.example.com/a.jpg"}) {
        require(!nso::is_safe_media_url(url), "Unsafe media URL rejected");
    }
}

void capture_names(const Path& root) {
    const auto seconds = nso::capture_filename_prefix(1700000000);
    require(seconds.size() == 16 && seconds.ends_with("00") &&
        std::all_of(seconds.begin(), seconds.end(), [](unsigned char c) { return std::isdigit(c) != 0; }),
        "Capture prefix is 16 digits");
    require(seconds == nso::capture_filename_prefix(1700000000000LL), "Seconds and milliseconds give the same name");

    const auto album = root / L"Folders";
    std::filesystem::create_directories(album / L"The Legend of Zelda Tears of the Kingdom");
    std::filesystem::create_directories(album / L"Splatoon 3");
    require(nso::resolve_game_folder(album, "Tears of the Kingdom - Nintendo Switch 2 Edition") ==
        "The Legend of Zelda Tears of the Kingdom", "Switch 2 Edition joins the existing game folder");
    require(nso::resolve_game_folder(album, "Splatoon 3") == "Splatoon 3", "Exact folder match");
    require(nso::resolve_game_folder(album, "Foo: Bar?") == "Foo Bar", "Unsafe folder characters removed");
    require(nso::resolve_game_folder(album, "") == "Other", "Untitled captures go to Other");
}

void sync_receipts(const Path& root) {
    const auto file = root / L"synced-media.json";
    const auto now = nso::unix_now();
    {
        nso::SyncReceipts receipts(file, root / L"Album A");
        require(!receipts.contains("one"), "New receipts are empty");
        receipts.add("one", 0, now);
        receipts.add("milliseconds", (now + 86400) * 1000, 0);
        receipts.add("expired", now - 30LL * 86400, 0);
        receipts.add("", 0, now);
        require(receipts.contains("one") && !receipts.contains(""), "Receipts record media IDs");
        require(receipts.save(), "Receipts save");
    }
    nso::SyncReceipts same(file, root / L"album a");
    require(same.contains("one") && same.contains("milliseconds"), "Receipts persist per album, case-insensitively");
    require(!same.contains("expired"), "Receipts drop captures that expired over a week ago");
    nso::SyncReceipts other(file, root / L"Album B");
    require(!other.contains("one"), "Another album has its own receipts");
}

std::wstring environment(const wchar_t* name) {
    wchar_t* value = nullptr;
    std::size_t length = 0;
    if (_wdupenv_s(&value, &length, name) != 0 || value == nullptr) return {};
    std::wstring result(value);
    std::free(value);
    return result;
}

void config_migration(const Path& root) {
    nso::AppConfig unacknowledged;
    unacknowledged.session_token = "token";
    require(nso::needs_nxapi_disclosure(unacknowledged), "Saved sign-ins need the disclosure first");
    unacknowledged.nxapi_disclosure_acknowledged = nso::kNxapiDisclosureVersion;
    require(!nso::needs_nxapi_disclosure(unacknowledged), "Acknowledged sign-ins don't ask again");
    require(!nso::needs_nxapi_disclosure(nso::AppConfig{}), "No sign-in, no disclosure");

    // A legacy settings file without any credential, so the Windows
    // Credential Manager is never touched.
    const auto app_data = root / L"Roaming";
    std::filesystem::create_directories(app_data / L"Switch Album Sync");
    {
        std::ofstream file(app_data / L"Switch Album Sync" / L"config.json", std::ios::binary);
        file << R"({"AutoSyncEnabled":true,"SyncIntervalMinutes":1,"discordPresence":true,)"
             << R"("nxapiAuthClientId":"eJ8TDme0c-Z4czx5SvZabA","appearance":"dark"})";
    }
    const auto previous = environment(L"APPDATA");
    require(_wputenv_s(L"APPDATA", app_data.c_str()) == 0, "Point APPDATA at the test folder");
    struct Restore {
        std::wstring value;
        ~Restore() { _wputenv_s(L"APPDATA", value.c_str()); }
    } restore{previous};

    nso::ConfigManager manager;
    require(manager.directory() == app_data / L"Switch Album Sync", "Config uses APPDATA");
    const auto config = manager.snapshot();
    require(config.auto_sync, "Legacy auto-sync key migrates");
    require(config.sync_interval_minutes == nso::kMinimumSyncIntervalMinutes, "Auto-sync interval has a floor");
    require(!config.discord_presence, "Old Rich Presence defaults need a fresh opt-in");
    require(config.appearance == nso::Appearance::dark, "Appearance loads");
    require(config.nxapi_disclosure_acknowledged == 0 && config.discord_disclosure_acknowledged == 0,
        "Disclosures start unacknowledged");
    const auto saved = nso::read_local_data(manager.directory() / L"config.json");
    require(saved.find("nxapiAuthClientId") == nullptr, "The saved nxapi client ID is removed");
    require(saved.integer("discordPresenceSettingVersion") == 1, "Config is rewritten after migration");
}

void rich_presence() {
    nso::NintendoPresence presence;
    presence.state = "PLAYING";
    presence.title_id = "0100c2500fc20000";
    presence.user_name = "Player";
    presence.profile_image_uri = "https://example.com/me.png";
    presence.sys_description = "Played for 10 hours or more";
    presence.total_play_time = 600;
    presence.custom_details = "Player • Fresh";
    presence.custom_details_without_name = "Fresh";
    require(nso::rpc_game_service_for(presence) == nso::RpcGameService::Splatoon3, "Splatoon 3 by title ID");

    auto hidden_name = presence;
    hidden_name.rpc.show_username = false;
    hidden_name = nso::rpc_display_presence(hidden_name);
    require(hidden_name.user_name.empty() && hidden_name.custom_details == "Fresh", "Username can be hidden");

    auto hidden_picture = presence;
    hidden_picture.rpc.show_profile_picture = false;
    hidden_picture = nso::rpc_display_presence(hidden_picture);
    require(hidden_picture.profile_image_uri.empty() && hidden_picture.user_name.empty(),
        "Hiding the picture also hides the username");

    auto hidden_time = presence;
    hidden_time.rpc.show_play_time = false;
    hidden_time = nso::rpc_display_presence(hidden_time);
    require(hidden_time.sys_description.empty() && hidden_time.total_play_time == 0, "Play time can be hidden");

    auto no_details = presence;
    no_details.rpc.splatoon3 = false;
    no_details = nso::rpc_display_presence(no_details);
    require(no_details.custom_details.empty(), "Disabled game details are removed");

    nso::NintendoPresence store;
    store.title_id = "0100C2500FC20000";
    require(nso::rpc_store_url(store) == "https://ec.nintendo.com/apps/0100c2500fc20000/US", "Store link from title ID");
    store.title_id = "not-a-title";
    require(nso::rpc_store_url(store).empty(), "Invalid title IDs give no store link");

    using std::chrono::seconds;
    require(nso::presence_poll_interval(true, 3) == seconds(60), "Playing polls every minute");
    require(nso::presence_poll_interval(false, 2) == seconds(60), "First offline checks stay at a minute");
    require(nso::presence_poll_interval(false, 3) == seconds(300), "Offline polling backs off to 5 minutes");
    require(nso::presence_poll_interval(false, nso::kPresenceOfflineChecksBeforeLongBackoff) == seconds(900),
        "After about an hour offline, polling backs off to 15 minutes");
    require(nso::presence_poll_interval(true, nso::kPresenceOfflineChecksBeforeLongBackoff) == seconds(60),
        "Playing again goes straight back to every minute");

    using std::chrono::hours;
    using std::chrono::minutes;
    require(!nso::presence_idle_paused(hours(7), hours(7)), "Polling continues during the first 8 hours offline");
    require(nso::presence_idle_paused(hours(8), minutes(15)), "Polling pauses after 8 hours offline with nobody at the PC");
    require(!nso::presence_idle_paused(hours(9), minutes(2)), "Using the PC resumes polling");

    nso::RpcEnrichmentCache enrichment;
    enrichment.begin("game", 1, 1);
    const auto start = nso::RpcEnrichmentCache::Clock::now();
    require(enrichment.should_probe(start), "A new game session probes once");
    enrichment.complete(nso::NintendoPresence{}, false, start);
    require(!enrichment.should_probe(start + hours(24)), "A failed probe is never repeated automatically");
    require(enrichment.begin("game", 1, 2) && enrichment.should_probe(start),
        "Refresh presence (a new settings revision) probes again");

    const auto before = nso::requests_today();
    nso::count_request("https://api-lp1.znc.srv.nintendo.net/v4/Media/List");
    nso::count_request("https://accounts.nintendo.com/connect/1.0.0/api/token");
    nso::count_request("https://nxapi-znca-api.fancy.org.uk/api/znca/f");
    nso::count_request("https://splatoon3.ink/assets/x.png");
    nso::count_request("https://evilnintendo.net/");
    const auto after = nso::requests_today();
    require(after.nintendo == before.nintendo + 2, "Nintendo requests are counted by host");
    require(after.nxapi == before.nxapi + 1, "nxapi requests are counted; other hosts are not");
}

void discord_detection() {
    if constexpr (nso::discord_available) {
        nso::DiscordPresence discord(nso::kDiscordApplicationId);
        const bool before = discord.client_running();
        const HANDLE pipe = CreateNamedPipeW(L"\\\\.\\pipe\\discord-ipc-9", PIPE_ACCESS_DUPLEX,
            PIPE_TYPE_BYTE, 1, 0, 0, 0, nullptr);
        if (pipe == INVALID_HANDLE_VALUE) {
            require(before, "Discord IPC pipe exists or can be created");
            return;
        }
        require(discord.client_running(), "A discord-ipc pipe means Discord is running");
        CloseHandle(pipe);
        if (!before) require(!discord.client_running(), "No discord-ipc pipe means no Discord client");
    }
}

// A tiny HTTP/1.1 server on 127.0.0.1 that records each request's headers.
class LoopbackServer {
public:
    LoopbackServer() {
        WSADATA data{};
        require(WSAStartup(MAKEWORD(2, 2), &data) == 0, "Start Winsock");
        listener_ = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        require(listener_ != INVALID_SOCKET, "Create loopback socket");
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        require(bind(listener_, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0, "Bind loopback socket");
        require(listen(listener_, 4) == 0, "Listen on loopback socket");
        int length = sizeof(address);
        require(getsockname(listener_, reinterpret_cast<sockaddr*>(&address), &length) == 0, "Read loopback port");
        port_ = ntohs(address.sin_port);
        acceptor_ = std::thread([this] { accept_loop(); });
    }

    ~LoopbackServer() {
        closesocket(listener_);
        if (acceptor_.joinable()) acceptor_.join();
        for (const auto client : clients_) shutdown(client, SD_BOTH);
        for (auto& connection : connections_) {
            if (connection.joinable()) connection.join();
        }
        WSACleanup();
    }

    std::string url() const { return "http://127.0.0.1:" + std::to_string(port_) + "/capture"; }

    std::vector<std::string> requests() {
        std::lock_guard lock(mutex_);
        return requests_;
    }

private:
    void accept_loop() {
        for (;;) {
            const SOCKET client = accept(listener_, nullptr, nullptr);
            if (client == INVALID_SOCKET) return;
            std::lock_guard lock(mutex_);
            clients_.push_back(client);
            connections_.emplace_back([this, client] { serve(client); });
        }
    }

    void serve(SOCKET client) {
        static constexpr char kResponse[] =
            "HTTP/1.1 200 OK\r\nContent-Length: 2\r\nSet-Cookie: session=abc; Path=/\r\n\r\nok";
        std::string buffer;
        char chunk[4096];
        for (;;) {
            if (const auto end = buffer.find("\r\n\r\n"); end != std::string::npos) {
                {
                    std::lock_guard lock(mutex_);
                    requests_.push_back(buffer.substr(0, end + 4));
                }
                buffer.erase(0, end + 4);
                send(client, kResponse, static_cast<int>(sizeof(kResponse) - 1), 0);
                continue;
            }
            const int received = recv(client, chunk, static_cast<int>(sizeof(chunk)), 0);
            if (received <= 0) break;
            buffer.append(chunk, static_cast<std::size_t>(received));
        }
        closesocket(client);
    }

    SOCKET listener_ = INVALID_SOCKET;
    unsigned short port_ = 0;
    std::thread acceptor_;
    std::mutex mutex_;
    std::vector<SOCKET> clients_;
    std::vector<std::thread> connections_;
    std::vector<std::string> requests_;
};

std::vector<std::string> header_values(const std::string& request, const std::string& name) {
    std::vector<std::string> values;
    std::size_t start = request.find("\r\n");
    while (start != std::string::npos) {
        start += 2;
        const auto end = request.find("\r\n", start);
        if (end == std::string::npos || end == start) break;
        const auto line = request.substr(start, end - start);
        const auto colon = line.find(':');
        if (colon != std::string::npos && nso::lower(line.substr(0, colon)) == name) {
            values.push_back(nso::trim(line.substr(colon + 1)));
        }
        start = end;
    }
    return values;
}

void http_client() {
    LoopbackServer server;
    {
        nso::HttpClient http;
        const auto first = http.get(server.url(), {"User-Agent: test-agent/1.0"}, 5);
        require(first.status == 200 && first.text() == "ok", "HTTP request succeeds");
        const auto second = http.get(server.url(), {}, 5);
        require(second.status == 200 && second.text() == "ok", "A second request reuses the shared session");
    }
    const auto requests = server.requests();
    require(requests.size() == 2, "Loopback server saw both requests");
    require(header_values(requests[0], "user-agent") == std::vector<std::string>{"test-agent/1.0"},
        "Only the caller's User-Agent is sent");
    require(header_values(requests[1], "user-agent") ==
        std::vector<std::string>{"Switch Album Sync/" NSO_APP_VERSION}, "One default User-Agent is sent");
    require(header_values(requests[1], "cookie").empty(), "Cookies from earlier responses are not replayed");
    require(header_values(requests[0], "connection").empty() ||
        nso::lower(header_values(requests[0], "connection").front()) != "close", "Connections stay open for reuse");
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
        json_nesting_limit();
        json_parsing();
        auth_callbacks();
        media_urls();
        capture_names(root);
        sync_receipts(root);
        config_migration(root);
        rich_presence();
        discord_detection();
        http_client();
        auto_sync_defaults(root);
        backups(root);
        duplicate_finder(root);
        highlights();
        video_editing(root);
        screenshot_text(root);
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
