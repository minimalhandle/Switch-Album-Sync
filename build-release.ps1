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

$root = $PSScriptRoot
$cmake = Join-Path $root 'build/cmake/bin/cmake.exe'
if (-not (Test-Path -LiteralPath $cmake -PathType Leaf)) {
    $cmake = (Get-Command cmake.exe -CommandType Application).Source
}

$build = Join-Path $root 'build/release'
& $cmake -S $root -B $build -A x64 -DNSO_ENABLE_DISCORD=ON -DNSO_STATIC_RUNTIME=ON -DNSO_BUILD_TESTS=OFF
if ($LASTEXITCODE -ne 0) { throw "Release configuration failed ($LASTEXITCODE)." }

& $cmake --build $build --config Release --parallel
if ($LASTEXITCODE -ne 0) { throw "Release build failed ($LASTEXITCODE)." }

$dist = Join-Path $root 'dist'
[System.IO.Directory]::CreateDirectory($dist) | Out-Null
$release = Join-Path $dist 'Switch Album Sync.exe'
$id = [guid]::NewGuid().ToString('N')
$staged = Join-Path $dist "Switch Album Sync-$id.exe"
$testTemp = Join-Path $build "self-test-$id"
$previousTemp = $env:TEMP
$previousTmp = $env:TMP

try {
    Copy-Item -LiteralPath (Join-Path $build 'Release/Switch Album Sync.exe') -Destination $staged
    [System.IO.Directory]::CreateDirectory($testTemp) | Out-Null
    try {
        $env:TEMP = $testTemp
        $env:TMP = $testTemp
        $test = Start-Process -FilePath $staged -ArgumentList '--self-test' -WindowStyle Hidden -Wait -PassThru
    } finally {
        $env:TEMP = $previousTemp
        $env:TMP = $previousTmp
    }
    if ($test.ExitCode -ne 0) { throw "Release self-test failed ($($test.ExitCode))." }

    # Replace atomically without leaving a ReplaceFile backup in dist.
    if (-not [SwitchAlbumSyncRelease]::MoveFileEx($staged, $release, 0x9)) {
        $code = [Runtime.InteropServices.Marshal]::GetLastWin32Error()
        throw "Could not publish $release (Win32 error $code). Exit Switch Album Sync from the tray and try again."
    }
    [SwitchAlbumSyncRelease]::SHChangeNotify(0x2000, 0x1005, $release, [IntPtr]::Zero)
    Write-Output "Verified release: $release"
} finally {
    if ([System.IO.File]::Exists($staged)) { Remove-Item -LiteralPath $staged -Force }
    if (Test-Path -LiteralPath $testTemp -PathType Container) {
        $resolved = (Resolve-Path -LiteralPath $testTemp).Path
        if ($resolved -eq $testTemp) { Remove-Item -LiteralPath $resolved -Recurse -Force }
    }
}
