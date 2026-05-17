[CmdletBinding()]
param(
    [string]$FFmpegRef = "n8.1",
    [string]$Dav1dRef = "1.5.3",
    [string]$BuildRoot = "",
    [string]$DistRoot = "",
    [switch]$Clean,
    [switch]$SkipPackage
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

$RepoRoot = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
if ([string]::IsNullOrWhiteSpace($BuildRoot)) {
    $BuildRoot = Join-Path $RepoRoot ".build"
}
if ([string]::IsNullOrWhiteSpace($DistRoot)) {
    $DistRoot = Join-Path $RepoRoot "dist"
}

$PackageName = "voidplayer-ffmpeg-windows-x64-$FFmpegRef"
$SourceRoot = Join-Path $BuildRoot "sources"
$WorkRoot = Join-Path $BuildRoot "work"
$Dav1dSource = Join-Path $SourceRoot "dav1d"
$FFmpegSource = Join-Path $SourceRoot "ffmpeg"
$Dav1dBuild = Join-Path $WorkRoot "dav1d-msvc"
$Dav1dInstall = Join-Path $WorkRoot "dav1d-install"
$FFmpegBuild = Join-Path $WorkRoot "ffmpeg-msvc"
$PackageRoot = Join-Path $DistRoot $PackageName

function Invoke-Step {
    param(
        [Parameter(Mandatory = $true)]
        [string]$FilePath,
        [Parameter(ValueFromRemainingArguments = $true)]
        [string[]]$Arguments
    )

    Write-Host "==> $FilePath $($Arguments -join ' ')"
    & $FilePath @Arguments
    if ($LASTEXITCODE -ne 0) {
        throw "Command failed with exit code $LASTEXITCODE`: $FilePath"
    }
}

function Find-Bash {
    $candidates = @(
        "C:\msys64\usr\bin\bash.exe",
        "C:\tools\msys64\usr\bin\bash.exe"
    )
    foreach ($candidate in $candidates) {
        if (Test-Path $candidate) {
            return $candidate
        }
    }

    $cmd = Get-Command bash.exe -ErrorAction SilentlyContinue
    if ($cmd) {
        return $cmd.Source
    }

    throw "MSYS2 bash.exe was not found. Install MSYS2 or add bash.exe to PATH."
}

function Convert-ToMsysPath {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Path
    )

    $fullPath = [System.IO.Path]::GetFullPath($Path)
    $escaped = $fullPath.Replace("'", "'\''")
    return (& $script:BashPath -lc "cygpath -u '$escaped'").Trim()
}

function Quote-Bash {
    param([string]$Value)
    return "'" + $Value.Replace("'", "'\''") + "'"
}

function Add-EnableList {
    param(
        [System.Collections.Generic.List[string]]$Args,
        [string]$Kind,
        [string[]]$Names
    )

    foreach ($name in $Names) {
        $Args.Add("--enable-$Kind=$name")
    }
}

function Assert-NoForbiddenRuntimeDependency {
    param(
        [Parameter(Mandatory = $true)]
        [string]$BinDir
    )

    $forbiddenPatterns = @(
        "^msys-2\.0\.dll$",
        "^libwinpthread-1\.dll$",
        "^libgcc_s.*\.dll$",
        "^libstdc\+\+-6\.dll$",
        "^libiconv-2\.dll$",
        "^libintl-8\.dll$"
    )

    if (-not (Test-Path $BinDir)) {
        throw "Runtime bin directory was not produced: $BinDir"
    }

    $packagedForbidden = Get-ChildItem $BinDir -File -Filter "*.dll" |
        Where-Object {
            $dllName = $_.Name
            $forbiddenPatterns | Where-Object { $dllName -match $_ }
        }
    if ($packagedForbidden) {
        $names = ($packagedForbidden | Select-Object -ExpandProperty Name) -join ", "
        throw "Package contains forbidden MSYS2/MinGW runtime DLLs: $names"
    }

    $dumpbin = Get-Command dumpbin.exe -ErrorAction SilentlyContinue
    if (-not $dumpbin) {
        throw "dumpbin.exe was not found. Run from a Visual Studio Developer PowerShell."
    }

    $badDependents = @()
    foreach ($dll in Get-ChildItem $BinDir -File -Filter "*.dll") {
        $output = & $dumpbin.Source /dependents $dll.FullName 2>&1
        if ($LASTEXITCODE -ne 0) {
            throw "dumpbin failed for $($dll.FullName)"
        }
        foreach ($line in $output) {
            $dependent = $line.ToString().Trim()
            foreach ($pattern in $forbiddenPatterns) {
                if ($dependent -match $pattern) {
                    $badDependents += "$($dll.Name) -> $dependent"
                }
            }
        }
    }

    if ($badDependents.Count -gt 0) {
        throw "Forbidden MSYS2/MinGW runtime dependency detected:`n$($badDependents -join "`n")"
    }
}

if ($Clean -and (Test-Path $BuildRoot)) {
    Remove-Item -LiteralPath $BuildRoot -Recurse -Force
}

New-Item -ItemType Directory -Force -Path $SourceRoot, $WorkRoot, $DistRoot | Out-Null
$script:BashPath = Find-Bash
$MsysUsrBin = Split-Path -Parent $script:BashPath
if ($env:Path -notlike "*$MsysUsrBin*") {
    $env:Path = "$env:Path;$MsysUsrBin"
}

if (-not (Get-Command meson -ErrorAction SilentlyContinue)) {
    throw "meson was not found. Run: python -m pip install meson ninja"
}
if (-not (Get-Command ninja -ErrorAction SilentlyContinue)) {
    throw "ninja was not found. Run: python -m pip install meson ninja"
}
if (-not (Get-Command cl.exe -ErrorAction SilentlyContinue)) {
    throw "cl.exe was not found. Run from a Visual Studio Developer PowerShell."
}
if (-not (Get-Command link.exe -ErrorAction SilentlyContinue)) {
    throw "link.exe was not found. Run from a Visual Studio Developer PowerShell."
}
if (-not (Get-Command dumpbin.exe -ErrorAction SilentlyContinue)) {
    throw "dumpbin.exe was not found. Run from a Visual Studio Developer PowerShell."
}
if (-not (Get-Command nasm.exe -ErrorAction SilentlyContinue) -and -not (Get-Command nasm -ErrorAction SilentlyContinue)) {
    throw "nasm was not found. Install NASM or install the MSYS2 nasm package."
}

if (-not (Test-Path $Dav1dSource)) {
    Invoke-Step git clone --depth 1 --branch $Dav1dRef https://code.videolan.org/videolan/dav1d.git $Dav1dSource
}
if (-not (Test-Path $FFmpegSource)) {
    Invoke-Step git clone --depth 1 --branch $FFmpegRef https://github.com/FFmpeg/FFmpeg.git $FFmpegSource
}

Invoke-Step meson setup $Dav1dBuild $Dav1dSource `
    --prefix $Dav1dInstall `
    --libdir lib `
    --buildtype release `
    --default-library static `
    -Denable_tools=false `
    -Denable_tests=false
Invoke-Step meson compile -C $Dav1dBuild
Invoke-Step meson install -C $Dav1dBuild

$ffmpegArgs = [System.Collections.Generic.List[string]]::new()
@(
    "--prefix=$(Convert-ToMsysPath $PackageRoot)",
    "--toolchain=msvc",
    "--target-os=win64",
    "--arch=x86_64",
    "--enable-shared",
    "--disable-static",
    "--disable-autodetect",
    "--disable-everything",
    "--disable-programs",
    "--disable-doc",
    "--disable-debug",
    "--enable-runtime-cpudetect",
    "--enable-avcodec",
    "--enable-avformat",
    "--enable-avutil",
    "--enable-swresample",
    "--enable-libdav1d",
    "--enable-d3d11va",
    "--enable-dxva2",
    "--enable-hwaccels",
    "--pkg-config-flags=--static"
) | ForEach-Object { $ffmpegArgs.Add($_) }

Add-EnableList $ffmpegArgs "protocol" @("cache", "concat", "file", "pipe")
Add-EnableList $ffmpegArgs "demuxer" @(
    "aac", "asf", "avi", "concat", "flac", "flv", "h264", "hevc", "ivf",
    "live_flv", "m4v", "matroska", "mov", "mp3", "mpegps", "mpegts",
    "mpegvideo", "ogg", "wav"
)
Add-EnableList $ffmpegArgs "decoder" @(
    "aac", "av1", "ffv1", "flac", "h264", "hevc", "libdav1d", "mjpeg",
    "mp3", "mpeg1video", "mpeg2video", "mpeg4", "opus", "pcm_alaw",
    "pcm_f32be", "pcm_f32le", "pcm_mulaw", "pcm_s16be", "pcm_s16le",
    "pcm_s24be", "pcm_s24le", "pcm_s32be", "pcm_s32le", "prores",
    "vorbis", "vp8", "vp9", "wmav1", "wmav2", "wmapro"
)
Add-EnableList $ffmpegArgs "parser" @(
    "aac", "aac_latm", "av1", "flac", "h264", "hevc", "mjpeg",
    "mpegaudio", "mpeg4video", "mpegvideo", "opus", "vorbis", "vp8", "vp9"
)
Add-EnableList $ffmpegArgs "bsf" @(
    "av1_frame_merge", "av1_frame_split", "extract_extradata",
    "h264_mp4toannexb", "hevc_mp4toannexb", "null"
)

$ffmpegBuildMsys = Convert-ToMsysPath $FFmpegBuild
$ffmpegSourceMsys = Convert-ToMsysPath $FFmpegSource
$dav1dPcMsys = Convert-ToMsysPath (Join-Path $Dav1dInstall "lib\pkgconfig")
$configureLine = ($ffmpegArgs | ForEach-Object { Quote-Bash $_ }) -join " "
$bashScript = @"
set -euo pipefail
export MSYSTEM=UCRT64
export CHERE_INVOKING=1
mkdir -p $(Quote-Bash $ffmpegBuildMsys)
cd $(Quote-Bash $ffmpegBuildMsys)
export PKG_CONFIG_PATH=$(Quote-Bash $dav1dPcMsys)
export PATH="/usr/bin:`$PATH"
command -v cl.exe
command -v link.exe
command -v dumpbin.exe
command -v make
command -v nasm.exe || command -v nasm
$(Quote-Bash "$ffmpegSourceMsys/configure") $configureLine
make -j`$(nproc)
make install
"@

Write-Host "==> configuring and building FFmpeg $FFmpegRef"
& $script:BashPath -lc $bashScript
if ($LASTEXITCODE -ne 0) {
    throw "FFmpeg build failed with exit code $LASTEXITCODE"
}

Assert-NoForbiddenRuntimeDependency -BinDir (Join-Path $PackageRoot "bin")

$licenseRoot = Join-Path $PackageRoot "LICENSES"
New-Item -ItemType Directory -Force -Path $licenseRoot | Out-Null
Get-ChildItem $FFmpegSource -File -Filter "COPYING*" | ForEach-Object {
    Copy-Item $_.FullName -Destination (Join-Path $licenseRoot $_.Name) -Force
}
foreach ($name in @("LICENSE.md", "README.md")) {
    $path = Join-Path $FFmpegSource $name
    if (Test-Path $path) {
        Copy-Item $path -Destination (Join-Path $licenseRoot "FFmpeg-$name") -Force
    }
}
$dav1dLicense = Join-Path $Dav1dSource "COPYING"
if (Test-Path $dav1dLicense) {
    Copy-Item $dav1dLicense -Destination (Join-Path $licenseRoot "dav1d-COPYING") -Force
}

$manifest = [ordered]@{
    package = $PackageName
    target = "windows-x64-msvc"
    ffmpegRef = $FFmpegRef
    dav1dRef = $Dav1dRef
    builtAtUtc = (Get-Date).ToUniversalTime().ToString("yyyy-MM-ddTHH:mm:ssZ")
    libraries = @("avcodec", "avformat", "avutil", "swresample")
    av1SoftwareDecoder = "libdav1d"
    analysisFfmpegSubmodule = "not included"
    forbiddenRuntimeDependencyCheck = "dumpbin /dependents"
}
$manifest | ConvertTo-Json -Depth 4 | Set-Content -Path (Join-Path $PackageRoot "voidplayer-ffmpeg-manifest.json") -Encoding UTF8

@"
VoidPlayer FFmpeg runtime/dev package

FFmpeg: $FFmpegRef
dav1d: $Dav1dRef
Target: windows-x64-msvc

This package is intended for VoidPlayer/windows/libs/ffmpeg.
It contains avcodec, avformat, avutil, swresample, headers, MSVC import
libraries, runtime DLLs, and license material.

The instrumented analysis FFmpeg submodule is not included.
"@ | Set-Content -Path (Join-Path $PackageRoot "README.txt") -Encoding UTF8

if (-not $SkipPackage) {
    $zipPath = Join-Path $DistRoot "$PackageName.zip"
    if (Test-Path $zipPath) {
        Remove-Item -LiteralPath $zipPath -Force
    }
    Compress-Archive -Path (Join-Path $PackageRoot "*") -DestinationPath $zipPath -Force
    Write-Host "==> package: $zipPath"
}

Write-Host "==> done: $PackageRoot"
