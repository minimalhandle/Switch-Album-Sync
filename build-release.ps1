# Builds, tests and publishes Switch Album Sync to dist.
#   .\build-release.ps1        Release build with the release nxapi-auth client.
#   .\build-release.ps1 -Test  Test build with the development client, published
#                              as "Switch Album Sync (test).exe" next to the release.
param([switch]$Test)

$ErrorActionPreference = 'Stop'

if (-not ('SwitchAlbumSyncRelease' -as [type])) {
    Add-Type @'
using System.Runtime.InteropServices;

public static class SwitchAlbumSyncRelease {
    [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    public static extern bool MoveFileEx(string source, string destination, int flags);

    [DllImport("shell32.dll", CharSet = CharSet.Unicode)]
    public static extern void SHChangeNotify(int eventId, uint flags, string path, System.IntPtr reserved);
}
'@
}

# nxapi-znca-api's public API terms require separate clients for development
# and release builds. Both are public clients registered at
# https://nxapi-auth.fancy.org.uk/oauth/clients.
$releaseClientId = 'aUCnVoLRU9sTIfyIjW7Yog'
$developmentClientId = '9F_ul8eygxiLn9N0uCaPeA'
# X-znca-Client-Version (fixed, never set at runtime). nxapi rejects 3.0.x for
# current Nintendo Switch Online versions and doesn't yet issue per-client values.
# Test builds use the compatibility value nxapi's own client publishes.
$testZncaClientVersion = 'd8fAZDPzwimzQ7c6'
# Set this once nxapi confirms a value for the release client. Releases are
# blocked until then so a build that can't sync is never published.
$releaseZncaClientVersion = ''

$root = $PSScriptRoot
$cmake = Join-Path $root 'build\cmake\bin\cmake.exe'
$ctest = Join-Path $root 'build\cmake\bin\ctest.exe'
if (-not (Test-Path -LiteralPath $cmake -PathType Leaf)) {
    $cmake = (Get-Command cmake.exe -CommandType Application).Source
    $ctest = (Get-Command ctest.exe -CommandType Application).Source
}

$kind = if ($Test) { 'test' } else { 'release' }
$clientId = if ($Test) { $developmentClientId } else { $releaseClientId }
$outputName = if ($Test) { 'Switch Album Sync (test).exe' } else { 'Switch Album Sync.exe' }
$zncaClientVersion = if ($Test) { $testZncaClientVersion } else { $releaseZncaClientVersion }
if (-not $zncaClientVersion) {
    throw 'No X-znca-Client-Version is set for the release client. Ask nxapi which value to use, set $releaseZncaClientVersion in this script, then build again. Use -Test for a test build.'
}

$build = Join-Path $root "build\$kind"
& $cmake -S $root -B $build -A x64 -DNSO_ENABLE_DISCORD=ON -DNSO_STATIC_RUNTIME=ON -DNSO_BUILD_TESTS=ON `
    "-DNSO_NXAPI_AUTH_CLIENT_ID=$clientId" "-DNSO_ZNCA_CLIENT_VERSION=$zncaClientVersion"
if ($LASTEXITCODE -ne 0) { throw "Configuration failed ($LASTEXITCODE)." }

& $cmake --build $build --config Release --parallel
if ($LASTEXITCODE -ne 0) { throw "Build failed ($LASTEXITCODE)." }

& $ctest --test-dir $build -C Release --output-on-failure
if ($LASTEXITCODE -ne 0) { throw "Tests failed ($LASTEXITCODE)." }

$dist = Join-Path $root 'dist'
[System.IO.Directory]::CreateDirectory($dist) | Out-Null
$published = Join-Path $dist $outputName
$id = [guid]::NewGuid().ToString('N')
$staged = Join-Path $dist "Switch Album Sync-$id.exe"
$testTemp = [System.IO.Path]::GetFullPath((Join-Path $build "self-test-$id"))
$previousTemp = $env:TEMP
$previousTmp = $env:TMP

try {
    Copy-Item -LiteralPath (Join-Path $build 'Release\Switch Album Sync.exe') -Destination $staged
    [System.IO.Directory]::CreateDirectory($testTemp) | Out-Null
    try {
        $env:TEMP = $testTemp
        $env:TMP = $testTemp
        $selfTest = Start-Process -FilePath $staged -ArgumentList '--self-test' -WindowStyle Hidden -Wait -PassThru
    } finally {
        $env:TEMP = $previousTemp
        $env:TMP = $previousTmp
    }
    if ($selfTest.ExitCode -ne 0) { throw "Self-test failed ($($selfTest.ExitCode))." }

    # Replace atomically without leaving a ReplaceFile backup in dist.
    if (-not [SwitchAlbumSyncRelease]::MoveFileEx($staged, $published, 0x9)) {
        $code = [Runtime.InteropServices.Marshal]::GetLastWin32Error()
        throw "Could not publish $published (Win32 error $code). Exit Switch Album Sync from the tray and try again."
    }
    [SwitchAlbumSyncRelease]::SHChangeNotify(0x2000, 0x1005, $published, [IntPtr]::Zero)
    Write-Output "Verified $kind build: $published"
} finally {
    if ([System.IO.File]::Exists($staged)) { Remove-Item -LiteralPath $staged -Force }
    if (Test-Path -LiteralPath $testTemp -PathType Container) {
        Remove-Item -LiteralPath $testTemp -Recurse -Force
    }
}
