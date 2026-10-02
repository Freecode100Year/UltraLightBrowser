param([ValidateSet('Release','Debug')][string]$Configuration = 'Release')
$ErrorActionPreference = 'Stop'
Push-Location $PSScriptRoot
try {
    foreach ($tool in @('nuget','cmake')) {
        if (-not (Get-Command $tool -ErrorAction SilentlyContinue)) {
            throw "Install $tool and add it to PATH. Also install Visual Studio 2022 Desktop development with C++."
        }
    }
    nuget restore packages.config -PackagesDirectory packages
    if ($LASTEXITCODE -ne 0) { throw 'NuGet restore failed.' }
    cmake -B build -S . -G 'Visual Studio 17 2022' -A x64
    if ($LASTEXITCODE -ne 0) { throw 'CMake configure failed.' }
    cmake --build build --config $Configuration --parallel
    if ($LASTEXITCODE -ne 0) { throw 'Build failed.' }
    $exe = "build\$Configuration\UltraLightBrowser.exe"
    if (-not (Test-Path $exe)) { throw 'Executable missing.' }
    $packageDirectory = "dist\$Configuration"
    New-Item -ItemType Directory -Force -Path $packageDirectory | Out-Null
    Copy-Item $exe $packageDirectory -Force
    $loader = "build\$Configuration\WebView2Loader.dll"
    if (Test-Path $loader) { Copy-Item $loader $packageDirectory -Force }
    foreach ($file in @('LICENSE','FULL_VERSION_GUIDE.md')) {
        Copy-Item $file $packageDirectory -Force
    }
    Write-Host "Ready: $packageDirectory\UltraLightBrowser.exe"
} finally { Pop-Location }
