# CaptureDock

**Automatically sync your Nintendo Switch Online album to your computer.**

CaptureDock is a fork of [Dycool’s NSO Album Sync](https://github.com/Dycool/NSO-Album-Sync).

🖼️ **Automatic album sync** — Downloads new screenshots and videos in the same `Album/<Game Name>/` layout used by Nintendo Switch USB transfers.

🎮 **Discord Rich Presence** — Optionally shows `Playing <Game Name>` with Nintendo Switch / Nintendo Switch 2 underneath and the game's artwork.

🌍 **Localized game folders** — Reuses existing game folders across different languages instead of creating duplicates.

🕒 **Original timestamps** — Keeps the original capture time on downloaded screenshots and videos.

🔐 **Secure sign-in** — Nintendo and Coral credentials are stored using the operating system credential store when available.

🖥️ **Cross-platform** — Native tray/menu-bar app for Windows, macOS and Linux.

## 🚀 Quick Start

1. Download the Windows x64 executable from [CaptureDock Releases](https://github.com/minimalhandle/CaptureDock/releases), or build from source.
2. Open CaptureDock and sign in with your Nintendo Account.
3. Use the default `Pictures/CaptureDock/Album` or choose your own album destination.
4. Enable Auto-sync to check for new captures every 60 minutes by default. Leave the app running in the tray or menu bar.

On Windows, the dashboard opens the **Capture library**, **Sync history**, and **Storage** views. Filter and preview local captures, mark favorites, or search tags with a query such as `tag:race`. Sync history keeps the last 100 runs and lets you retry an individual failure for the current sign-in and album. Storage shows screenshot and video sizes by game. The **Recent captures** tray submenu copies captures as files. **Copy diagnostics** shares status codes and counts without account or file details. Closing the dashboard leaves the app running when its tray icon is available.

Use **Import USB / SD** in the dashboard or tray to bring existing captures into your library. For USB, enable the console's **Copy to PC over USB** mode, connect the cable directly to the console, and select the capture folder under **This PC**. For microSD, select the card or its **Nintendo / Album** folder. CaptureDock copies the files, preserves their timestamps, and queues possible matches in **Duplicate review**. Date-organized captures without game folder names go under **Imported captures**.

CaptureDock runs alongside NSO Album Sync with a separate executable, tray icon, settings, credentials, startup entry, and default album. CaptureDock opens automatically after Nintendo sign-in when the browser callback is available. If another app owns the callback, right-click the account selection button on Nintendo’s page, copy its link, then use **Paste sign-in link** in CaptureDock.

Discord Rich Presence is optional and disabled by default. It requires the Discord desktop client and Activity Sharing. CaptureDock supports Zelda Notes live locations for **Ocarina of Time on Switch 2**, **Breath of the Wild - Switch 2 Edition**, and **Tears of the Kingdom - Switch 2 Edition**.

The 1.0.0 release includes a portable **Windows 10+ x64** executable.

---

## 🔨 Building

Requires **CMake 3.20+** and a **C++20** compiler. To build the dashboard and album sync without the Discord SDK, use `-DNSO_ENABLE_DISCORD=OFF`.

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DNSO_ENABLE_DISCORD=OFF
cmake --build build --config Release --parallel
```

For Rich Presence builds, extract the official standalone C++ Discord Social SDK from the Discord Developer Portal to `third_party/discord_social_sdk`, or set `-DNSO_DISCORD_SOCIAL_SDK_ROOT=<path>`. Set `-DNSO_ENABLE_DISCORD=ON` or leave the option unset in a fresh build directory to use the release default.

Rich Presence uses the CaptureDock Discord application ID. Public CI builds without the Discord SDK. The Windows release embeds the SDK runtime and includes the third-party notices.

Windows builds are unsigned. macOS builds use local/ad-hoc codesigning and do not require an Apple Developer membership. Required third-party runtime notices are included in release packages and are also available in [`THIRD-PARTY-NOTICES.txt`](THIRD-PARTY-NOTICES.txt).

---

## 🔐 Nintendo Account & nxapi

CaptureDock uses the third-party [`nxapi-znca-api`](https://github.com/samuelthomas2774/nxapi-znca-api) service for Nintendo Switch Online request attestation and Coral request/response encryption.

The app shows a disclosure before sign-in because the Nintendo Account `id_token`, Nintendo Account ID, birthday, country and language required by Coral, the Coral access token, and Coral API requests/responses used by the app are processed by that service. nxapi authentication tokens are kept in memory only, while reusable Nintendo/Coral credentials are stored in the operating system credential store when available.

---

## Credits

- **[Dycool/NSO-Album-Sync](https://github.com/Dycool/NSO-Album-Sync)** created the original app. Thanks to Diogo Enes and the upstream contributors.

- **Zelda Notes RPC** — Thanks to [Bentheminernz](https://github.com/Bentheminernz) for the Zelda Notes RPC reference work that helped inform the live-location Rich Presence integration.
- **nxapi-znca-api** — [nxapi-znca-api](https://github.com/samuelthomas2774/nxapi-znca-api) provides the Nintendo Switch Online request attestation and Coral encryption service used by CaptureDock.

---

## ⚖️ Legal Disclaimer

CaptureDock is an independent, unofficial open-source project. Nintendo Switch and Nintendo Switch Online are registered trademarks of Nintendo Co., Ltd. and Nintendo of America Inc. All game titles, screenshots, and artwork remain the property of their respective owners.

---

## 📄 License

Licensed under the [MIT License](LICENSE). Third-party runtime notices are listed in [`THIRD-PARTY-NOTICES.txt`](THIRD-PARTY-NOTICES.txt).
