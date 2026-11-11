#Requires -Version 5.1
<#
.SYNOPSIS
Check system prerequisites and prepare missing project tools in .tools.
.DESCRIPTION
Git, Python x64 >=3.10, clang-cl >=19.1.7, MSVC >=14.44.35207 and Windows SDK
>=10.0.26100.0 must be installed by the user. Project tools prefer a sufficient
system version, then .tools; missing/outdated project tools download official,
SHA256-verified releases. -Check never downloads or writes files.
#>
[CmdletBinding()]
param(
    [switch]$Check,
    [switch]$PassThru
)
$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'
if ($env:OS -ne 'Windows_NT' -or -not [Environment]::Is64BitProcess) { throw 'Run Windows x64 PowerShell 5.1 or newer.' }
. (Join-Path $PSScriptRoot 'tool-environment.ps1')
$project = Split-Path -Parent $PSScriptRoot
[Net.ServicePointManager]::SecurityProtocol = [Net.ServicePointManager]::SecurityProtocol -bor [Net.SecurityProtocolType]::Tls12
$toolsRoot = [IO.Path]::GetFullPath((Join-Path $project '.tools'))
$vsRoots = @(Get-RbankVisualStudioRoots)
$msvc = Find-RbankMsvc $vsRoots
$sdk = Find-RbankWindowsSdk
$llvmBins = @((Join-Path $env:ProgramFiles 'LLVM/bin'))
foreach ($root in $vsRoots) { $llvmBins += @((Join-Path $root 'VC/Tools/Llvm/x64/bin'), (Join-Path $root 'VC/Tools/Llvm/bin')) }
$compilerCandidates = @(Get-RbankCandidates 'clang-cl' @($llvmBins | ForEach-Object { Join-Path $_ 'clang-cl.exe' }))
$compilerCandidates += Join-Path $toolsRoot 'llvm/bin/clang-cl.exe'
$compiler = Find-RbankTool $compilerCandidates 'compiler' $rbankMinimum.compiler
if ($compiler) { $llvmBins += Split-Path $compiler.path -Parent }
$git = Find-RbankTool (Get-RbankCandidates 'git' @((Join-Path $env:ProgramFiles 'Git/cmd/git.exe'))) 'git' ([version]'0.0') $toolsRoot
$pythonCandidates = @(Get-RbankCandidates 'python') + @(Get-RbankCandidates 'python3')
$launcher = Get-Command py -CommandType Application -ErrorAction SilentlyContinue | Select-Object -First 1
if ($launcher) {
    $launched = Get-RbankNativeOutput $launcher.Source @('-3', '-c', 'import sys; print(sys.executable)')
    if ($launched) { $pythonCandidates += $launched.Trim() }
}
$python = Find-RbankTool $pythonCandidates 'python' $rbankMinimum.python $toolsRoot
$base = [ordered]@{ Git = $git; 'Python x64 >=3.10' = $python; 'C++ clang-cl >=19.1.7' = $compiler; 'MSVC >=14.44.35207' = $msvc; 'Windows SDK >=10.0.26100.0' = $sdk }
$missingBase = @()
foreach ($entry in $base.GetEnumerator()) {
    if ($entry.Value) {
        $location = if ($entry.Key -like 'Windows SDK*') { $entry.Value.root } else { $entry.Value.path }
        Write-Host "[OK] $($entry.Key): $($entry.Value.version) $location"
    } else {
        Write-Host "[MISSING] $($entry.Key)" -ForegroundColor Yellow
        $missingBase += $entry.Key
    }
}
if ($missingBase.Count) {
    Write-Host 'Install the missing prerequisites yourself, then rerun this script:'
    if (-not $git) { Write-Host '  Git for Windows: https://git-scm.com/downloads/win' }
    if (-not $python) { Write-Host '  Python x64 >=3.10 (with ctypes/ssl; add to PATH or install py launcher): https://www.python.org/downloads/windows/' }
    if (-not $compiler) { Write-Host '  LLVM x64 >=19.1.7 (clang-cl; add bin to PATH): https://github.com/llvm/llvm-project/releases' }
    if (-not $msvc -or -not $sdk) {
        Write-Host '  Visual Studio Installer: install Desktop development with C++, MSVC v143 >=14.44.35207 and Windows 11 SDK >=10.0.26100.0.'
        Write-Host '  https://visualstudio.microsoft.com/visual-cpp-build-tools/'
    }
    throw "System prerequisites are missing or too old: $($missingBase -join ', '). No tools were downloaded."
}

$specifications = @(
    @{ key = 'xmake'; exe = 'xmake.exe'; relative = 'xmake/xmake.exe'; minimum = $rbankMinimum.xmake },
    @{ key = 'clangd'; exe = 'clangd.exe'; relative = 'llvm/bin/clangd.exe'; minimum = $rbankMinimum.llvm },
    @{ key = 'formatter'; exe = 'clang-format.exe'; relative = 'llvm/bin/clang-format.exe'; minimum = $rbankMinimum.llvm },
    @{ key = 'llvmReadobj'; exe = 'llvm-readobj.exe'; relative = 'llvm/bin/llvm-readobj.exe'; minimum = $rbankMinimum.llvm },
    @{ key = 'llvmLib'; exe = 'llvm-lib.exe'; relative = 'llvm/bin/llvm-lib.exe'; minimum = $rbankMinimum.llvm },
    @{ key = 'llvmDlltool'; exe = 'llvm-dlltool.exe'; relative = 'llvm/bin/llvm-dlltool.exe'; minimum = $rbankMinimum.llvm },
    @{ key = 'sevenZip'; exe = '7z.exe'; relative = '7zip/7z.exe'; minimum = $rbankMinimum.sevenZip }
)
$resolved = @{}
foreach ($spec in $specifications) {
    $extra = @()
    if ($spec.relative -like 'llvm/*') { $extra += @($llvmBins | ForEach-Object { Join-Path $_ $spec.exe }) }
    if ($spec.key -eq 'sevenZip') { $extra += Join-Path $env:ProgramFiles '7-Zip/7z.exe' }
    $system = Find-RbankTool (Get-RbankCandidates $spec.exe $extra) $spec.key $spec.minimum $toolsRoot
    $local = Join-Path $toolsRoot $spec.relative
    if ($system) { $resolved[$spec.key] = $system }
    else { $resolved[$spec.key] = Find-RbankTool @($local) $spec.key $spec.minimum }
}
$missing = @($specifications | Where-Object { -not $resolved[$_.key] })
if ($Check -and $missing.Count) {
    foreach ($spec in $missing) { Write-Host "[MISSING] $($spec.exe) >=$($spec.minimum)" -ForegroundColor Yellow }
    throw 'Project tools need preparation. Run ./scripts/prepare-tools.ps1 (without -Check).'
}

$identityHash = [Security.Cryptography.SHA256]::Create()
try { $identity = [BitConverter]::ToString($identityHash.ComputeHash([Text.Encoding]::UTF8.GetBytes($project.ToLowerInvariant()))).Replace('-', '') } finally { $identityHash.Dispose() }
$toolsLock = New-Object Threading.Mutex($false, "Local\rbank-tools-$identity")
if (-not $toolsLock.WaitOne(0)) { $toolsLock.Dispose(); throw 'Tool preparation is already running for this checkout.' }
$temporary = $null
try {
    if (-not $Check) {
        [void](Assert-RbankToolsPath $toolsRoot $toolsRoot)
        New-Item -ItemType Directory -Force $toolsRoot | Out-Null
        $temporary = Join-Path $toolsRoot ('prepare-' + [guid]::NewGuid().ToString('N'))
        New-Item -ItemType Directory -Path $temporary | Out-Null
        [Net.ServicePointManager]::SecurityProtocol = [Net.ServicePointManager]::SecurityProtocol -bor [Net.SecurityProtocolType]::Tls12
        if (-not $resolved.xmake -or -not $resolved.sevenZip) {
            $archive = Get-RbankToolArchive 'xmake' $temporary
            $unpacked = Join-Path $temporary 'xmake-unpacked'
            Expand-Archive -LiteralPath $archive -DestinationPath $unpacked
            $children = @(Get-ChildItem -LiteralPath $unpacked -Directory)
            if ($children.Count -ne 1) { throw 'Unexpected xmake archive layout.' }
            $stage = $children[0].FullName
            if (-not (Find-RbankTool @((Join-Path $stage 'xmake.exe')) 'xmake' $rbankMinimum.xmake)) { throw 'Invalid downloaded xmake.' }
            if (-not $resolved.sevenZip) {
                $zipStage = Join-Path $temporary '7zip'
                New-Item -ItemType Directory -Path $zipStage | Out-Null
                Copy-Item -Path (Join-Path $stage 'winenv/bin/7z.*') -Destination $zipStage
                if (-not (Find-RbankTool @((Join-Path $zipStage '7z.exe')) 'sevenZip' $rbankMinimum.sevenZip)) { throw 'Invalid bundled 7-Zip.' }
                Install-RbankToolDirectory $zipStage (Join-Path $toolsRoot '7zip') $toolsRoot
                $resolved.sevenZip = Find-RbankTool @((Join-Path $toolsRoot '7zip/7z.exe')) 'sevenZip' $rbankMinimum.sevenZip
            }
            if (-not $resolved.xmake) {
                Install-RbankToolDirectory $stage (Join-Path $toolsRoot 'xmake') $toolsRoot
                $resolved.xmake = Find-RbankTool @((Join-Path $toolsRoot 'xmake/xmake.exe')) 'xmake' $rbankMinimum.xmake
            }
        }
        $missingLlvm = @($specifications | Where-Object { $_.relative -like 'llvm/*' -and -not $resolved[$_.key] })
        if ($missingLlvm.Count) {
            $archive = Get-RbankToolArchive 'llvm' $temporary
            $stage = Join-Path $temporary 'llvm'
            & $resolved.sevenZip.path x $archive "-o$stage" -y -bso0 -bsp0
            if ($LASTEXITCODE) { throw 'LLVM extraction failed.' }
            foreach ($spec in ($specifications | Where-Object { $_.relative -like 'llvm/*' })) {
                if (-not (Find-RbankTool @((Join-Path $stage ('bin/' + $spec.exe))) $spec.key $spec.minimum)) { throw "Invalid downloaded $($spec.exe)." }
            }
            Remove-RbankToolsDirectory (Join-Path $stage '$PLUGINSDIR') $toolsRoot
            Install-RbankToolDirectory $stage (Join-Path $toolsRoot 'llvm') $toolsRoot
            foreach ($spec in $missingLlvm) { $resolved[$spec.key] = Find-RbankTool @((Join-Path $toolsRoot $spec.relative)) $spec.key $spec.minimum }
        }
    }
    $result = [ordered]@{ project = $project; compiler = $compiler.path; git = $git.path; python = $python.path; msvc = $msvc.path; sdk = $sdk; versions = @{} }
    foreach ($spec in $specifications) {
        $tool = $resolved[$spec.key]
        if (-not $tool) { throw "Tool is still unavailable: $($spec.exe)" }
        Write-Host "[OK] $($spec.exe) >=$($spec.minimum): $($tool.version) $($tool.path)"
        $result[$spec.key] = $tool.path
        $result.versions[$spec.key] = $tool.version
    }
    $result.toolPaths = @(@((Split-Path $compiler.path), (Split-Path $git.path), (Split-Path $python.path), "$($msvc.path)/bin/Hostx64/x64") + @($specifications | ForEach-Object { Split-Path $resolved[$_.key].path }) | Select-Object -Unique)
    $result.requiredFiles = @($compiler.path, $git.path, $python.path, "$($msvc.path)/bin/Hostx64/x64/cl.exe", "$($msvc.path)/bin/Hostx64/x64/link.exe", "$($msvc.path)/include/vector", "$($sdk.root)/Include/$($sdk.version)/um/Windows.h") + @($specifications | ForEach-Object { $resolved[$_.key].path })
    if (-not $Check) {
        # Validate an actual C++20 compile/link against the detected STL/CRT/SDK.
        $source = Join-Path $temporary 'probe.cpp'
        [IO.File]::WriteAllText($source, '#include <vector>
#include <Windows.h>
int main() { std::vector<int> values{1}; return values.front() != 1 || GetCurrentProcessId() == 0; }
')
        $arguments = @('/nologo', '/std:c++20', '/EHsc', '/MD', "/imsvc$($msvc.path)/include", "/imsvc$($sdk.root)/Include/$($sdk.version)/ucrt", "/imsvc$($sdk.root)/Include/$($sdk.version)/um", "/imsvc$($sdk.root)/Include/$($sdk.version)/shared", $source, "/Fo$(Join-Path $temporary 'probe.obj')", "/Fe$(Join-Path $temporary 'probe.exe')", '/link', "/LIBPATH:$($msvc.path)/lib/x64", "/LIBPATH:$($sdk.root)/Lib/$($sdk.version)/ucrt/x64", "/LIBPATH:$($sdk.root)/Lib/$($sdk.version)/um/x64", 'kernel32.lib')
        $previous = $ErrorActionPreference
        $ErrorActionPreference = 'Continue'
        try {
            $probeOutput = (& $compiler.path @arguments 2>&1 | Out-String)
            $probeExit = $LASTEXITCODE
        } finally { $ErrorActionPreference = $previous }
        if ($probeExit) { throw "C++20 compile/link probe failed. Repair your LLVM/MSVC/Windows SDK installation.`n$probeOutput" }
        & (Join-Path $temporary 'probe.exe')
        if ($LASTEXITCODE) { throw 'C++ runtime probe failed.' }
        $environmentFile = Assert-RbankToolsPath (Join-Path $toolsRoot 'environment.json') $toolsRoot
        [IO.File]::WriteAllText($environmentFile, (ConvertTo-Json -InputObject $result -Depth 5))
    }
    Write-Host 'Development tools are ready.'
    if ($PassThru) { return [pscustomobject]$result }
} finally {
    try { if ($temporary) { Remove-RbankToolsDirectory $temporary $toolsRoot } }
    finally { $toolsLock.ReleaseMutex(); $toolsLock.Dispose() }
}
