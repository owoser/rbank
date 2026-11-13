param(
    [switch]$CompileCommandsOnly,
    [switch]$NoLink
)
$ErrorActionPreference = 'Stop'
$project = Split-Path -Parent $PSScriptRoot
& (Join-Path $PSScriptRoot 'prepare-deps.ps1') -Check
$tools = & (Join-Path $PSScriptRoot 'prepare-tools.ps1') -Check -PassThru
$msvc = $tools.msvc
$sdkRoot = $tools.sdk.root
$sdk = $tools.sdk.version
$clang = $tools.compiler
$deps = Join-Path $project 'build/deps'
if (-not $clang) { throw 'Compiler is missing. Run ./scripts/prepare-tools.ps1.' }
$includeDirs = @(
    "$deps/levilamina/src",
    "$deps/levilamina/src-server",
    "$deps/fmt/include",
    "$deps/entt/src",
    "$deps/expected-lite/include",
    "$deps/gsl/include",
    "$deps/glm",
    "$deps/leveldb/include",
    "$deps/magic-enum/include",
    "$deps/json/include",
    "$deps/rapidjson/include",
    "$deps/type-safe/include",
    "$deps/debug-assert",
    "$deps/pcg/include",
    "$deps/pfr/include",
    "$deps/parallel-hashmap",
    "$deps/concurrentqueue",
    "$deps/stb",
    "$deps/httplib",
    "$deps/pcg/prefix"
)
$systemIncludes = @(
    "$msvc/include",
    "$sdkRoot/Include/$sdk/ucrt",
    "$sdkRoot/Include/$sdk/um",
    "$sdkRoot/Include/$sdk/shared",
    "$sdkRoot/Include/$sdk/winrt"
)
$systemLibraries = @("$msvc/lib/x64", "$sdkRoot/Lib/$sdk/ucrt/x64", "$sdkRoot/Lib/$sdk/um/x64")
$env:INCLUDE = $systemIncludes -join ';'
$env:LIB = $systemLibraries -join ';'
$env:PATH = "$deps/prelink;$deps/bedrock;$(Split-Path $clang -Parent);$msvc/bin/Hostx64/x64;$env:PATH"

$flags = @('/nologo', '/std:c++20', '/MD', '/EHsc', '/utf-8', '/O2', '/W3', '/DNOMINMAX', '/DUNICODE', '/DLL_PLAT_S', '/D_HAS_CXX23=1',
    '-Wno-invalid-offsetof', '-Wno-pragma-system-header-outside-header', '-Wno-microsoft-cast', '-Wno-missing-field-initializers',
    '-Wno-potentially-evaluated-expression', '-Wno-overloaded-virtual', '-Wno-ignored-qualifiers', '-Wno-microsoft-include')
$flags[$flags.IndexOf('/DLL_PLAT_S')] = '/DLL_PLAT_S'
$includes = @('/Isrc') + @($includeDirs | ForEach-Object { "/I$_" })
$sysIncludes = @($systemIncludes | ForEach-Object { "/imsvc$_" })

function Write-CompileCommands($files) {
    $commands = @(foreach ($source in $files) {
        $relative = $source.FullName.Substring($project.Length + 1)
        $object = Join-Path $project ('build/local/' + ($relative -replace '[\\/]', '_') + '.obj')
        [ordered]@{ directory = $project; file = $source.FullName; arguments = @($clang) + $flags + $includes + $sysIncludes + @('/c', $source.FullName, "/Fo$object") }
    })
    [IO.File]::WriteAllText((Join-Path $project 'build/compile_commands.json'), (ConvertTo-Json -InputObject $commands -Depth 5))
    Write-Output "Generated build/compile_commands.json ($($commands.Count) sources)"
}

Push-Location $project
try {
    $sources = @(Get-ChildItem -LiteralPath src -Filter '*.cpp' -Recurse | Sort-Object FullName)
    New-Item -ItemType Directory -Force build/local, build/rbank | Out-Null
    Write-CompileCommands $sources
    if ($CompileCommandsOnly) { return }

    $objects = @()
    $headerTime = (Get-ChildItem src -Filter '*.h' -Recurse | Sort-Object LastWriteTimeUtc -Descending | Select-Object -First 1).LastWriteTimeUtc
    $failed = 0
    foreach ($source in $sources) {
        $object = Join-Path $project ('build/local/' + ($source.FullName.Substring($project.Length + 1) -replace '[\\/]', '_') + '.obj')
        $needs = -not (Test-Path -LiteralPath $object)
        if (-not $needs) {
            $objectTime = (Get-Item -LiteralPath $object).LastWriteTimeUtc
            $needs = $objectTime -lt $source.LastWriteTimeUtc -or $objectTime -lt $headerTime
        }
        if ($needs) {
            Write-Output "Compiling $($source.FullName.Substring($project.Length + 1))"
            & $clang @flags @includes @sysIncludes /c $source.FullName "/Fo$object"
            if ($LASTEXITCODE) { $failed++ }
        }
        $objects += $object
    }
    if ($failed) { throw "$failed source file(s) failed to compile" }
    if ($NoLink) { return }

    New-Item -ItemType Directory -Force build/local/prelink/lib | Out-Null
    & "$deps/prelink/prelink.exe" server-windows-x64 build/local/prelink "$deps/bedrock/bedrock_runtime_data" @objects
    if ($LASTEXITCODE) { throw 'Official prelink failed' }
    & "$msvc/bin/Hostx64/x64/link.exe" /NOLOGO /DLL /MACHINE:X64 /OUT:build/rbank/rbank.dll /PDB:build/local/rbank.pdb `
        /IMPLIB:build/local/rbank.lib /INCREMENTAL:NO /DEBUG /DELAYLOAD:bedrock_runtime.dll @objects `
        "$deps/lib/LeviLamina.lib" build/local/prelink/lib/bedrock_runtime_api.lib "$deps/lib/fmt.lib" `
        "$deps/lib/SymbolProvider.lib" ws2_32.lib crypt32.lib bcrypt.lib secur32.lib user32.lib shell32.lib
    if ($LASTEXITCODE) { throw 'Link failed' }
    Copy-Item -LiteralPath manifest.json -Destination build/rbank/manifest.json -Force
    if (Test-Path assets) { Copy-Item -Path assets/* -Destination build/rbank -Recurse -Force }
    Copy-Item -LiteralPath "$deps/mariadb/libmariadb.dll" -Destination build/rbank -Force
    Copy-Item -LiteralPath "$deps/mariadb/Connector-C-LICENSE.txt" -Destination build/rbank -Force
    New-Item -ItemType Directory -Force build/rbank/plugin | Out-Null
    Get-ChildItem "$deps/mariadb/plugin" -Filter '*.dll' | Copy-Item -Destination build/rbank/plugin -Force
    Write-Output 'Built build/rbank/rbank.dll'
} finally { Pop-Location }
