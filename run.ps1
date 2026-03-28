#!/usr/bin/env pwsh
param(
    [ValidateSet("Release", "Debug")]
    [string]$Config = "Release"
)

$ErrorActionPreference = "Stop"

$exe = "build/$Config/forge-point.exe"
$buildDir = "build"

function Test-NeedsRebuild {
    if (-not (Test-Path $exe)) { return $true }

    $exeTime = (Get-Item $exe).LastWriteTime

    $sources = Get-ChildItem -Recurse -Include *.cpp,*.hpp,*.h,*.txt,CMakeLists.txt |
        Where-Object { $_.FullName -notmatch '\\build\\' -and $_.FullName -notmatch '\\_deps\\' }

    foreach ($src in $sources) {
        if ($src.LastWriteTime -gt $exeTime) { return $true }
    }
    return $false
}

if (-not (Test-Path $buildDir)) {
    Write-Host "No build directory found. Building from scratch..." -ForegroundColor Yellow
    cmake -S . -B build
    if ($LASTEXITCODE) { exit $LASTEXITCODE }
}

if (Test-NeedsRebuild) {
    Write-Host "Changes detected. Rebuilding..." -ForegroundColor Yellow
    cmake --build build --config $Config
    if ($LASTEXITCODE) { exit $LASTEXITCODE }
}

Write-Host "Launching forge-point ($Config)..." -ForegroundColor Green
& ".\$exe"
