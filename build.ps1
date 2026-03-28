#!/usr/bin/env pwsh
param(
    [ValidateSet("Release", "Debug")]
    [string]$Config = "Release",
    [switch]$Clean
)

$ErrorActionPreference = "Stop"

if ($Clean) {
    Write-Host "Cleaning build directory..." -ForegroundColor Yellow
    if (Test-Path build) { Remove-Item -Recurse -Force build }
}

Write-Host "Configuring..." -ForegroundColor Cyan
cmake -S . -B build
if ($LASTEXITCODE) { exit $LASTEXITCODE }

Write-Host "Building ($Config)..." -ForegroundColor Cyan
cmake --build build --config $Config
if ($LASTEXITCODE) { exit $LASTEXITCODE }

Write-Host "Build succeeded." -ForegroundColor Green
