# Switch Album Sync

Keep screenshots and videos from your Nintendo Switch or Switch 2 in one Windows library. Sync captures from Nintendo Switch Online, bring in older files over USB or microSD, and share your favourites from the same app.

## Get started

1. Download **Switch Album Sync.exe** from the [latest release](https://github.com/minimalhandle/Switch-Album-Sync/releases/latest). The portable build runs on Windows 10 or later (x64).
2. Open the app and choose where to keep your captures. The default is `Pictures\Switch Album Sync`. Captures in an older `Album` subfolder still appear in the library.
3. Sign in to sync from Nintendo Switch Online. Use **Sync now** for a manual check or enable **Auto-sync** to check periodically while the app runs in the system tray.

## Organise and share captures

- **Import USB / SD** copies selected captures while preserving their timestamps. On the console, use **Copy to PC over USB**, or select the microSD card in the Windows picker. Review suggested game folders and possible duplicates before importing.
- **Album tools → Watch a folder** imports captures from a separate folder while the app runs. It waits for files to finish writing and keeps the originals.
- **Open library** lets you preview captures, tag favourites, and save filters as smart collections. **Quick Share** exports a folder, a ZIP archive, or a PNG grid of up to 48 screenshots.
- **Album tools** also opens sync history, storage usage by game, and duplicate review.
- **Discord Rich Presence** is optional and starts off. Enable it from the dashboard if you want the Discord desktop client to show your current game.

## Nintendo sign-in

Cloud sync uses [nxapi-znca-api](https://github.com/samuelthomas2774/nxapi-znca-api). Before you sign in, the app explains which Nintendo account details, credentials, and service requests are sent to that service. Local imports, folder watching, and the capture library work without Nintendo sign-in.

## Build from source

Use CMake 3.20 or newer with a C++20 toolchain. To build and run the Windows feature tests without Discord Rich Presence:

```powershell
cmake -S . -B build/dev -A x64 -DNSO_ENABLE_DISCORD=OFF -DNSO_BUILD_TESTS=ON
cmake --build build/dev --config Release --parallel
ctest --test-dir build/dev -C Release --output-on-failure
```

For a release with Rich Presence, extract the Windows C++ Discord Social SDK to `third_party/discord_social_sdk` and run `.\build-release.ps1`. The script tests the packaged build and writes the executable to `dist`.

## Credits and licence

Switch Album Sync builds on [NSO Album Sync](https://github.com/Dycool/NSO-Album-Sync). Thanks to the three people behind the original app and its credited integrations:

- [Diogo Enes (Dycool)](https://github.com/Dycool) for creating NSO Album Sync.
- [Bentheminernz](https://github.com/Bentheminernz) for the Zelda Notes Rich Presence reference work.
- [samuelthomas2774](https://github.com/samuelthomas2774) for nxapi-znca-api, which handles Nintendo Switch Online request attestation and Coral encryption.

Switch Album Sync is an independent project. See the [MIT licence](LICENSE) and [third-party notices](THIRD-PARTY-NOTICES.txt).
