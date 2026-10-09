#Requires -Version 7
<#
.SYNOPSIS
Fetches the pinned Microsoft.GameInput NuGet package (cmake/gameinput.cmake) into
generated/deps/gameinput.

.DESCRIPTION
Downloads the pinned .nupkg, verifies its SHA-256, extracts it into
generated/deps/gameinput/<version>/ and writes a .verified-sha256 marker there. A second run is a
no-op.

Exit codes: 0 success (including no-op), 1 failure, 2 usage error.

.PARAMETER From
Use this local .nupkg instead of downloading (offline use, or a kept copy of the pinned package).
It gets the same SHA-256 check and is never modified or deleted.

.PARAMETER Force
Discard an existing extraction and extract again.
#>
[CmdletBinding(PositionalBinding = $false)]
param(
    [string]$From,
    [switch]$Force,
    # Catches unknown options so they become a usage error (exit 2) instead of a binding error.
    [Parameter(ValueFromRemainingArguments)]
    [string[]]$Unexpected
)

$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'

if ($Unexpected -or ($PSBoundParameters.ContainsKey('From') -and [string]::IsNullOrWhiteSpace($From))) {
    if ($Unexpected) { [Console]::Error.WriteLine("fetch-gameinput: unexpected argument: $($Unexpected -join ' ')") }
    else { [Console]::Error.WriteLine("fetch-gameinput: -From needs a path") }
    [Console]::Error.WriteLine("usage: tools/fetch-gameinput.ps1 [-From <nupkg>] [-Force]")
    exit 2
}

Add-Type -AssemblyName System.IO.Compression.FileSystem

function Get-Sha256 {
    param([string]$Path)
    (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash.ToLowerInvariant()
}

# Extracts every entry under $Destination, rejecting anything that would land outside it.
function Expand-ZipSafely {
    param([string]$ZipPath, [string]$Destination)
    $destRoot = [System.IO.Path]::GetFullPath($Destination).TrimEnd('\', '/') + [System.IO.Path]::DirectorySeparatorChar
    $archive = [System.IO.Compression.ZipFile]::OpenRead($ZipPath)
    try {
        foreach ($entry in $archive.Entries) {
            $name = $entry.FullName
            $segments = $name -split '[\\/]'
            if ([System.IO.Path]::IsPathRooted($name) -or $name.Contains(':') -or ($segments -contains '..')) {
                throw "unsafe entry in the archive: '$name'"
            }
            $target = [System.IO.Path]::GetFullPath([System.IO.Path]::Combine($destRoot, $name))
            if (-not $target.StartsWith($destRoot, [System.StringComparison]::OrdinalIgnoreCase)) {
                throw "archive entry escapes the target directory: '$name'"
            }
            if ($name.EndsWith('/') -or $name.EndsWith('\')) {
                [void][System.IO.Directory]::CreateDirectory($target)
                continue
            }
            [void][System.IO.Directory]::CreateDirectory([System.IO.Path]::GetDirectoryName($target))
            [System.IO.Compression.ZipFileExtensions]::ExtractToFile($entry, $target, $true)
        }
    }
    finally {
        $archive.Dispose()
    }
}

function Read-Pin {
    param([string]$Text, [string]$Name, [string]$Pattern)
    $match = [regex]::Match($Text, "set\(\s*$Name\s+`"($Pattern)`"\s*\)")
    if (-not $match.Success) { throw "$Name is missing or malformed in cmake/gameinput.cmake" }
    $match.Groups[1].Value
}

function Invoke-Com {
    param($Object, [string]$Name, [string]$Kind, $Arguments = $null)
    $Object.GetType().InvokeMember($Name, $Kind, $null, $Object, $Arguments)
}

# Runs an MSI SQL query and returns each row as an array of strings.
function Select-MsiRows {
    param($Database, [string]$Sql)
    $view = Invoke-Com $Database 'OpenView' 'InvokeMethod' @($Sql)
    try {
        [void](Invoke-Com $view 'Execute' 'InvokeMethod')
        while ($true) {
            $record = Invoke-Com $view 'Fetch' 'InvokeMethod'
            if ($null -eq $record) { break }
            $count = Invoke-Com $record 'FieldCount' 'GetProperty'
            , @(for ($i = 1; $i -le $count; $i++) { Invoke-Com $record 'StringData' 'GetProperty' @([int]$i) })
        }
    }
    finally {
        [void](Invoke-Com $view 'Close' 'InvokeMethod')
    }
}

# Writes the embedded cabinet that holds the x64 GameInputRedist.dll to $CabinetPath and returns the
# file's key in the MSI, which is also its name inside the cabinet. The Windows Installer COM API
# opens the database read-only; nothing is installed.
function Save-RedistCabinet {
    param([string]$MsiPath, [string]$CabinetPath)
    $installer = $database = $view = $record = $null
    try {
        $installer = New-Object -ComObject WindowsInstaller.Installer
        $database = Invoke-Com $installer 'OpenDatabase' 'InvokeMethod' @($MsiPath, [int]0)

        # Several architectures share the file name; the x64 one is the one in the "x64" directory.
        $rows = @(Select-MsiRows $database ("SELECT File.File, File.FileName, File.Sequence FROM File, Component, Directory " +
            "WHERE File.Component_ = Component.Component AND Component.Directory_ = Directory.Directory " +
            "AND Directory.DefaultDir = 'x64'") | Where-Object { $_[1].EndsWith('|GameInputRedist.dll') -or $_[1] -eq 'GameInputRedist.dll' })
        if ($rows.Count -ne 1) { throw "expected one x64 GameInputRedist.dll in the MSI, found $($rows.Count)" }
        $fileKey = $rows[0][0]
        $sequence = [int]$rows[0][2]

        # The cabinet that holds the file is the one with the smallest LastSequence >= its sequence.
        $media = @(Select-MsiRows $database 'SELECT LastSequence, Cabinet FROM Media' |
            Where-Object { [int]$_[0] -ge $sequence -and $_[1].StartsWith('#') } |
            Sort-Object { [int]$_[0] })
        if ($media.Count -eq 0) { throw "no embedded cabinet in the MSI holds sequence $sequence" }
        $cabinetName = $media[0][1].Substring(1)

        $view = Invoke-Com $database 'OpenView' 'InvokeMethod' @("SELECT Data FROM _Streams WHERE Name = '$cabinetName'")
        [void](Invoke-Com $view 'Execute' 'InvokeMethod')
        $record = Invoke-Com $view 'Fetch' 'InvokeMethod'
        if ($null -eq $record) { throw "stream '$cabinetName' not found in the MSI" }
        $remaining = [int](Invoke-Com $record 'DataSize' 'GetProperty' @([int]1))
        $out = [System.IO.File]::Create($CabinetPath)
        try {
            # Format 1 (msiReadStreamBytes) returns one character per byte.
            while ($remaining -gt 0) {
                $text = Invoke-Com $record 'ReadStream' 'InvokeMethod' @([int]1, [int][Math]::Min($remaining, 65536), [int]1)
                if ($null -eq $text -or $text.Length -eq 0) { throw "short read of the MSI stream '$cabinetName'" }
                $bytes = [System.Text.Encoding]::Latin1.GetBytes($text)
                $out.Write($bytes, 0, $bytes.Length)
                $remaining -= $bytes.Length
            }
        }
        finally {
            $out.Dispose()
        }
        $fileKey
    }
    finally {
        # Release the COM objects so the MSI file is closed before the package directory is moved.
        $record = $view = $database = $installer = $null
        [GC]::Collect()
        [GC]::WaitForPendingFinalizers()
    }
}

# Extracts the x64 GameInputRedist.dll from the package's MSI without installing it: the cabinet is
# read through the Windows Installer COM API and expand.exe unpacks the file from it. The package
# ships no loose runtime DLL.
function Export-RedistDll {
    param([string]$MsiPath, [string]$WorkDir, [string]$Destination, [string]$ExpectedHash)
    $cabinetPath = Join-Path $WorkDir 'redist.cab'
    $fileKey = Save-RedistCabinet $MsiPath $cabinetPath

    $unpackDir = Join-Path $WorkDir 'unpacked'
    [void][System.IO.Directory]::CreateDirectory($unpackDir)
    $expand = Join-Path $env:SystemRoot 'System32/expand.exe'
    $expandOutput = & $expand $cabinetPath "-F:$fileKey" $unpackDir
    if ($LASTEXITCODE -ne 0) { throw "expand.exe failed ($LASTEXITCODE): $expandOutput" }
    $unpacked = Join-Path $unpackDir $fileKey
    if (-not (Test-Path -LiteralPath $unpacked -PathType Leaf)) { throw "expand.exe did not produce '$fileKey'" }

    # PE check: the MZ header, then the COFF machine field of the PE header (0x8664 = x64).
    $header = [System.IO.File]::ReadAllBytes($unpacked)
    $peOffset = if ($header.Length -ge 0x40) { [BitConverter]::ToInt32($header, 0x3C) } else { 0 }
    if ($header.Length -lt 0x40 -or $header[0] -ne 0x4D -or $header[1] -ne 0x5A -or
        $peOffset -lt 0 -or $peOffset + 6 -gt $header.Length -or
        [BitConverter]::ToUInt16($header, $peOffset + 4) -ne 0x8664) {
        throw "the extracted GameInputRedist.dll is not an x64 PE file"
    }
    $actual = Get-Sha256 $unpacked
    if ($actual -ne $ExpectedHash) {
        throw "SHA-256 mismatch for the extracted GameInputRedist.dll: expected $ExpectedHash, got $actual"
    }
    [void][System.IO.Directory]::CreateDirectory([System.IO.Path]::GetDirectoryName($Destination))
    Move-Item -LiteralPath $unpacked -Destination $Destination -Force
}

$staging = $null
try {
    $repoRoot = [System.IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
    $pinFile = Join-Path $repoRoot 'cmake/gameinput.cmake'
    if (-not (Test-Path -LiteralPath $pinFile)) { throw "pin file not found: cmake/gameinput.cmake" }
    $pinText = Get-Content -LiteralPath $pinFile -Raw
    $version = Read-Pin $pinText 'DAL_GAMEINPUT_VERSION' '[0-9]+\.[0-9]+\.[0-9]+'
    $expectedHash = (Read-Pin $pinText 'DAL_GAMEINPUT_SHA256' '[0-9A-Fa-f]{64}').ToLowerInvariant()
    $redistHash = (Read-Pin $pinText 'DAL_GAMEINPUT_REDIST_SHA256' '[0-9A-Fa-f]{64}').ToLowerInvariant()
    $packageFile = "microsoft.gameinput.$version.nupkg"

    $depsDir = Join-Path $repoRoot 'generated/deps/gameinput'
    $downloadDir = Join-Path $depsDir 'download'
    $finalDir = Join-Path $depsDir $version
    $markerFile = Join-Path $finalDir '.verified-sha256'

    # Resolved against the caller's location; verified before anything on disk is touched.
    $packagePath = $null
    if ($From) {
        $packagePath = [System.IO.Path]::GetFullPath($From, $PWD.ProviderPath)
        if (-not (Test-Path -LiteralPath $packagePath -PathType Leaf)) { throw "-From file not found: $From" }
        $actual = Get-Sha256 $packagePath
        if ($actual -ne $expectedHash) {
            throw "SHA-256 mismatch for -From file '$From': expected $expectedHash, got $actual"
        }
    }

    # The side-by-side DLL is part of the extraction: a missing or altered DLL means extracting again.
    $redistDll = Join-Path $finalDir 'sxs/x64/GameInputRedist.dll'
    $verified = (Test-Path -LiteralPath $markerFile -PathType Leaf) -and
        ((Get-Content -LiteralPath $markerFile -Raw).Trim() -eq $expectedHash) -and
        (Test-Path -LiteralPath $redistDll -PathType Leaf) -and
        ((Get-Sha256 $redistDll) -eq $redistHash)
    if ($verified -and -not $Force) {
        "gameinput: up to date ($version)"
        exit 0
    }

    if (-not $packagePath) {
        $packagePath = Join-Path $downloadDir $packageFile
        # A cached download is reused only if it still matches the pin.
        if ((Test-Path -LiteralPath $packagePath -PathType Leaf) -and (Get-Sha256 $packagePath) -ne $expectedHash) {
            Remove-Item -LiteralPath $packagePath -Force
        }
        if (-not (Test-Path -LiteralPath $packagePath -PathType Leaf)) {
            $url = "https://api.nuget.org/v3-flatcontainer/microsoft.gameinput/$version/$packageFile"
            [void][System.IO.Directory]::CreateDirectory($downloadDir)
            $partPath = "$packagePath.part"
            "gameinput: downloading $url"
            try {
                Invoke-WebRequest -Uri $url -OutFile $partPath
            }
            catch {
                if (Test-Path -LiteralPath $partPath) { Remove-Item -LiteralPath $partPath -Force }
                throw "download failed: $($_.Exception.Message)`n  The pinned version may have been unlisted; use -From with a copy of the .nupkg."
            }
            $actual = Get-Sha256 $partPath
            if ($actual -ne $expectedHash) {
                Remove-Item -LiteralPath $partPath -Force
                throw "SHA-256 mismatch for the download: expected $expectedHash, got $actual"
            }
            Move-Item -LiteralPath $partPath -Destination $packagePath -Force
        }
    }

    # Extract beside the final location, then move into place, so a failure leaves no partial tree.
    [void][System.IO.Directory]::CreateDirectory($depsDir)
    $staging = Join-Path $depsDir ".staging-$PID"
    if (Test-Path -LiteralPath $staging) { Remove-Item -LiteralPath $staging -Recurse -Force }
    $extracted = Join-Path $staging 'package'
    [void][System.IO.Directory]::CreateDirectory($extracted)
    "gameinput: extracting $version"
    Expand-ZipSafely $packagePath $extracted

    foreach ($path in 'native/include/GameInput.h', 'native/lib/x64/GameInput.lib', 'LICENSE.txt',
        'NOTICE.txt', 'redist/GameInputRedist.msi') {
        if (-not (Test-Path -LiteralPath (Join-Path $extracted $path) -PathType Leaf)) {
            throw "the package does not contain '$path'"
        }
    }
    "gameinput: extracting x64 GameInputRedist.dll from the MSI"
    $workDir = Join-Path $staging 'msi-work'
    [void][System.IO.Directory]::CreateDirectory($workDir)
    Export-RedistDll (Join-Path $extracted 'redist/GameInputRedist.msi') $workDir `
        (Join-Path $extracted 'sxs/x64/GameInputRedist.dll') $redistHash
    if (Test-Path -LiteralPath $finalDir) { Remove-Item -LiteralPath $finalDir -Recurse -Force }
    Move-Item -LiteralPath $extracted -Destination $finalDir
    Set-Content -LiteralPath $markerFile -Value $expectedHash -NoNewline
    "gameinput: ready in generated/deps/gameinput/$version"
    exit 0
}
catch {
    [Console]::Error.WriteLine("fetch-gameinput: $($_.Exception.Message)")
    exit 1
}
finally {
    if ($staging -and (Test-Path -LiteralPath $staging)) {
        Remove-Item -LiteralPath $staging -Recurse -Force -ErrorAction SilentlyContinue
    }
}
