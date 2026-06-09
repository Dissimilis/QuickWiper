# Compile and run the native pass-planner unit test (native\test_plan.cpp).
# Pure logic, no device and no elevation needed. Exit 0 => all checks passed.
$ErrorActionPreference = 'Stop'
$mingw = "$env:LOCALAPPDATA\Microsoft\WinGet\Packages\BrechtSanders.WinLibs.POSIX.UCRT_Microsoft.Winget.Source_8wekyb3d8bbwe\mingw64\bin"
$gpp   = Join-Path $mingw 'g++.exe'
$root  = Split-Path $PSScriptRoot -Parent
$exe   = Join-Path $env:TEMP 'qw-test_plan.exe'

& $gpp (Join-Path $root 'native\test_plan.cpp') (Join-Path $root 'native\core.cpp') `
    -o $exe -O2 -std=c++17 -DUNICODE -D_UNICODE -lbcrypt -ladvapi32
if ($LASTEXITCODE) { throw "compile failed" }

& $exe
$code = $LASTEXITCODE
Remove-Item $exe -ErrorAction SilentlyContinue
exit $code
