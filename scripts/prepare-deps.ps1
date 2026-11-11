#Requires -Version 5.1
<#
.SYNOPSIS
Populate build/deps from scripts/dependencies.lock.json.
.DESCRIPTION
Every archive is pinned by URL and SHA256. Downloads land in a temporary
directory, are verified, then moved into build/deps. An already-installed
component is kept when its stamp matches the lock, so re-runs are cheap.
Import libraries (fmt, SymbolProvider, LeviLamina) are compiled locally from
the pinned sources. -Check verifies the tree without downloading or writing.
#>
[CmdletBinding()]
param(
    [switch]$Check,
    [switch]$PassThru
)
$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'
[Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12

$project = Split-Path -Parent $PSScriptRoot
$buildRoot = [IO.Path]::GetFullPath((Join-Path $project 'build'))
$deps = Join-Path $buildRoot 'deps'
$lockFile = Join-Path $PSScriptRoot 'dependencies.lock.json'
$manifestFile = Join-Path $buildRoot 'deps-manifest.json'

function Assert-DepsPath([string]$Path) {
    $absolute = [IO.Path]::GetFullPath($Path)
    if ($absolute -ne $buildRoot -and -not $absolute.StartsWith($buildRoot + '\', [StringComparison]::OrdinalIgnoreCase)) {
        throw "Path must stay inside build: $absolute"
    }
    $cursor = $absolute
    while ($cursor -and $cursor.Length -ge $buildRoot.Length) {
        if ((Test-Path -LiteralPath $cursor) -and ((Get-Item -LiteralPath $cursor -Force).Attributes -band [IO.FileAttributes]::ReparsePoint)) {
            throw "Refusing a reparse point: $cursor"
        }
        $cursor = [IO.Path]::GetDirectoryName($cursor)
    }
    return $absolute
}

function Remove-DepsDirectory([string]$Path) {
    $absolute = Assert-DepsPath $Path
    if (-not (Test-Path -LiteralPath $absolute)) { return }
    $active = @(Get-CimInstance Win32_Process | Where-Object {
        $_.ExecutablePath -and $_.ExecutablePath.StartsWith($absolute + '\', [StringComparison]::OrdinalIgnoreCase)
    })
    if ($active.Count) { throw "Stop processes using $absolute first: $($active.ProcessId -join ', ')" }
    Remove-Item -LiteralPath $absolute -Recurse -Force
}

function Get-DepsDownload([string]$Url, [string]$Destination, [string]$Sha256) {
    if (-not (Test-Path -LiteralPath $Destination)) {
        Write-Host "Downloading $Url"
        Invoke-WebRequest -Uri $Url -UseBasicParsing -OutFile $Destination
    }
    $actual = (Get-FileHash -LiteralPath $Destination -Algorithm SHA256).Hash.ToLowerInvariant()
    if ($actual -ne $Sha256) {
        Remove-Item -LiteralPath $Destination -Force
        throw "SHA256 mismatch for ${Url}: expected $Sha256, got $actual"
    }
}

function Install-DepsZip([string]$Archive, [string]$Destination, [string]$SevenZip, [switch]$StripRoot) {
    $stage = Join-Path $temporary ([guid]::NewGuid().ToString('N'))
    if ($SevenZip) {
        & $SevenZip x $Archive "-o$stage" -y -bso0 -bsp0
        if ($LASTEXITCODE) { throw "Archive extraction failed: $Archive" }
    } else {
        Expand-Archive -LiteralPath $Archive -DestinationPath $stage
    }
    $source = $stage
    if ($StripRoot) {
        $children = @(Get-ChildItem -LiteralPath $stage -Force)
        if ($children.Count -ne 1 -or -not $children[0].PSIsContainer) { throw "Expected one root directory: $Archive" }
        $source = $children[0].FullName
    }
    Remove-DepsDirectory $Destination
    Move-Item -LiteralPath $source -Destination $Destination
}

# Do not force -Encoding here: PowerShell 5.1 returns the whole array as one item when it does.
$lock = ConvertFrom-Json -InputObject (Get-Content -LiteralPath $lockFile -Raw)
$lockHash = (Get-FileHash -LiteralPath $lockFile -Algorithm SHA256).Hash.ToLowerInvariant()

# Binaries that ship as their own release archive.
$binaries = @(
    @{ id = 'prelink'; url = 'https://github.com/LiteLDev/prelink-release/releases/download/v0.8.8/prelink-v0.8.8-windows-x64.zip'; sha256 = '046811a5d1f8bdd0c1e74afdd3e0a2d778df03568c06c835424b2c821896e39c'; probe = 'prelink.exe' },
    @{ id = 'bedrock'; url = 'https://github.com/LiteLDev/bedrock-runtime-data/releases/download/v26.60.6-server.12/bedrock-runtime-data-v26.60.6-server.12-windows-x64.zip'; sha256 = 'e91b84de0cf046e25f7309eaf09590fec4985512b234695121fa1431ae68344b'; probe = 'bedrock_runtime_data' },
    @{ id = 'loader'; url = 'https://github.com/LiteLDev/LeviLamina/releases/download/v26.60.5/levilamina-v26.60.5-server-release-windows-x64.zip'; sha256 = 'c032c0bdf8a58656958e36eb33093fae12d2822c00e35672cf4804f7caad33a5'; probe = 'LeviLamina/LeviLamina.dll' }
)
$mariaUrl = 'https://dlm.mariadb.com/4516894/Connectors/c/connector-c-3.4.8/mariadb-connector-c-3.4.8-win64.msi'
$mariaSha = '2d4c05a515a79929dcf1b1e9fd21959d02162d130f83f46ef30717c32778cc56'
$licenseUrl = 'https://raw.githubusercontent.com/mariadb-corporation/mariadb-connector-c/v3.4.8/COPYING.LIB'
$licenseSha = 'dc626520dcd53a22f727af3ee42c770e56c97a64fe3adb063799d8ab032fe551'

# Every file the build needs after preparation, relative to build/deps.
$required = @()
foreach ($item in $lock) { $required += "$($item.id)/$($item.probe)" }
foreach ($item in $binaries) { $required += "$($item.id)/$($item.probe)" }
$required += @('mariadb/libmariadb.dll', 'mariadb/plugin/caching_sha2_password.dll', 'mariadb/Connector-C-LICENSE.txt',
    'lib/LeviLamina.lib', 'lib/fmt.lib', 'lib/SymbolProvider.lib', 'pcg/prefix/pcg_cpp/pcg_random.hpp')

if ($Check) {
    $missing = @($required | Where-Object { -not (Test-Path -LiteralPath (Join-Path $deps $_)) })
    $stale = @()
    foreach ($item in $lock) {
        $stamp = Join-Path $deps "$($item.id)/.rbank-sha256"
        if (-not (Test-Path -LiteralPath $stamp) -or (Get-Content -LiteralPath $stamp -Raw).Trim() -ne $item.sha256) { $stale += $item.id }
    }
    if (-not (Test-Path -LiteralPath $manifestFile)) { $stale += 'manifest' }
    elseif ((Get-Content -LiteralPath $manifestFile -Raw | ConvertFrom-Json).lockHash -ne $lockHash) { $stale += 'lock' }
    if ($missing.Count -or $stale.Count) {
        throw "build/deps is incomplete or outdated. Run ./scripts/prepare-deps.ps1. Missing: $($missing -join ', ') Outdated: $($stale -join ', ')"
    }
    Write-Host 'Dependencies are ready.'
    return
}

$tools = & (Join-Path $PSScriptRoot 'prepare-tools.ps1') -PassThru
if (-not $? -or -not $tools.compiler) { throw 'Tool preparation failed.' }
$compiler = $tools.compiler
$sevenZip = $tools.sevenZip

[void](Assert-DepsPath $buildRoot)
New-Item -ItemType Directory -Force $deps | Out-Null
$temporary = Assert-DepsPath (Join-Path $buildRoot ('prepare-deps-' + [guid]::NewGuid().ToString('N')))
New-Item -ItemType Directory -Path $temporary | Out-Null
try {
    foreach ($item in $lock) {
        $destination = Join-Path $deps $item.id
        $stamp = Join-Path $destination '.rbank-sha256'
        $probe = Join-Path $destination $item.probe
        $current = if (Test-Path -LiteralPath $stamp) { (Get-Content -LiteralPath $stamp -Raw).Trim() } else { '' }
        if ($current -eq $item.sha256 -and (Test-Path -LiteralPath $probe)) { continue }
        $archive = Join-Path $temporary ($item.id + '.zip')
        Get-DepsDownload $item.url $archive $item.sha256
        Install-DepsZip $archive $destination $sevenZip -StripRoot
        [IO.File]::WriteAllText($stamp, $item.sha256)
    }

    foreach ($item in $binaries) {
        $destination = Join-Path $deps $item.id
        if (Test-Path -LiteralPath (Join-Path $destination $item.probe)) { continue }
        $archive = Join-Path $temporary ($item.id + '.zip')
        Get-DepsDownload $item.url $archive $item.sha256
        Install-DepsZip $archive $destination $sevenZip
    }

    $maria = Join-Path $deps 'mariadb'
    if (-not (Test-Path -LiteralPath (Join-Path $maria 'libmariadb.dll')) -or -not (Test-Path -LiteralPath (Join-Path $maria 'Connector-C-LICENSE.txt'))) {
        $archive = Join-Path $temporary 'mariadb.msi'
        Get-DepsDownload $mariaUrl $archive $mariaSha
        $extracted = Join-Path $temporary 'mariadb'
        & $sevenZip x $archive "-o$extracted" -y -bso0 -bsp0
        if ($LASTEXITCODE) { throw 'Connector/C extraction failed' }
        Remove-DepsDirectory $maria
        New-Item -ItemType Directory -Force (Join-Path $maria 'plugin') | Out-Null
        Copy-Item -LiteralPath (Join-Path $extracted 'libdll') -Destination (Join-Path $maria 'libmariadb.dll')
        Get-ChildItem -LiteralPath $extracted -File -Filter '*.dll' | Copy-Item -Destination (Join-Path $maria 'plugin') -Force
        Get-DepsDownload $licenseUrl (Join-Path $maria 'Connector-C-LICENSE.txt') $licenseSha
    }

    # The SDK includes pcg headers with a pcg_cpp prefix; upstream keeps them directly in include.
    $pcgInclude = Join-Path $deps 'pcg/prefix/pcg_cpp'
    New-Item -ItemType Directory -Force $pcgInclude | Out-Null
    Copy-Item -Path (Join-Path $deps 'pcg/include/*.hpp') -Destination $pcgInclude -Force

    # Import libraries depend on the toolchain: rebuild them when it changes.
    $compilerVersion = (& $compiler --version) -join ' '
    $toolchain = "$($tools.msvc)|$($tools.sdk.version)|$compilerVersion"
    $libDir = Join-Path $deps 'lib'
    $libStamp = Join-Path $libDir '.rbank-toolchain'
    $previous = if (Test-Path -LiteralPath $libStamp) { [IO.File]::ReadAllText($libStamp) } else { '' }
    if ((Test-Path -LiteralPath $libDir) -and $previous -ne $toolchain) {
        Remove-DepsDirectory $libDir
        Remove-DepsDirectory (Join-Path $buildRoot 'local')
    }
    New-Item -ItemType Directory -Force $libDir | Out-Null

    if (-not (Test-Path -LiteralPath (Join-Path $libDir 'fmt.lib'))) {
        $objects = @()
        foreach ($name in @('format', 'os')) {
            $object = Join-Path $temporary ($name + '.obj')
            & $compiler /nologo /c /std:c++20 /MD /EHsc /utf-8 /O2 ("/I" + (Join-Path $deps 'fmt/include')) (Join-Path $deps "fmt/src/$name.cc") "/Fo$object"
            if ($LASTEXITCODE) { throw 'fmt compilation failed' }
            $objects += $object
        }
        & $tools.llvmLib /nologo ("/OUT:" + (Join-Path $libDir 'fmt.lib')) @objects
        if ($LASTEXITCODE) { throw 'fmt library creation failed' }
    }

    if (-not (Test-Path -LiteralPath (Join-Path $libDir 'SymbolProvider.lib'))) {
        $objects = @()
        foreach ($source in (Get-ChildItem -LiteralPath (Join-Path $deps 'symbolprovider/src') -Filter '*.cpp' -Recurse)) {
            $object = Join-Path $temporary ($source.BaseName + '.obj')
            & $compiler /nologo /c /std:c++20 /MD /EHsc /utf-8 /O2 $source.FullName "/Fo$object"
            if ($LASTEXITCODE) { throw 'SymbolProvider compilation failed' }
            $objects += $object
        }
        & $tools.llvmLib /nologo ("/OUT:" + (Join-Path $libDir 'SymbolProvider.lib')) @objects
        if ($LASTEXITCODE) { throw 'SymbolProvider library creation failed' }
    }

    $loaderLib = Join-Path $libDir 'LeviLamina.lib'
    if (-not (Test-Path -LiteralPath $loaderLib)) {
        $exports = & $tools.llvmReadobj --coff-exports (Join-Path $deps 'loader/LeviLamina/LeviLamina.dll')
        if ($LASTEXITCODE) { throw 'Loader export reading failed' }
        $definitions = @('LIBRARY LeviLamina.dll', 'EXPORTS')
        foreach ($line in $exports) { if ($line -match '^\s+Name: (.+)$') { $definitions += $Matches[1] } }
        $def = Join-Path $temporary 'LeviLamina.def'
        [IO.File]::WriteAllLines($def, $definitions)
        & $tools.llvmDlltool -m i386:x86-64 -d $def -l $loaderLib
        if ($LASTEXITCODE) { throw 'Loader import library creation failed' }
    }
    [IO.File]::WriteAllText($libStamp, $toolchain)

    $missing = @($required | Where-Object { -not (Test-Path -LiteralPath (Join-Path $deps $_)) })
    if ($missing.Count) { throw "Preparation incomplete: $($missing -join ', ')" }
    $manifest = [ordered]@{ lockHash = $lockHash; toolchain = $toolchain; required = $required }
    [IO.File]::WriteAllText($manifestFile, (ConvertTo-Json -InputObject $manifest -Depth 5))
    Write-Host "Dependencies are ready in $deps."
    if ($PassThru) { return [pscustomobject]$manifest }
} finally {
    if ($temporary -and (Test-Path -LiteralPath $temporary)) { Remove-DepsDirectory $temporary }
}
