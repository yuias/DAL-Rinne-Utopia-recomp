#Requires -Version 7
# Writes <BinDir>/config.json for dal_game when the game data is present and no config.json exists.
[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$BinDir,
    [Parameter(Mandatory)][string]$GameDir,
    [string]$EbootElf
)

$ErrorActionPreference = 'Stop'

$BinDir = [System.IO.Path]::GetFullPath($BinDir, $PWD.ProviderPath)
$GameDir = [System.IO.Path]::GetFullPath($GameDir, $PWD.ProviderPath)
if ($EbootElf) {
    $EbootElf = [System.IO.Path]::GetFullPath($EbootElf, $PWD.ProviderPath)
}

if (-not (Test-Path -LiteralPath (Join-Path $GameDir 'PS3_GAME/PARAM.SFO') -PathType Leaf)) {
    "config.json not written: $GameDir has no PS3_GAME/PARAM.SFO"
    return
}

$configPath = Join-Path $BinDir 'config.json'

# Windows paths compare case-insensitively; trailing separators are not significant.
function ConvertTo-ComparablePath([string]$path) {
    return [System.IO.Path]::TrimEndingDirectorySeparator([System.IO.Path]::GetFullPath($path)).ToLowerInvariant()
}

if (Test-Path -LiteralPath $configPath) {
    # An existing file is the user's; it is never modified.
    try {
        $existing = [System.IO.File]::ReadAllText($configPath) | ConvertFrom-Json -AsHashtable
    }
    catch {
        "config.json left unchanged: $configPath cannot be parsed"
        return
    }
    $existingGameDir = if ($existing -is [System.Collections.IDictionary]) { $existing['game_dir'] } else { $null }
    if (-not ($existingGameDir -is [string]) -or
        (ConvertTo-ComparablePath $existingGameDir) -ne (ConvertTo-ComparablePath $GameDir)) {
        "config.json left unchanged: game_dir in $configPath differs from $GameDir"
    }
    elseif ($EbootElf -and -not ($existing['elf'] -is [string])) {
        # Without elf, dal_game looks for EBOOT.elf in the game directory, not where the build put it.
        "config.json left unchanged: $configPath has no elf; add `"elf`": $($EbootElf | ConvertTo-Json)"
    }
    return
}

# sdata_cache and hdd_dir are left out: dal_game finds them next to bin/, and a written value would
# pin a path that a clean build deletes.
$config = [ordered]@{ game_dir = $GameDir }
if ($EbootElf) {
    $config['elf'] = $EbootElf
}
$json = ($config | ConvertTo-Json).ReplaceLineEndings("`n") + "`n"

try {
    $stream = [System.IO.File]::Open($configPath, [System.IO.FileMode]::CreateNew, [System.IO.FileAccess]::Write,
        [System.IO.FileShare]::None)
}
catch [System.IO.IOException] {
    if (Test-Path -LiteralPath $configPath) {
        "config.json left unchanged: $configPath was created meanwhile"
        return
    }
    throw
}
try {
    $bytes = [System.Text.UTF8Encoding]::new($false).GetBytes($json)
    $stream.Write($bytes, 0, $bytes.Length)
}
finally {
    $stream.Dispose()
}
"wrote $configPath"
