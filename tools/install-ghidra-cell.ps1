#Requires -Version 7
<#
.SYNOPSIS
Installs (or removes) the staged Cell PPU language module for Ghidra.

.DESCRIPTION
Copies the module built by `python tools/ps3tool.py ghidra-cell-lang build` into a place Ghidra loads
modules from. Installing is optional: it is only needed for the Ghidra GUI and for runs outside this
repository's tools. The script lists what it will change and asks before writing; it writes only
inside the destination (plus the missing `Extensions` folder of the settings directory). Running it
again with nothing to change prints `up to date` and touches nothing.

Ghidra aborts at startup when a module named CellPPU exists in more than one place, so the script
refuses to install while another copy exists.

Exit codes: 0 success (including up to date, cancelled and -WhatIf), 1 failure.

.PARAMETER GhidraDir
The Ghidra install directory. Defaults to $env:GHIDRA_INSTALL_DIR. There is no disk search.

.PARAMETER Target
User (default): <settings>/Extensions/CellPPU, which survives a replacement of the install directory.
Processors: <GhidraDir>/Ghidra/Processors/CellPPU.

.PARAMETER SettingsDir
Ghidra's user settings directory for this version. The default follows Ghidra's own rule:
XDG_CONFIG_HOME (with `<username>-ghidra` instead of `ghidra` when it lies outside the home
directory), else %APPDATA% on Windows, ~/Library on macOS, ~/.config elsewhere, then
ghidra/ghidra_<version>_<release>. For the User target it must already exist (start Ghidra once).

.PARAMETER Source
The staged module. Defaults to generated/ghidra/cell/CellPPU in this repository.

.PARAMETER Uninstall
Removes the installed module instead of installing it.
#>
[CmdletBinding(SupportsShouldProcess, ConfirmImpact = 'High', PositionalBinding = $false)]
param(
    [string]$GhidraDir = $env:GHIDRA_INSTALL_DIR,
    [ValidateSet('User', 'Processors')]
    [string]$Target = 'User',
    [string]$SettingsDir,
    [string]$Source,
    [switch]$Uninstall
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$moduleName = 'CellPPU'
$languageId = 'PowerPC:BE:64:CellPPU-32addr'
$sep = [System.IO.Path]::DirectorySeparatorChar
# Windows file systems ignore case; Ghidra's own paths there are compared the same way.
$comparison = [System.StringComparison]::Ordinal
$comparer = [System.StringComparer]::Ordinal
if ($IsWindows) {
    $comparison = [System.StringComparison]::OrdinalIgnoreCase
    $comparer = [System.StringComparer]::OrdinalIgnoreCase
}

function Read-Properties {
    param([string]$Path)
    $props = @{}
    foreach ($line in [System.IO.File]::ReadAllLines($Path)) {
        $text = $line.Trim()
        if ($text -eq '' -or $text.StartsWith('#') -or $text.StartsWith('!')) { continue }
        $match = [regex]::Match($text, '^([^=:\s]+)\s*[=:]?\s*(.*)$')
        if ($match.Success) { $props[$match.Groups[1].Value] = $match.Groups[2].Value.Trim() }
    }
    $props
}

function Test-Exists {
    param([string]$Path)
    [System.IO.Directory]::Exists($Path) -or [System.IO.File]::Exists($Path)
}

function Test-Inside {
    param([string]$Root, [string]$Path)
    $prefix = $Root.TrimEnd('\', '/') + $sep
    $Path.StartsWith($prefix, $comparison)
}

function Test-ReparsePoint {
    param([string]$Path)
    ([System.IO.File]::GetAttributes($Path) -band [System.IO.FileAttributes]::ReparsePoint) -ne 0
}

function Get-HomeDir {
    $homeDir = if ($IsWindows) { $env:USERPROFILE } else { $env:HOME }
    if (-not $homeDir) { $homeDir = [System.Environment]::GetFolderPath('UserProfile') }
    if (-not $homeDir) { throw 'cannot determine the home directory; pass -SettingsDir' }
    [System.IO.Path]::GetFullPath($homeDir, $PWD.ProviderPath)
}

# Like Ghidra's getEnvFile: the value is trimmed, a blank one counts as unset, a relative one is an error.
function Get-EnvDir {
    param([string]$Name)
    $value = [System.Environment]::GetEnvironmentVariable($Name)
    if ($null -eq $value -or $value.Trim() -eq '') { return $null }
    $value = $value.Trim()
    if (-not [System.IO.Path]::IsPathRooted($value)) {
        throw "$Name must be an absolute path (got '$value'); fix it or pass -SettingsDir"
    }
    [System.IO.Path]::GetFullPath($value)
}

# Ghidra's SystemUtilities.getCleanUserName: no spaces, no leading DOMAIN\ or path part. The environment
# is read first (it matches the OS value on a normal login), which also lets the tests set the name.
function Get-CleanUserName {
    $name = if ($IsWindows) { $env:USERNAME } else { $env:USER }
    if ([string]::IsNullOrWhiteSpace($name)) { $name = [System.Environment]::UserName }
    ($name -replace ' ', '') -replace '^.*[\\/]', ''
}

# Mirrors the environment part of Ghidra's settings directory rule.
function Get-DefaultSettingsDir {
    param([string]$Version, [string]$Release)
    $leaf = "ghidra_${Version}_${Release}"
    $homeDir = Get-HomeDir
    $xdg = Get-EnvDir 'XDG_CONFIG_HOME'
    if ($xdg) {
        $inside = ($xdg.TrimEnd('\', '/') -eq $homeDir.TrimEnd('\', '/')) -or (Test-Inside $homeDir $xdg)
        $name = if ($inside) { 'ghidra' } else { "$(Get-CleanUserName)-ghidra" }
        return [System.IO.Path]::Combine($xdg, $name, $leaf)
    }
    if ($IsWindows) {
        $appData = Get-EnvDir 'APPDATA'
        if (-not $appData) { throw 'APPDATA is not set; pass -SettingsDir' }
        return [System.IO.Path]::Combine($appData, 'ghidra', $leaf)
    }
    if ($IsMacOS) { return [System.IO.Path]::Combine($homeDir, 'Library', 'ghidra', $leaf) }
    [System.IO.Path]::Combine($homeDir, '.config', 'ghidra', $leaf)
}

# Maps relative path (with '/') to full path for every file below $Dir.
function Get-TreeFiles {
    param([string]$Dir, [switch]$RejectReparse)
    $files = [System.Collections.Generic.Dictionary[string, string]]::new($comparer)
    $pending = [System.Collections.Generic.Stack[string]]::new()
    $pending.Push($Dir)
    while ($pending.Count -gt 0) {
        $current = $pending.Pop()
        foreach ($entry in [System.IO.Directory]::EnumerateFileSystemEntries($current)) {
            if ($RejectReparse -and (Test-ReparsePoint $entry)) {
                throw "refusing to use $Dir`: $entry is a symbolic link or junction"
            }
            if ([System.IO.Directory]::Exists($entry)) {
                $pending.Push($entry)
            }
            else {
                $files[[System.IO.Path]::GetRelativePath($Dir, $entry).Replace('\', '/')] = $entry
            }
        }
    }
    $files
}

# Ordinal order keeps the plan identical across cultures.
function Get-SortedKeys {
    param($Table)
    $keys = [string[]]$Table.Keys
    [System.Array]::Sort($keys, [System.StringComparer]::Ordinal)
    $keys
}

function Get-Sha256 {
    param([string]$Path)
    (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash
}

try {
    if ([string]::IsNullOrWhiteSpace($GhidraDir)) { throw 'pass -GhidraDir or set GHIDRA_INSTALL_DIR' }
    $ghidraRoot = [System.IO.Path]::GetFullPath($GhidraDir, $PWD.ProviderPath)
    $appPropsFile = [System.IO.Path]::Combine($ghidraRoot, 'Ghidra', 'application.properties')
    $powerPcDir = [System.IO.Path]::Combine($ghidraRoot, 'Ghidra', 'Processors', 'PowerPC')
    if (-not [System.IO.File]::Exists($appPropsFile) -or -not [System.IO.Directory]::Exists($powerPcDir)) {
        throw "not a Ghidra install: $ghidraRoot"
    }
    $appProps = Read-Properties $appPropsFile
    $version = $appProps['application.version']
    $release = $appProps['application.release.name']
    if (-not $version -or -not $release) {
        throw "application.version or application.release.name is missing in $appPropsFile"
    }

    if ($SettingsDir) {
        $settings = [System.IO.Path]::GetFullPath($SettingsDir, $PWD.ProviderPath)
    }
    else {
        $settings = [System.IO.Path]::GetFullPath((Get-DefaultSettingsDir $version $release))
    }

    $userDest = [System.IO.Path]::GetFullPath([System.IO.Path]::Combine($settings, 'Extensions', $moduleName))
    $procDest = [System.IO.Path]::GetFullPath(
        [System.IO.Path]::Combine($ghidraRoot, 'Ghidra', 'Processors', $moduleName))
    $ghidraExtDest = [System.IO.Path]::GetFullPath(
        [System.IO.Path]::Combine($ghidraRoot, 'Ghidra', 'Extensions', $moduleName))
    if ($Target -eq 'User') {
        $root = $settings
        $dest = $userDest
        $others = @($procDest, $ghidraExtDest)
    }
    else {
        $root = $ghidraRoot
        $dest = $procDest
        $others = @($userDest, $ghidraExtDest)
    }

    "ghidra: $version $release ($ghidraRoot)"
    "target: $dest"

    # Safety checks come before any write.
    if ([System.IO.Path]::GetFileName($dest) -ne $moduleName -or -not (Test-Inside $root $dest)) {
        throw "destination is not a $moduleName folder inside $root"
    }
    if ($Target -eq 'User' -and -not $Uninstall -and -not [System.IO.Directory]::Exists($settings)) {
        throw "settings directory not found: $settings; start Ghidra once or pass -SettingsDir"
    }
    $walk = $root
    foreach ($segment in [System.IO.Path]::GetRelativePath($root, $dest).Split($sep)) {
        $walk = [System.IO.Path]::Combine($walk, $segment)
        if (-not (Test-Exists $walk)) { break }
        if (Test-ReparsePoint $walk) { throw "refusing to write through a symbolic link or junction: $walk" }
    }
    $destExists = Test-Exists $dest
    if ($destExists) {
        $destProps = [System.IO.Path]::Combine($dest, 'extension.properties')
        if (-not [System.IO.Directory]::Exists($dest) -or -not [System.IO.File]::Exists($destProps) -or
            (Read-Properties $destProps)['name'] -ne $moduleName) {
            throw "refusing to touch $dest`: extension.properties does not name $moduleName"
        }
    }

    if ($Uninstall) {
        if (-not $destExists) {
            "not installed: $dest"
            exit 0
        }
        $destFiles = Get-TreeFiles $dest -RejectReparse
        "plan: remove $($destFiles.Count) files"
        if (-not $PSCmdlet.ShouldProcess($dest, 'uninstall Cell PPU language')) {
            'cancelled: nothing changed'
            exit 0
        }
        [System.IO.Directory]::Delete($dest, $true)
        "uninstalled: $dest"
        exit 0
    }

    foreach ($other in $others) {
        if (Test-Exists $other) {
            throw "$moduleName already exists at $other; Ghidra cannot load two copies; uninstall it first"
        }
    }

    if (-not $Source) {
        $Source = [System.IO.Path]::Combine($PSScriptRoot, '..', 'generated', 'ghidra', 'cell', $moduleName)
    }
    $sourceDir = [System.IO.Path]::GetFullPath($Source, $PWD.ProviderPath)
    $buildHint = 'run python tools/ps3tool.py ghidra-cell-lang build'
    $sourceProps = [System.IO.Path]::Combine($sourceDir, 'extension.properties')
    if (-not [System.IO.File]::Exists($sourceProps)) {
        throw "staged module not found or not built: $sourceDir; $buildHint"
    }
    $sourceInfo = Read-Properties $sourceProps
    if ($sourceInfo['name'] -ne $moduleName) {
        throw "$sourceDir is not a $moduleName module (extension.properties name); $buildHint"
    }
    if ($sourceInfo['version'] -ne $version) {
        throw "staged module is for Ghidra '$($sourceInfo['version'])' but the target is $version; $buildHint"
    }
    if (-not [System.IO.File]::Exists([System.IO.Path]::Combine($sourceDir, 'data', 'languages', 'ppc_cell.sla'))) {
        throw "staged module has no compiled ppc_cell.sla: $sourceDir; $buildHint"
    }
    if ($sourceDir -eq $dest -or (Test-Inside $sourceDir $dest) -or (Test-Inside $dest $sourceDir)) {
        throw "the source and the destination overlap: $sourceDir"
    }

    $sourceFiles = Get-TreeFiles $sourceDir -RejectReparse
    $destFiles = [System.Collections.Generic.Dictionary[string, string]]::new($comparer)
    if ($destExists) { $destFiles = Get-TreeFiles $dest -RejectReparse }
    $added = [System.Collections.Generic.List[string]]::new()
    $updated = [System.Collections.Generic.List[string]]::new()
    $removed = [System.Collections.Generic.List[string]]::new()
    $unchanged = 0
    foreach ($rel in (Get-SortedKeys $sourceFiles)) {
        if (-not $destFiles.ContainsKey($rel)) { $added.Add($rel) }
        elseif ((Get-Sha256 $sourceFiles[$rel]) -ne (Get-Sha256 $destFiles[$rel])) { $updated.Add($rel) }
        else { $unchanged++ }
    }
    foreach ($rel in (Get-SortedKeys $destFiles)) {
        if (-not $sourceFiles.ContainsKey($rel)) { $removed.Add($rel) }
    }

    if ($added.Count + $updated.Count + $removed.Count -eq 0) {
        "up to date: $dest"
        exit 0
    }

    "plan: added $($added.Count), updated $($updated.Count), removed $($removed.Count), unchanged $unchanged"
    foreach ($rel in $added) { "  added $rel" }
    foreach ($rel in $updated) { "  updated $rel" }
    foreach ($rel in $removed) { "  removed $rel" }

    $action = "install Cell PPU language: added $($added.Count), updated $($updated.Count), removed $($removed.Count)"
    if (-not $PSCmdlet.ShouldProcess($dest, $action)) {
        'cancelled: nothing changed'
        exit 0
    }

    # The .sla goes last so an interrupted copy never leaves a compiled file that is newer than its sources.
    $toCopy = @($added) + @($updated)
    $ordered = @($toCopy | Where-Object { -not $_.EndsWith('.sla', $comparison) }) +
        @($toCopy | Where-Object { $_.EndsWith('.sla', $comparison) })
    foreach ($rel in $ordered) {
        $copyTarget = [System.IO.Path]::GetFullPath([System.IO.Path]::Combine($dest, $rel.Replace('/', [string]$sep)))
        if (-not (Test-Inside $dest $copyTarget)) { throw "refusing to write outside the destination: $rel" }
        [void][System.IO.Directory]::CreateDirectory([System.IO.Path]::GetDirectoryName($copyTarget))
        [System.IO.File]::Copy($sourceFiles[$rel], $copyTarget, $true)
    }
    foreach ($rel in $removed) { [System.IO.File]::Delete($destFiles[$rel]) }
    foreach ($dir in ([System.IO.Directory]::GetDirectories($dest, '*', [System.IO.SearchOption]::AllDirectories) |
            Sort-Object -Property Length -Descending)) {
        if ([System.IO.Directory]::Exists($dir) -and
            -not [System.IO.Directory]::EnumerateFileSystemEntries($dir).GetEnumerator().MoveNext()) {
            [System.IO.Directory]::Delete($dir)
        }
    }

    # Ghidra recompiles a .sla that is older than its sources, so make it the newest file.
    $installed = Get-TreeFiles $dest
    $newest = [System.DateTime]::MinValue
    foreach ($rel in $installed.Keys) {
        if (-not $rel.EndsWith('.sla', $comparison)) {
            $time = [System.IO.File]::GetLastWriteTimeUtc($installed[$rel])
            if ($time -gt $newest) { $newest = $time }
        }
    }
    $stamp = [System.DateTime]::UtcNow
    if ($stamp -le $newest) { $stamp = $newest.AddSeconds(1) }
    foreach ($rel in $installed.Keys) {
        if ($rel.EndsWith('.sla', $comparison)) { [System.IO.File]::SetLastWriteTimeUtc($installed[$rel], $stamp) }
    }

    "installed: restart Ghidra to load $languageId"
    exit 0
}
catch {
    [Console]::Error.WriteLine("install-ghidra-cell: $($_.Exception.Message)")
    exit 1
}
