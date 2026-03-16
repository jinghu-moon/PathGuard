param(
    [string[]]$Abi = @('armeabi-v7a', 'arm64-v8a', 'x86', 'x86_64')
)

$ErrorActionPreference = 'Stop'

$scripts = Split-Path -Parent $MyInvocation.MyCommand.Path
$root = Split-Path -Parent $scripts
$dist = Join-Path $root 'dist'
$buildNativeScript = Join-Path $scripts 'build-native.ps1'

if (-not (Test-Path "$dist")) {
    New-Item -ItemType Directory -Path "$dist" | Out-Null
}

if (Test-Path "$buildNativeScript") {
    $abiArgs = @()
    if ($Abi) {
        $abiArgs = @('-Abi', ($Abi -join ','))
    }
    & powershell -ExecutionPolicy Bypass -File "$buildNativeScript" @abiArgs
}

$moduleProp = Join-Path $root 'module/module.prop'
$version = '0.2.0'

if (Test-Path "$moduleProp") {
    $match = Select-String -Path "$moduleProp" -Pattern '^version=(.+)$' | Select-Object -First 1
    if ($match) {
        $version = $match.Matches[0].Groups[1].Value.Trim()
    }
}

Add-Type -AssemblyName System.IO.Compression.FileSystem

$baseItems = @(
    'module.prop',
    'customize.sh',
    'post-fs-data.sh',
    'service.sh',
    'action.sh',
    'uninstall.sh',
    'skip_mount',
    'config'
)

$moduleRoot = Join-Path $root 'module'

$abis = $Abi
$knownAbis = @('armeabi-v7a', 'arm64-v8a', 'x86', 'x86_64')
$invalidAbis = $abis | Where-Object { $_ -and ($knownAbis -notcontains $_) }
if ($invalidAbis.Count -gt 0) {
    $invalidText = ($invalidAbis -join ', ')
    throw "Unknown ABI(s): $invalidText. Supported: $($knownAbis -join ', ')"
}

function Add-ZipFile {
    param(
        [System.IO.Compression.ZipArchive]$Archive,
        [string]$FullPath,
        [string]$EntryName
    )

    if (-not (Test-Path "$FullPath")) {
        return
    }
    [System.IO.Compression.ZipFileExtensions]::CreateEntryFromFile($Archive, $FullPath, $EntryName, [System.IO.Compression.CompressionLevel]::Optimal) | Out-Null
}

function Add-ZipDirectory {
    param(
        [System.IO.Compression.ZipArchive]$Archive,
        [string]$Root,
        [string]$RelativeDir
    )

    $fullDir = Join-Path $Root $RelativeDir
    if (-not (Test-Path "$fullDir")) {
        return
    }
    Get-ChildItem -Path "$fullDir" -File -Recurse | ForEach-Object {
        $relative = $_.FullName.Substring($Root.Length + 1).Replace('\', '/')
        Add-ZipFile -Archive $Archive -FullPath $_.FullName -EntryName $relative
    }
}

foreach ($abi in $abis) {
    $zipBase = "folder-manager-magisk-v$version-$abi"
    $zipPath = Join-Path $dist "$zipBase.zip"
    if (Test-Path "$zipPath") {
        $stamp = Get-Date -Format "yyyyMMddHHmmss"
        $zipPath = Join-Path $dist "$zipBase-$stamp.zip"
    }

    $archive = [System.IO.Compression.ZipFile]::Open($zipPath, 'Create')
    try {
        foreach ($item in $baseItems) {
            $fullPath = Join-Path $moduleRoot $item
            if (Test-Path "$fullPath") {
                if (Test-Path "$fullPath" -PathType Container) {
                    Get-ChildItem -Path "$fullPath" -File -Recurse | ForEach-Object {
                        $entryName = $_.FullName.Substring($moduleRoot.Length + 1).Replace('\', '/')
                        Add-ZipFile -Archive $archive -FullPath $_.FullName -EntryName $entryName
                    }
                } else {
                    Add-ZipFile -Archive $archive -FullPath $fullPath -EntryName $item
                }
            }
        }

        # README.md 从项目根目录打包
        $readmePath = Join-Path $root 'README.md'
        Add-ZipFile -Archive $archive -FullPath $readmePath -EntryName 'README.md'

        $zygiskSo = Join-Path $moduleRoot "zygisk/$abi.so"
        Add-ZipFile -Archive $archive -FullPath $zygiskSo -EntryName "zygisk/$abi.so"

        $daemonPath = Join-Path $moduleRoot "bin/$abi/folder_manager_daemon"
        Add-ZipFile -Archive $archive -FullPath $daemonPath -EntryName "bin/$abi/folder_manager_daemon"
    }
    finally {
        $archive.Dispose()
    }

    Write-Host "Created: $zipPath"
}
