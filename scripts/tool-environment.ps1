# Shared discovery for prepare-tools.ps1. No downloads or persistent environment changes.
$rbankMinimum = @{
    python = [version]'3.10.0'
    compiler = [version]'19.1.7'
    msvc = [version]'14.44.35207'
    sdk = [version]'10.0.26100.0'
    xmake = [version]'3.1.2'
    llvm = [version]'19.1.7'
    sevenZip = [version]'24.8.0'
}

function Get-RbankNativeOutput([string]$Path, [string[]]$Arguments) {
    # Windows PowerShell turns native stderr into errors, including normal version output.
    $previous = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    try {
        $output = (& $Path @Arguments 2>&1 | Out-String)
        if ($LASTEXITCODE -eq 0) { return $output }
    } catch {
        return $null
    } finally {
        $ErrorActionPreference = $previous
    }
}

function Get-RbankVersion([string]$Path, [string]$Name) {
    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) { return $null }
    $arguments = @('--version')
    $pattern = '(?i)(?:clangd version|clang version|clang-format version|LLVM version)\s+(\d+\.\d+(?:\.\d+)?)'
    if ($Name -eq 'xmake') { $pattern = 'xmake\s+v(\d+\.\d+\.\d+)' }
    if ($Name -eq 'git') { $pattern = 'git version\s+(\d+\.\d+(?:\.\d+)?)' }
    if ($Name -eq 'python') {
        $arguments = @('-c', 'import sys, ctypes, ssl, struct; assert struct.calcsize(''P'') == 8; print(''Python %d.%d.%d'' % sys.version_info[:3])')
        $pattern = 'Python\s+(\d+\.\d+\.\d+)'
    }
    if ($Name -eq 'sevenZip') {
        $arguments = @('i')
        $pattern = '7-Zip\s+(\d+\.\d+)'
    }
    # llvm-lib and llvm-dlltool have no version switch; use their suite's version reporter.
    $reporter = $Path
    if ($Name -in @('llvmLib', 'llvmDlltool')) {
        $reporter = Join-Path (Split-Path -Parent $Path) 'llvm-readobj.exe'
        if (-not (Test-Path -LiteralPath $reporter)) { return $null }
    }
    $output = Get-RbankNativeOutput $reporter $arguments
    if ($output -and $output -match $pattern) {
        $value = $Matches[1]
        if ($value -notmatch '^\d+\.\d+\.\d+') { $value += '.0' }
        return [version]$value
    }
}

function Get-RbankVisualStudioRoots {
    $roots = @()
    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
    if (Test-Path -LiteralPath $vswhere) {
        $roots += @(& $vswhere -products '*' -property installationPath)
    }
    if ($env:VSINSTALLDIR) { $roots += $env:VSINSTALLDIR }
    return @($roots | Where-Object { $_ -and (Test-Path -LiteralPath $_) } | Select-Object -Unique)
}

function Find-RbankMsvc([string[]]$VisualStudioRoots) {
    $directories = @()
    if ($env:VCToolsInstallDir) { $directories += $env:VCToolsInstallDir.TrimEnd('\') }
    foreach ($root in $VisualStudioRoots) {
        $path = Join-Path $root 'VC/Tools/MSVC'
        if (Test-Path -LiteralPath $path) {
            $directories += @(Get-ChildItem -LiteralPath $path -Directory | Select-Object -ExpandProperty FullName)
        }
    }
    $candidates = @()
    foreach ($path in ($directories | Select-Object -Unique)) {
        $version = $null
        if (-not [version]::TryParse((Split-Path $path -Leaf), [ref]$version) -or $version -lt $rbankMinimum.msvc) { continue }
        $candidates += [pscustomobject]@{ path = $path; version = $version }
    }
    foreach ($candidate in ($candidates | Sort-Object version -Descending)) {
        $path = $candidate.path
        $files = @('bin/Hostx64/x64/cl.exe', 'bin/Hostx64/x64/link.exe', 'include/vector', 'lib/x64/msvcrt.lib')
        if (-not @($files | Where-Object { -not (Test-Path -LiteralPath (Join-Path $path $_)) }).Count) {
            return [pscustomobject]@{ path = $path; version = $candidate.version.ToString() }
        }
    }
}

function Find-RbankWindowsSdk {
    $roots = @($env:WindowsSdkDir)
    foreach ($key in @('HKLM:/SOFTWARE/Microsoft/Windows Kits/Installed Roots', 'HKLM:/SOFTWARE/WOW6432Node/Microsoft/Windows Kits/Installed Roots')) {
        $item = Get-ItemProperty -LiteralPath $key -ErrorAction SilentlyContinue
        if ($item) { $roots += $item.KitsRoot10 }
    }
    $roots += Join-Path ${env:ProgramFiles(x86)} 'Windows Kits/10'
    $found = @()
    foreach ($root in ($roots | Where-Object { $_ } | Select-Object -Unique)) {
        $include = Join-Path $root 'Include'
        if (-not (Test-Path -LiteralPath $include)) { continue }
        foreach ($directory in (Get-ChildItem -LiteralPath $include -Directory)) {
            $version = $null
            if (-not [version]::TryParse($directory.Name, [ref]$version) -or $version -lt $rbankMinimum.sdk) { continue }
            $files = @("Include/$version/um/Windows.h", "Include/$version/ucrt/stdio.h", "Include/$version/shared/sdkddkver.h", "Lib/$version/um/x64/kernel32.lib", "Lib/$version/ucrt/x64/ucrt.lib", "bin/$version/x64/rc.exe")
            if (-not @($files | Where-Object { -not (Test-Path -LiteralPath (Join-Path $root $_)) }).Count) {
                $found += [pscustomobject]@{ root = $root.TrimEnd('\'); version = $version.ToString() }
            }
        }
    }
    return $found | Sort-Object { [version]$_.version } -Descending | Select-Object -First 1
}

function Get-RbankCandidates([string]$Name, [string[]]$ExtraPaths = @()) {
    $paths = @(Get-Command $Name -CommandType Application -All -ErrorAction SilentlyContinue | Select-Object -ExpandProperty Source)
    return @($paths + $ExtraPaths | Where-Object { $_ -and (Test-Path -LiteralPath $_ -PathType Leaf) } | Select-Object -Unique)
}

function Find-RbankTool([string[]]$Candidates, [string]$Name, [version]$Minimum, [string]$ExcludeRoot) {
    foreach ($path in $Candidates) {
        if ($ExcludeRoot -and [IO.Path]::GetFullPath($path).StartsWith($ExcludeRoot + '\', [StringComparison]::OrdinalIgnoreCase)) { continue }
        $version = Get-RbankVersion $path $Name
        if ($version -and $version -ge $Minimum) {
            return [pscustomobject]@{ path = $path; version = $version.ToString() }
        }
    }
}

function Assert-RbankToolsPath([string]$Path, [string]$Root) {
    $absolute = [IO.Path]::GetFullPath($Path)
    if ($absolute -ne $Root -and -not $absolute.StartsWith($Root + '\', [StringComparison]::OrdinalIgnoreCase)) {
        throw "Path must stay inside .tools: $absolute"
    }
    $cursor = $absolute
    while ($cursor -and $cursor.Length -ge $Root.Length) {
        if ((Test-Path -LiteralPath $cursor) -and ((Get-Item -LiteralPath $cursor -Force).Attributes -band [IO.FileAttributes]::ReparsePoint)) {
            throw "Refusing a reparse point: $cursor"
        }
        $cursor = [IO.Path]::GetDirectoryName($cursor)
    }
    return $absolute
}

function Remove-RbankToolsDirectory([string]$Path, [string]$Root) {
    $absolute = Assert-RbankToolsPath $Path $Root
    if (Test-Path -LiteralPath $absolute) {
        $active = @(Get-CimInstance Win32_Process | Where-Object {
            $_.ExecutablePath -and $_.ExecutablePath.StartsWith($absolute + '\', [StringComparison]::OrdinalIgnoreCase)
        })
        if ($active.Count) { throw "Stop processes using $absolute first: $($active.ProcessId -join ', ')" }
        Remove-Item -LiteralPath $absolute -Recurse -Force
    }
}

function Get-RbankToolArchive([string]$Name, [string]$Temporary) {
    $releases = @{
        xmake = @('https://github.com/xmake-io/xmake/releases/download/v3.1.2/xmake-v3.1.2.win64.zip', 'be1c5fd5abec08ac14004daca7bfb1ec6e1f2f2dea94877de8f44dc6e0aed1b1', 'xmake.zip')
        llvm = @('https://github.com/llvm/llvm-project/releases/download/llvmorg-19.1.7/LLVM-19.1.7-win64.exe', 'f19ae5bc4823ac69ec01dc2ded503ec80a04ad2208dda1595d1f0413c148ef90', 'llvm.exe')
    }
    $release = $releases[$Name]
    $archive = Join-Path $Temporary $release[2]
    if (-not (Test-Path -LiteralPath $archive)) {
        Write-Host "Downloading $($release[0])"
        Invoke-WebRequest -Uri $release[0] -UseBasicParsing -OutFile $archive
    }
    if ((Get-FileHash -LiteralPath $archive -Algorithm SHA256).Hash -ne $release[1]) { throw "SHA256 mismatch: $Name" }
    return $archive
}

function Install-RbankToolDirectory([string]$Stage, [string]$Destination, [string]$Root) {
    [void](Assert-RbankToolsPath $Stage $Root)
    [void](Assert-RbankToolsPath $Destination $Root)
    $backup = $null
    if (Test-Path -LiteralPath $Destination) {
        # Verify before moving anything; the previous installation survives a failed download/extract.
        $active = @(Get-CimInstance Win32_Process | Where-Object {
            $_.ExecutablePath -and $_.ExecutablePath.StartsWith($Destination + '\', [StringComparison]::OrdinalIgnoreCase)
        })
        if ($active.Count) { throw "Stop tools before replacing $Destination" }
        $backup = Join-Path (Split-Path $Stage -Parent) ([guid]::NewGuid().ToString('N'))
        [void](Assert-RbankToolsPath $backup $Root)
        Move-Item -LiteralPath $Destination -Destination $backup
    }
    try {
        Move-Item -LiteralPath $Stage -Destination $Destination
    } catch {
        if ($backup) { Move-Item -LiteralPath $backup -Destination $Destination }
        throw
    }
    if ($backup) { Remove-RbankToolsDirectory $backup $Root }
}
