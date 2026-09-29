param(
    [switch]$Setup,
    [switch]$Pristine,
    [switch]$InternalClock,
    [ValidateSet('Off', 'Bridge', 'Panel')]
    [string]$ScreenMode = 'Off',
    [string]$SdkPath = $env:ZEPHYR_SDK_INSTALL_DIR
)

$ErrorActionPreference = 'Stop'
$root = Split-Path (Split-Path $PSScriptRoot -Parent) -Parent
$python = Join-Path $root '.tools\venv\Scripts\python.exe'
$oldPath = $env:PATH
$oldBase = $env:ZEPHYR_BASE
$oldSdk = $env:ZEPHYR_SDK_INSTALL_DIR
$oldOverlay = $env:DTC_OVERLAY_FILE
$oldExtraOverlay = $env:EXTRA_DTC_OVERLAY_FILE
$oldConf = $env:EXTRA_CONF_FILE
$oldBaseConf = $env:CONF_FILE

function Invoke-Checked {
    param([string]$File, [string[]]$Arguments)
    & $File @Arguments
    if ($LASTEXITCODE -ne 0) {
        throw "$File failed with exit code $LASTEXITCODE"
    }
}

Push-Location $root
try {
    if ($Setup) {
        if (-not (Test-Path -LiteralPath $python)) {
            Invoke-Checked 'python' @('-m', 'venv', '.tools/venv')
        }
        Invoke-Checked $python @('-m', 'pip', 'install', 'west==1.5.0')
        if (-not (Test-Path -LiteralPath '.west/config')) {
            Invoke-Checked $python @('-m', 'west', 'init', '-l', 'firmware')
        }
        Invoke-Checked $python @('-m', 'west', 'update', '--narrow', '-o=--depth=1')
        Invoke-Checked $python @('-m', 'pip', 'install', '-r',
            '.tools/zephyr/scripts/requirements-base.txt')
    }
    if (-not (Test-Path -LiteralPath $python) -or
        -not (Test-Path -LiteralPath '.tools/zephyr/CMakeLists.txt')) {
        throw 'Dependencies missing. Run with -Setup first.'
    }
    $env:PATH = (Split-Path $python -Parent) + ';' + $oldPath
    $env:ZEPHYR_BASE = Join-Path $root '.tools\zephyr'
    if ($SdkPath) {
        $env:ZEPHYR_SDK_INSTALL_DIR = (Resolve-Path -LiteralPath $SdkPath).Path
    }

    Invoke-Checked $python @('firmware/tools/code_graph.py')
    $buildDir = if ($InternalClock) { 'firmware/build-hsi' } else { 'firmware/build' }
    $overlays = @()
    if ($InternalClock) { $overlays += 'boards/ht_main_hsi.overlay' }
    $conf = ''
    $baseConf = 'prj.conf'
    if ($ScreenMode -ne 'Off') {
        $mode = $ScreenMode.ToLowerInvariant()
        $buildDir += "-screen-$mode"
        $overlays += 'boards/ht_main_screen.overlay'
        $conf = "screen-$mode.conf"
        if ($ScreenMode -eq 'Bridge') {
            $baseConf = $conf
            $conf = ''
        }
    }
    $overlay = $overlays -join ';'
    $env:DTC_OVERLAY_FILE = $null
    $env:EXTRA_DTC_OVERLAY_FILE = $null
    $env:EXTRA_CONF_FILE = $null
    $env:CONF_FILE = $null
    $buildArgs = @('-m', 'west', 'build', '-b', 'ht_main',
        'firmware', '-d', $buildDir)
    if ($Pristine) {
        $buildArgs += @('-p', 'always')
    }
    $buildArgs += @('--', "-DDTC_OVERLAY_FILE:STRING=$overlay", '-DEXTRA_DTC_OVERLAY_FILE:STRING=',
        "-DCONF_FILE:STRING=$baseConf", "-DEXTRA_CONF_FILE:STRING=$conf")
    Invoke-Checked $python $buildArgs
}
finally {
    $env:PATH = $oldPath
    $env:ZEPHYR_BASE = $oldBase
    $env:ZEPHYR_SDK_INSTALL_DIR = $oldSdk
    $env:DTC_OVERLAY_FILE = $oldOverlay
    $env:EXTRA_DTC_OVERLAY_FILE = $oldExtraOverlay
    $env:EXTRA_CONF_FILE = $oldConf
    $env:CONF_FILE = $oldBaseConf
    Pop-Location
}
