#Requires -Version 5.1
<#
.SYNOPSIS
Populate docs-ref from scripts/references.lock.json.
.DESCRIPTION
Upstream repositories are fetched at the commit pinned in the lock file and
checked out sparsely, so docs-ref holds only the paths AGENTS.md links to
instead of complete histories. An existing clone whose origin matches the lock
is updated in place and must be clean; anything else is refused. -Check verifies
the tree without writing.
#>
[CmdletBinding()]
param(
    [switch]$Check
)
$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'
[Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12

$project = Split-Path -Parent $PSScriptRoot
$refs = [IO.Path]::GetFullPath((Join-Path $project 'docs-ref'))
$lockFile = Join-Path $PSScriptRoot 'references.lock.json'

function Invoke-Git([string]$Root, [string[]]$Arguments, [switch]$AllowFailure) {
    # Git reports progress on stderr; Windows PowerShell turns that into errors.
    $previous = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    try {
        $output = (& git -C $Root @Arguments 2>&1 | Out-String)
        $exit = $LASTEXITCODE
    } finally {
        $ErrorActionPreference = $previous
    }
    if ($exit -and -not $AllowFailure) { throw "git $($Arguments -join ' ') failed in ${Root}:`n$output" }
    return [pscustomobject]@{ exit = $exit; output = $output.Trim() }
}

function Get-RemoteUrl([string]$Path) {
    $result = Invoke-Git $Path @('remote', 'get-url', 'origin') -AllowFailure
    if ($result.exit) { return $null }
    return $result.output.TrimEnd('/')
}

function Test-Reference([object]$Entry) {
    $problems = @()
    $target = Join-Path $refs $Entry.dir
    if (-not (Test-Path -LiteralPath (Join-Path $target '.git'))) {
        return @("$($Entry.dir): missing")
    }
    $origin = Get-RemoteUrl $target
    if (-not $origin -or $origin -ne $Entry.url.TrimEnd('/')) { $problems += "$($Entry.dir): origin is not $($Entry.url)" }
    $head = (Invoke-Git $target @('rev-parse', 'HEAD') -AllowFailure).output
    if ($head -ne $Entry.commit) { $problems += "$($Entry.dir): at $head, expected $($Entry.commit)" }
    if (-not (Test-Path -LiteralPath (Join-Path $target $Entry.probe))) { $problems += "$($Entry.dir): missing $($Entry.probe)" }
    $dirty = (Invoke-Git $target @('status', '--porcelain') -AllowFailure).output
    if ($dirty) { $problems += "$($Entry.dir): local changes`n$dirty" }
    return $problems
}

# Do not force -Encoding here: PowerShell 5.1 returns the whole array as one item when it does.
$lock = ConvertFrom-Json -InputObject (Get-Content -LiteralPath $lockFile -Raw)

if ($Check) {
    $problems = @()
    foreach ($entry in $lock) { $problems += @(Test-Reference $entry) }
    if ($problems.Count) {
        throw "docs-ref is incomplete or outdated. Run ./scripts/prepare-refs.ps1.`n$($problems -join "`n")"
    }
    Write-Host 'References are ready.'
    return
}

$temporary = $null
try {
    foreach ($entry in $lock) {
        $problems = @(Test-Reference $entry)
        if (-not $problems.Count) {
            Write-Host "[OK] $($entry.dir) $($entry.label) $($entry.commit.Substring(0, 7))"
            continue
        }

        $target = [IO.Path]::GetFullPath((Join-Path $refs $entry.dir))
        $exists = Test-Path -LiteralPath $target
        $fresh = $false
        if ($exists) {
            if (-not (Test-Path -LiteralPath (Join-Path $target '.git'))) {
                throw "$target exists and is not a git clone. Remove it or set it aside first."
            }
            $origin = Get-RemoteUrl $target
            if (-not $origin -or $origin -ne $entry.url.TrimEnd('/')) {
                throw "$target has origin '$origin', expected $($entry.url). Refusing to overwrite it."
            }
            $dirty = (Invoke-Git $target @('status', '--porcelain', '--untracked-files=no')).output
            if ($dirty) {
                throw "$target has local changes. Restore them with 'git -C $target checkout -- .' or remove the directory.`n$dirty"
            }
            $clone = $target
        } else {
            New-Item -ItemType Directory -Force $refs | Out-Null
            $temporary = Join-Path $refs ('.rbank-prepare-' + [guid]::NewGuid().ToString('N'))
            $clone = $temporary
            $fresh = $true
            Invoke-Git $project @('init', '--quiet', $clone) | Out-Null
            Invoke-Git $clone @('remote', 'add', 'origin', $entry.url) | Out-Null
            Write-Host "Cloning $($entry.dir) $($entry.commit.Substring(0, 7))"
        }

        if ($fresh -or (Invoke-Git $clone @('cat-file', '-e', "$($entry.commit)^{commit}") -AllowFailure).exit) {
            Invoke-Git $clone @('fetch', '--quiet', '--depth', '1', '--filter=blob:none', 'origin', $entry.commit) | Out-Null
        }
        Invoke-Git $clone (@('sparse-checkout', 'set', '--cone') + @($entry.paths)) | Out-Null
        Invoke-Git $clone @('checkout', '--quiet', '--detach', $entry.commit) | Out-Null

        if ($fresh) {
            Move-Item -LiteralPath $clone -Destination $target
            $temporary = $null
        }

        $problems = @(Test-Reference $entry)
        if ($problems.Count) { throw "Preparation failed: $($problems -join '; ')" }
        Write-Host "[OK] $($entry.dir) $($entry.label) $($entry.commit.Substring(0, 7))"
    }
    Write-Host "References are ready in $refs."
} finally {
    if ($temporary -and (Test-Path -LiteralPath $temporary)) { Remove-Item -LiteralPath $temporary -Recurse -Force }
}
