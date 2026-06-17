<#
.SYNOPSIS
  Configure, build and package Argus into an NSIS installer on Windows.

.DESCRIPTION
  Builds the CLI + GUI (Release), stages the executables, bundled fonts and the
  runtime DLLs, then runs makensis to produce dist\Argus-<version>-setup.exe.

  Requirements on the build agent:
    * Visual Studio 2019+ (MSVC) and CMake >= 3.16
    * libsndfile, e.g. via vcpkg; pass its toolchain file in $env:CMAKE_TOOLCHAIN_FILE
    * NSIS (makensis) on PATH

.EXAMPLE
  $env:CMAKE_TOOLCHAIN_FILE = "C:\vcpkg\scripts\buildsystems\vcpkg.cmake"
  powershell -ExecutionPolicy Bypass -File packaging\windows\build-windows.ps1
#>
param(
  [string]$Version = ""
)
$ErrorActionPreference = "Stop"
$Root = (Resolve-Path "$PSScriptRoot\..\..").Path
Set-Location $Root

if (-not $Version) {
  try { $Version = (git describe --tags --always) -replace '^v','' } catch { $Version = "1.0.0" }
}

# Resolve a vcpkg toolchain (for libsndfile). Prefer an explicit CMAKE_TOOLCHAIN_FILE,
# otherwise derive it from the runner's VCPKG_INSTALLATION_ROOT / VCPKG_ROOT.
if (-not $env:CMAKE_TOOLCHAIN_FILE -or -not (Test-Path $env:CMAKE_TOOLCHAIN_FILE)) {
  foreach ($vcpkgRoot in @($env:VCPKG_INSTALLATION_ROOT, $env:VCPKG_ROOT, "C:\vcpkg")) {
    if ($vcpkgRoot) {
      $candidate = Join-Path $vcpkgRoot "scripts\buildsystems\vcpkg.cmake"
      if (Test-Path $candidate) { $env:CMAKE_TOOLCHAIN_FILE = $candidate; break }
    }
  }
}

$cmakeArgs = @("-S", ".", "-B", "build", "-DCMAKE_BUILD_TYPE=Release", "-DARGUS_BUILD_GUI=ON")
if ($env:CMAKE_TOOLCHAIN_FILE) {
  Write-Host "Using vcpkg toolchain: $($env:CMAKE_TOOLCHAIN_FILE)"
  $cmakeArgs += "-DCMAKE_TOOLCHAIN_FILE=$($env:CMAKE_TOOLCHAIN_FILE)"
  $cmakeArgs += "-DVCPKG_TARGET_TRIPLET=x64-windows"
}
cmake @cmakeArgs
cmake --build build --config Release -j

# Locate built executables (multi-config puts them under Release\).
function Find-Exe($name) {
  $hits = Get-ChildItem -Path build -Recurse -Filter $name -ErrorAction SilentlyContinue
  if (-not $hits) { throw "could not find $name under build\" }
  return $hits[0].FullName
}
$gui = Find-Exe "Argus.exe"
$cli = Find-Exe "argus.exe"

$stage = Join-Path $Root "packaging\windows\stage"
Remove-Item -Recurse -Force $stage -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force -Path "$stage\resources" | Out-Null

Copy-Item $gui "$stage\Argus.exe"
Copy-Item $cli "$stage\argus.exe"
Copy-Item "$Root\src\ui\resources\JetBrainsMono-Regular.ttf" "$stage\resources\"
Copy-Item "$Root\src\ui\resources\JetBrainsMono-Bold.ttf" "$stage\resources\"

# Bundle runtime DLLs sitting beside the GUI exe (libsndfile + codecs, etc.).
Get-ChildItem -Path (Split-Path $gui) -Filter *.dll -ErrorAction SilentlyContinue |
  ForEach-Object { Copy-Item $_.FullName "$stage\" }

$dist = Join-Path $Root "dist"
New-Item -ItemType Directory -Force -Path $dist | Out-Null
$out = Join-Path $dist "Argus-$Version-setup.exe"

# Resolve makensis (choco installs it but may not refresh PATH for this step).
$makensis = "makensis"
if (-not (Get-Command makensis -ErrorAction SilentlyContinue)) {
  $candidates = @(
    "C:\Program Files (x86)\NSIS\makensis.exe",
    "C:\Program Files\NSIS\makensis.exe",
    (Join-Path $env:ProgramData "chocolatey\bin\makensis.exe")
  )
  foreach ($c in $candidates) { if (Test-Path $c) { $makensis = $c; break } }
}
Write-Host "Using makensis: $makensis"
& $makensis "/DVERSION=$Version" "/DSTAGE=$stage" "/DOUTFILE=$out" "$Root\packaging\windows\argus.nsi"
if ($LASTEXITCODE -ne 0) { throw "makensis failed with exit code $LASTEXITCODE" }
Write-Host "wrote $out"
