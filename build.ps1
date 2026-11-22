# Build mtkradio.exe on Windows (MSVC / clang-cl)
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $MyInvocation.MyCommand.Path
$build = Join-Path $root 'build'

cmake -S $root -B $build -G "Ninja" -DCMAKE_BUILD_TYPE=Release
cmake --build $build --target mtkradio

$fwSrc = Join-Path $root 'firmware'
$fwDst = Join-Path $build 'firmware'
if (Test-Path $fwSrc) {
  New-Item -ItemType Directory -Force -Path $fwDst | Out-Null
  Copy-Item -Force (Join-Path $fwSrc '*.bin') $fwDst
}

Write-Host "Built: $(Join-Path $build 'mtkradio.exe')" -ForegroundColor Green
