# run_tests.ps1 - Run Forge-Point tests
param(
    [switch]$Coverage,
    [string]$Filter,
    [switch]$Help
)

if ($Help) {
    Write-Host "Forge-Point Test Runner"
    Write-Host ""
    Write-Host "Usage:"
    Write-Host "  .\run_tests.ps1              Run all tests"
    Write-Host "  .\run_tests.ps1 -Coverage    Run tests with coverage"
    Write-Host "  .\run_tests.ps1 -Filter <pattern>  Run tests matching pattern"
    Write-Host "  .\run_tests.ps1 -Help        Show this help"
    Write-Host ""
    Write-Host "Examples:"
    Write-Host "  .\run_tests.ps1 -Filter '*Command*'"
    Write-Host "  .\run_tests.ps1 -Filter 'MenuNavigationTest.*'"
    exit 0
}

$BuildDir = "$PSScriptRoot\build"
$TestExe = "$BuildDir\Release\forge_point_tests.exe"

# Check if tests are built
if (-not (Test-Path $TestExe)) {
    Write-Host "Building tests..." -ForegroundColor Cyan
    Push-Location $BuildDir
    cmake --build . --config Release
    if ($LASTEXITCODE -ne 0) {
        Write-Host "Build failed!" -ForegroundColor Red
        Pop-Location
        exit 1
    }
    Pop-Location
}

Write-Host "Running Forge-Point Tests" -ForegroundColor Cyan
Write-Host "=========================" -ForegroundColor Cyan
Write-Host ""

if ($Filter) {
    Write-Host "Filter: $Filter" -ForegroundColor Yellow
    & $TestExe --gtest_filter="$Filter" --gtest_color=yes
} elseif ($Coverage) {
    Write-Host "Running with coverage..." -ForegroundColor Yellow
    # Note: OpenCppCoverage or similar tool would be needed here
    Write-Host "Coverage requires additional setup. Running normal tests instead." -ForegroundColor Yellow
    & $TestExe --gtest_color=yes
} else {
    & $TestExe --gtest_color=yes
}

$ExitCode = $LASTEXITCODE

Write-Host ""
if ($ExitCode -eq 0) {
    Write-Host "All tests passed!" -ForegroundColor Green
} else {
    Write-Host "Some tests failed!" -ForegroundColor Red
}

exit $ExitCode
