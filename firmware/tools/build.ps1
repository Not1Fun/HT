param(
    [switch]$Setup,
    [switch]$Pristine,
    [switch]$InternalClock,
    [ValidateSet('Off', 'Bridge', 'Panel')]
    [string]$ScreenMode = 'Off',
    [switch]$Temperature,
    [switch]$Dds,
    [switch]$Output,
    [switch]$Bootloader,
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
    if ($Output -and ($ScreenMode -ne 'Panel' -or $Temperature -or $Dds)) {
        throw '-Output requires -ScreenMode Panel; it owns ADC temperature sampling and cannot combine with -Temperature or -Dds.'
    }
    if ($Temperature -and $ScreenMode -ne 'Panel') {
        throw '-Temperature requires -ScreenMode Panel and powered AVDD.'
    }
    if ($Dds -and (-not $Temperature -or $ScreenMode -ne 'Panel')) {
        throw '-Dds requires -ScreenMode Panel -Temperature and an unpowered/disconnected amplifier.'
    }
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
        if ($Bootloader) {
            Invoke-Checked $python @('-m', 'pip', 'install', '-r',
                '.tools/bootloader/mcuboot/scripts/requirements.txt')
        }
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
    if ($Temperature) {
        $buildDir += '-temperature'
        $conf += ';screen-temperature.conf'
    }
    if ($Dds) {
        $buildDir += '-dds'
        $conf += ';dds-bench.conf'
        $overlays += 'boards/ht_main_dds.overlay'
    }
    if ($Output) {
        $buildDir += '-output'
        $conf += ';output.conf'
        $overlays += 'boards/ht_main_output.overlay'
    }
    if ($Bootloader) {
        if (-not (Test-Path -LiteralPath '.tools/bootloader/mcuboot/boot/zephyr/CMakeLists.txt') -or
            -not (Test-Path -LiteralPath '.tools/modules/lib/zcbor/zephyr/module.yml')) {
            throw 'MCUboot dependencies missing. Run with -Setup -Bootloader first.'
        }
        $buildDir += '-boot'
        $overlays += 'boards/ht_main_boot.overlay'
    }
    $overlay = $overlays -join ';'
    $env:DTC_OVERLAY_FILE = $null
    $env:EXTRA_DTC_OVERLAY_FILE = $null
    $env:EXTRA_CONF_FILE = $null
    $env:CONF_FILE = $null
    $buildArgs = @('-m', 'west', 'build', '-b', 'ht_main',
        'firmware', '-d', $buildDir)
    if ($Bootloader) { $buildArgs += '--sysbuild' }
    if ($Pristine) {
        $buildArgs += @('-p', 'always')
    }
    $buildArgs += @('--', "-DDTC_OVERLAY_FILE:STRING=$overlay", '-DEXTRA_DTC_OVERLAY_FILE:STRING=',
        "-DCONF_FILE:STRING=$baseConf", "-DEXTRA_CONF_FILE:STRING=$conf")
    if ($Bootloader) {
        $firmwareDir = (Join-Path $root 'firmware').Replace('\', '/')
        $bootOverlays = @("$firmwareDir/sysbuild/mcuboot.overlay")
        if ($InternalClock) { $bootOverlays += "$firmwareDir/boards/ht_main_hsi.overlay" }
        $buildArgs += @("-DBOARD_ROOT:PATH=$firmwareDir", "-DDTS_ROOT:PATH=$firmwareDir",
            "-Dmcuboot_CONF_FILE:FILEPATH=$firmwareDir/sysbuild/mcuboot.conf",
            '-Dmcuboot_EXTRA_CONF_FILE:STRING=',
            "-Dmcuboot_DTC_OVERLAY_FILE:STRING=$($bootOverlays -join ';')",
            "-Dmcuboot_EXTRA_ZEPHYR_MODULES:STRING=$firmwareDir/bootloader")
    }
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
