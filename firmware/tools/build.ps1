param(
    [switch]$Setup,
    [switch]$Pristine,
    [string]$SdkPath = $env:ZEPHYR_SDK_INSTALL_DIR
)

$ErrorActionPreference = 'Stop'
$root = Split-Path (Split-Path $PSScriptRoot -Parent) -Parent
$python = Join-Path $root '.tools\venv\Scripts\python.exe'
$oldPath = $env:PATH
$oldBase = $env:ZEPHYR_BASE
$oldSdk = $env:ZEPHYR_SDK_INSTALL_DIR

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
    $buildArgs = @('-m', 'west', 'build', '-b', 'ht_main',
        'firmware', '-d', 'firmware/build')
    if ($Pristine) {
        $buildArgs += @('-p', 'always')
    }
    Invoke-Checked $python $buildArgs
}
finally {
    $env:PATH = $oldPath
    $env:ZEPHYR_BASE = $oldBase
    $env:ZEPHYR_SDK_INSTALL_DIR = $oldSdk
    Pop-Location
}
