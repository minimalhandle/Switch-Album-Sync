# Switch Album Sync

Keep screenshots and videos from your Nintendo Switch or Switch 2 in one Windows library. Switch Album Sync copies new captures from your Nintendo Switch Online album to your PC, imports older ones over USB or microSD, sorts them by game, and lets you browse, tag and share them.

Switch Album Sync is an unofficial, independent app. It isn't made, endorsed or supported by Nintendo.

## Get started

1. Download **Switch Album Sync.exe** from the [latest release](https://github.com/minimalhandle/Switch-Album-Sync/releases/latest). It's a single portable file for Windows 10 or later (x64). The exe isn't code-signed, so Windows may show a SmartScreen warning the first time.
2. Choose where to keep your captures. The default is `Pictures\Switch Album Sync`.
3. To sync from Nintendo Switch Online, click **Sign in…**, read the notice about the third-party service the app uses, and sign in on Nintendo's website. Then use **Sync now**, or turn on **Auto-sync**.

You don't need to sign in to import from USB or microSD, watch a folder, or use the library.

## What it does

### Sync from Nintendo Switch Online

- **Sync now** asks Nintendo for your album and downloads captures you don't have yet into a folder per game. Folder names match existing folders, including localized names and Nintendo Switch 2 Editions, so a game's captures stay together.
- Keeps each capture's original date and time on the file.
- Downloads each capture once. Deleting, renaming or moving a file in your album won't bring it back on the next sync.
- If a capture you already copied from the console looks like the same shot, the download goes to **Duplicate review** so you can keep one or both.
- **Auto-sync** is off until you turn it on. It then syncs every 60 minutes (never more often than every 15) while the app runs, including from the notification area. It only runs while you're signed in.
- **More → Auto-sync options** skips scheduled syncs while battery saver is on, on metered connections, or during quiet hours (23:00 to 07:00). A skipped sync just waits for the next one; **Sync now** always works.
- **Sync history** lists what each sync downloaded, skipped or couldn't save. **Retry failed** tries the failed items again. If a download fails because the connection dropped, the app tries that capture again by itself up to three times (after 1, 2 and 4 minutes). If Nintendo answers with an error, it waits for you instead.
- Nintendo's album keeps captures for a limited time. If a capture that couldn't be saved leaves Nintendo's album within 3 days, the sync status and notification say so.
- **Recent captures** on the dashboard shows the newest captures. **Capture actions** copies the latest one or shows a selected one in its folder.
- **Notifications** (in the notification-area menu) tell you when a sync or import finishes; click one to see the new captures.

### Import and organise

- **Import USB / SD…** copies captures from the console's **Copy to PC over USB** mode or a microSD card, keeping their timestamps. Before anything is copied, you can review suggested game folders and possible duplicates.
- **More → Watch a folder** imports captures from another folder while the app runs. It waits for files to finish writing and leaves the originals where they are.
- **Open library** previews screenshots and plays videos. You can search, filter by game, date and type, show only what's new since your last visit, sort by any column, tag captures, mark favourites, assign a capture to a different game, and save filters as smart collections.
- **Text in screenshots** (optional) lets search find words shown in your screenshots. Windows' built-in text recognition reads them on your PC; nothing is uploaded.
- **Storage** shows how much space each game's captures use. **More → Storage cleanup** lists your largest captures so you can free up space.
- **More → Find duplicates** finds identical copies anywhere in the album, and optionally screenshots that look the same. You choose what to remove, and removed files go to the Recycle Bin.
- **More → Back up to a folder** keeps a second copy of your album, for example on another drive or in a OneDrive folder. New and changed captures are copied after each sync or import; nothing in the backup is ever deleted.
- **More → Export settings / Import settings** moves your preferences, tags, favourites, assigned games and smart collections to another PC. Sign-in details aren't included.

### Share

- **Copy** puts selected captures on the clipboard as files, ready to paste into a chat or folder.
- **Quick Share** exports a folder, a ZIP archive, or a PNG grid of up to 48 screenshots.
- **More → Highlights grid** makes a PNG grid of a month or year: favourites first, then a spread across the period.
- **Edit…** in the library trims a video into an MP4 clip with sound or a looping GIF, or opens a screenshot editor to crop, blur (pixelate) part of it, such as a username, or add text. Edits are saved as new files; originals never change.

### Discord Rich Presence (optional)

Rich Presence shows the game you're playing on your Discord profile. It's off until you turn it on, and the app explains what it shares before it starts. Under **Settings**, you choose whether to show your Nintendo Switch username, profile picture and play time, and which game details to add:

- **Splatoon 3** and **Splatoon 2**: your name, title, weapon and rank.
- **Animal Crossing: New Horizons**: your island and resident name.
- **Zelda Notes live location**: where you are on the map in *Ocarina of Time* on Nintendo Switch 2, and in *Breath of the Wild* and *Tears of the Kingdom* (Nintendo Switch 2 Editions only, as Zelda Notes requires).

How often it checks Nintendo:

- It only checks while the Discord desktop app is running.
- Every minute while you're playing. After a few checks find you offline, every 5 minutes, and after about an hour offline, every 15 minutes. After a failed check, it waits 5 minutes before the next one.
- After 8 hours offline, it stops checking until someone uses the PC again, so a PC left on overnight doesn't keep contacting Nintendo.
- Game details are looked up once per game session. If that lookup fails, it isn't repeated automatically; **Refresh presence** tries again.
- Zelda Notes keeps one live connection open while you play, as the Zelda Notes map does. If the connection drops, the app reconnects with increasing waits (up to 30 seconds). If Nintendo answers with an error, live location stops until you start another game or use **Refresh presence**.

### Make it yours

- **More → Appearance** follows Windows or uses light or dark mode. The app is per-monitor DPI aware, respects Windows high-contrast settings, and uses standard Windows controls that screen readers can name.
- **Start with Windows** (notification-area menu) starts the app in the notification area when you sign in to Windows.
- **HTTP proxy…** (notification-area menu) sends the app's internet traffic through a proxy you choose. Requests stay encrypted (HTTPS) end to end, but the proxy can see which services the app contacts.
- **More → Copy diagnostics** copies a short report for troubleshooting: app version, sign-in and auto-sync state, the latest status and error, and how many requests the app sent today to Nintendo and to nxapi. It never includes tokens or account details.

## How sign-in works and what is shared

Nintendo Switch Online sign-in uses the same flow as Nintendo's app. You sign in on Nintendo's website, and Nintendo hands a sign-in token back to Switch Album Sync. If your browser can't hand it back automatically, the app asks you to copy the link from Nintendo's page and use **Paste sign-in link…**. That long-term token is stored in Windows Credential Manager and is only ever sent to Nintendo.

Requests to the Nintendo Switch Online service must be signed and encrypted by Nintendo's app. Switch Album Sync uses [nxapi-znca-api](https://github.com/samuelthomas2774/nxapi-znca-api), a third-party service from the nxapi project, for this step ([help for users of apps that use it](https://github.com/samuelthomas2774/nxapi-znca-api/blob/docs/docs/end-user-help.md)). While you use Nintendo Switch Online features, it receives:

- your short-lived Nintendo Account id_token and the account details it carries (Nintendo Account ID, birthday, country and language);
- your short-lived Nintendo Switch Online (Coral) access token;
- every Nintendo Switch Online request and response the app makes.

The app shows this before you sign in and asks you to agree. If you're already signed in from an older version, it asks once on startup instead. Choose **Sign out** at any time to delete your saved sign-in and every cached token.

| Service | When | What it receives |
| --- | --- | --- |
| Nintendo Accounts and Nintendo Switch Online | Sign-in, syncing, Rich Presence | Your sign-in and album requests |
| Nintendo's media servers | Downloading captures | Requests for your captures |
| Nintendo game services (SplatNet 3, SplatNet 2, NookLink, Zelda Notes) | Only when Rich Presence details are on for that game | Game service tokens issued for your account; their requests go straight to Nintendo |
| nxapi-znca-api and nxapi-auth (`fancy.org.uk`) | Every Nintendo Switch Online request, including signing in to a game service | As listed above |
| Discord desktop app on this PC | Only while Rich Presence is on | Your current game and the details you choose to show |
| Discord, loading Rich Presence images | Only while Rich Presence is on | Image links on Nintendo's servers, splatoon3.ink and Zelda Wiki |
| Your HTTP proxy | Only if you set one | Encrypted traffic to the services above |

Nothing is sent anywhere else. Library, editing, duplicate, backup and text-search features work entirely on your PC. The app has no accounts, analytics or ads, and it's free.

## What this means for your Nintendo account

- **Nintendo sees the Nintendo Switch Online app.** Switch Album Sync signs in and makes requests the way Nintendo's app does, so Nintendo sees them as coming from the Nintendo Switch Online app. Your account's sign-in records may show those sign-ins.
- **It's unofficial.** Nintendo doesn't support or approve third-party apps using its services. Using one is your choice and at your own risk; Nintendo could change or restrict access at any time.
- **What it does with your account:** it reads your album and downloads captures. With Rich Presence on, it reads your online status and, for the games you enable, reads your game details from that game's service. Zelda Notes live location opens the same live map session the Zelda Notes page uses. The app doesn't change or delete anything in your album, your games or your account settings.
- **Captures stay in Nintendo's album.** Downloading copies them; Nintendo's album removes them on its own schedule as usual.
- **Your password never reaches the app.** You sign in on Nintendo's website. The app stores the resulting sign-in token in Windows Credential Manager. **Sign out** deletes it and every cached token.

## Using Nintendo's services responsibly

Switch Album Sync follows nxapi-znca-api's [public API terms](https://github.com/samuelthomas2774/nxapi-znca-api/blob/docs/docs/public-api-terms.md) and keeps its use of Nintendo's services modest:

- Automatic activity (auto-sync and Rich Presence) is off until you turn it on.
- Requests to nxapi run one at a time and stay within its published rate limits.
- Each Nintendo Switch Online request takes two nxapi requests: one to encrypt it and one to decrypt Nintendo's reply. Signing in and getting a game-service token each take one nxapi request to sign and encrypt the request, plus one to decrypt the reply.
- A sync where nothing is new takes one Nintendo Switch Online request (two nxapi requests), plus an occasional nxapi-auth token or supported-version check. The sign-in is renewed about every two hours, when it expires.
- Tokens are reused for as long as they're valid, including across restarts. nxapi-auth tokens are kept in memory only.
- When nxapi asks the app to wait, it waits, even across restarts.
- After an error response, the app doesn't automatically repeat the request, to Nintendo or to nxapi. Only connections that drop before any reply are retried automatically.

## Where your data lives

- **Captures:** your chosen album folder.
- **Settings, sync history, labels and collections:** `%APPDATA%\Switch Album Sync`.
- **Sign-in and cached Nintendo tokens:** Windows Credential Manager, under entries starting with `Switch Album Sync/`.

## Build from source

Use CMake 3.20 or newer with Visual Studio 2022 (C++20). To build and run the tests without Discord Rich Presence:

```powershell
cmake -S . -B build/dev -A x64 -DNSO_ENABLE_DISCORD=OFF -DNSO_BUILD_TESTS=ON
cmake --build build/dev --config Release --parallel
ctest --test-dir build/dev -C Release --output-on-failure
```

For a build with Rich Presence, extract the Windows C++ Discord Social SDK to `third_party/discord_social_sdk` and run `.\build-release.ps1`. The script builds, runs the tests and a self-test, and writes the executable to `dist`. `.\build-release.ps1 -Test` makes a test build, `dist\Switch Album Sync (test).exe`.

nxapi-znca-api's terms require every client to be registered, with a separate client for development. If you build your own copy, register a public client at [nxapi-auth](https://nxapi-auth.fancy.org.uk/oauth/clients) with the `ca:gf ca:er ca:dr` scopes, complete its description and contact details, and pass `-DNSO_NXAPI_AUTH_CLIENT_ID=<your client ID>` when you configure. The `X-znca-Client-Version` value (`NSO_ZNCA_CLIENT_VERSION`) must be one nxapi's maintainer has confirmed for your client.

## Credits and licence

Switch Album Sync started as a fork of [NSO Album Sync](https://github.com/Dycool/NSO-Album-Sync) and has since been largely rewritten. Thanks to:

- [Diogo Enes (Dycool)](https://github.com/Dycool) for creating NSO Album Sync.
- [Bentheminernz](https://github.com/Bentheminernz) for the Zelda Notes Rich Presence reference work.
- The [nxapi project](https://gitlab.fancy.org.uk/samuel/nxapi) for nxapi-znca-api, nxapi-auth and the [app version data](https://gitlab.fancy.org.uk/samuel/nintendo-app-versions) used for SplatNet 3.
- [Zelda Wiki](https://zeldawiki.wiki) for the location images in Zelda Notes Rich Presence.
- [splatoon3.ink](https://splatoon3.ink) for the Splatoon 3 images it mirrors from SplatNet 3.

Released under the [MIT licence](LICENSE). See the [third-party notices](THIRD-PARTY-NOTICES.txt) for bundled components. Nintendo Switch, Nintendo Switch Online and game names are trademarks of Nintendo; Discord is a trademark of Discord Inc.
