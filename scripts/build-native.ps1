param(
    [string[]]$Abi = @('armeabi-v7a', 'arm64-v8a', 'x86', 'x86_64')
)

$ErrorActionPreference = 'Stop'

$scripts = Split-Path -Parent $MyInvocation.MyCommand.Path
$root = Split-Path -Parent $scripts
$nativeRoot = Join-Path $root 'native'
$zygiskRoot = Join-Path $root 'module/zygisk'

function Find-NdkBuild {
    $candidates = @()

    $command1 = Get-Command 'ndk-build' -ErrorAction SilentlyContinue
    if ($command1) {
        $candidates += $command1.Source
    }

    $command2 = Get-Command 'ndk-build.cmd' -ErrorAction SilentlyContinue
    if ($command2) {
        $candidates += $command2.Source
    }

    if ($env:ANDROID_NDK_HOME) {
        $candidates += (Join-Path $env:ANDROID_NDK_HOME 'ndk-build.cmd')
        $candidates += (Join-Path $env:ANDROID_NDK_HOME 'ndk-build')
    }

    if ($env:ANDROID_NDK_ROOT) {
        $candidates += (Join-Path $env:ANDROID_NDK_ROOT 'ndk-build.cmd')
        $candidates += (Join-Path $env:ANDROID_NDK_ROOT 'ndk-build')
    }

    foreach ($candidate in $candidates) {
        if ($candidate -and (Test-Path "$candidate")) {
            return $candidate
        }
    }

    throw 'ndk-build not found. Please install Android NDK and ensure ndk-build is available.'
}

if (-not (Test-Path "$nativeRoot/Android.mk")) {
    throw 'Missing native/Android.mk. Native build cannot continue.'
}

$ndkBuild = Find-NdkBuild

New-Item -ItemType Directory -Force -Path "$zygiskRoot" | Out-Null

$abis = $Abi
$knownAbis = @('armeabi-v7a', 'arm64-v8a', 'x86', 'x86_64')
$invalidAbis = $abis | Where-Object { $_ -and ($knownAbis -notcontains $_) }
if ($invalidAbis.Count -gt 0) {
    $invalidText = ($invalidAbis -join ', ')
    throw "Unknown ABI(s): $invalidText. Supported: $($knownAbis -join ', ')"
}

$abiList = $abis -join ' '
if ($abiList.Trim().Length -eq 0) {
    throw 'No ABI specified. Use -Abi arm64-v8a or -Abi armeabi-v7a,arm64-v8a.'
}

Write-Host "Using ndk-build: $ndkBuild"
& "$ndkBuild" -C "$nativeRoot" `
    "NDK_PROJECT_PATH=$nativeRoot" `
    "APP_BUILD_SCRIPT=$nativeRoot/Android.mk" `
    "NDK_APPLICATION_MK=$nativeRoot/Application.mk" `
    "APP_ABI=$abiList"

if ($LASTEXITCODE -ne 0) {
    throw "ndk-build failed with exit code: $LASTEXITCODE"
}

foreach ($abi in $abis) {
    $source = Join-Path $nativeRoot "libs/$abi/libfolder_manager.so"
    if (Test-Path "$source") {
        $target = Join-Path $zygiskRoot "$abi.so"
        Copy-Item -Force "$source" "$target"
        Write-Host "Copied: $source -> $target"
    }

    $daemonSource = Join-Path $nativeRoot "libs/$abi/folder_manager_daemon"
    if (Test-Path "$daemonSource") {
        $daemonDir = Join-Path $root "module/bin/$abi"
        New-Item -ItemType Directory -Force -Path "$daemonDir" | Out-Null
        $daemonTarget = Join-Path $daemonDir "folder_manager_daemon"
        Copy-Item -Force "$daemonSource" "$daemonTarget"
        Write-Host "Copied: $daemonSource -> $daemonTarget"
    }
}
