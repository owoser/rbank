# Host-side checks. Default: pure rule tests (no MC, no MySQL).
# -Integration: also builds and runs tests/integration.cpp against MySQL.
# Credentials come from .env-dev (MYSQL_ADDRESS / MYSQL_PORT / MYSQL_USER / MYSQL_PASS);
# they are injected into the process environment only and never written to disk.
#
# Keep this file ASCII-only: Windows PowerShell 5.1 reads BOM-less scripts as ANSI,
# and multi-byte characters in a comment can swallow the following line.
param([switch]$Integration)

$ErrorActionPreference = 'Stop'
$project = Split-Path -Parent $PSScriptRoot
$msvc = 'C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Tools\MSVC\14.44.35207'
$sdkRoot = 'C:\Program Files (x86)\Windows Kits\10'
$sdk = '10.0.26100.0'
$clang = Join-Path $project '.tools/llvm/bin/clang-cl.exe'
$deps = Join-Path $project 'build/deps'
$systemIncludes = @(
    "$msvc/include",
    "$sdkRoot/Include/$sdk/ucrt",
    "$sdkRoot/Include/$sdk/um",
    "$sdkRoot/Include/$sdk/shared"
)
$systemLibraries = @("$msvc/lib/x64", "$sdkRoot/Lib/$sdk/ucrt/x64", "$sdkRoot/Lib/$sdk/um/x64")
$env:INCLUDE = $systemIncludes -join ';'
$env:LIB = $systemLibraries -join ';'
$env:PATH = "$(Split-Path $clang -Parent);$msvc/bin/Hostx64/x64;$env:PATH"

$flags = @('/nologo', '/std:c++20', '/MD', '/EHsc', '/utf-8', '/O2', '/W3', '/DNOMINMAX', '/DUNICODE',
    '-Wno-invalid-offsetof', '-Wno-unused-variable')
$includes = @('/Isrc', "/I$deps/json/include") + @($systemIncludes | ForEach-Object { "/imsvc$_" })

Push-Location $project
try {
    & (Join-Path $PSScriptRoot 'prepare-deps.ps1') -Check
    if (-not (Test-Path -LiteralPath $clang)) { throw "Compiler is missing: $clang. Run ./scripts/prepare-tools.ps1." }
    New-Item -ItemType Directory -Force build/test | Out-Null
    & $clang @flags @includes tests/selftest.cpp src/bootstrap/config/Config.cpp src/infra/cardgen/CardGenerator.cpp `
        /Febuild/test/selftest.exe /link bcrypt.lib
    if ($LASTEXITCODE) { throw 'selftest build failed' }

    & build/test/selftest.exe
    if ($LASTEXITCODE) { throw 'selftest failed' }
    if (-not $Integration) { return }

    foreach ($line in Get-Content (Join-Path $project '.env-dev')) {
        if ($line -match '^\s*(MYSQL_[A-Z_]+)\s*=\s*(.+?)\s*$') {
            Set-Item -Path "env:$($Matches[1])" -Value $Matches[2]
        }
    }
    if (-not $env:MYSQL_VERIFY) { $env:MYSQL_VERIFY = '1' }
    $env:MYSQL_DATABASE = 'rbank_test'
    $env:MYSQL_LIBRARY = Join-Path $project 'build/deps/mariadb/libmariadb.dll'

    & $clang @flags @includes tests/integration.cpp src/bootstrap/config/Config.cpp src/infra/cardgen/CardGenerator.cpp `
        src/infra/runtime/Clock.cpp src/infra/mysql/Database.cpp src/core/service/Bank.cpp `
        /Febuild/test/integration.exe /link bcrypt.lib ws2_32.lib user32.lib
    if ($LASTEXITCODE) { throw 'integration build failed' }

    & build/test/integration.exe
    if ($LASTEXITCODE) { throw 'integration failed' }
} finally { Pop-Location }
