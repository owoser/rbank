# 宿主侧自检：只编译不依赖 MC / MySQL 的部分，运行 tests/selftest.cpp。
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
    New-Item -ItemType Directory -Force build/test | Out-Null
    & $clang @flags @includes tests/selftest.cpp src/bootstrap/config/Config.cpp src/infra/cardgen/CardGenerator.cpp `
        /Febuild/test/selftest.exe /link bcrypt.lib
    if ($LASTEXITCODE) { throw 'selftest build failed' }

    & build/test/selftest.exe
    if ($LASTEXITCODE) { throw 'selftest failed' }
} finally { Pop-Location }
