# Build QuickWiper (native C++/Win32) into a single small self-contained exe.
# Requires MinGW-w64 (WinLibs UCRT). No Visual Studio / Windows SDK needed.
$ErrorActionPreference = 'Stop'

$mingw  = "$env:LOCALAPPDATA\Microsoft\WinGet\Packages\BrechtSanders.WinLibs.POSIX.UCRT_Microsoft.Winget.Source_8wekyb3d8bbwe\mingw64\bin"
$gpp    = Join-Path $mingw 'g++.exe'
$windres= Join-Path $mingw 'windres.exe'
$strip  = Join-Path $mingw 'strip.exe'

$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$dist = Join-Path (Split-Path $here -Parent) 'dist'
if (-not (Test-Path $dist)) { New-Item -ItemType Directory -Path $dist | Out-Null }
$out  = Join-Path $dist 'QuickWiper.exe'
$res  = Join-Path $here 'resource.o'

Push-Location $here
try {
    & $windres 'resource.rc' -O coff -o $res
    if ($LASTEXITCODE) { throw "windres failed" }

    & $gpp 'main.cpp' 'core.cpp' 'cli.cpp' 'gui.cpp' $res `
        -o $out `
        -O2 -std=c++17 -mwindows -DUNICODE -D_UNICODE `
        -static -static-libgcc -static-libstdc++ `
        -lcomctl32 -lbcrypt -ladvapi32
    if ($LASTEXITCODE) { throw "g++ failed" }

    & $strip $out
    Remove-Item $res -ErrorAction SilentlyContinue
    $mb = [math]::Round((Get-Item $out).Length / 1MB, 2)
    Write-Output "Built $out  ($mb MB)"
}
finally { Pop-Location }
