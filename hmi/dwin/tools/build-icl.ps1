param(
    [Parameter(Mandatory = $true)][string]$ToolDir,
    [Parameter(Mandatory = $true)][string]$InputDir,
    [Parameter(Mandatory = $true)][string]$OutputFile
)

$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'
Set-StrictMode -Version Latest

$ToolDir = [IO.Path]::GetFullPath($ToolDir)
$InputDir = [IO.Path]::GetFullPath($InputDir)
$OutputFile = [IO.Path]::GetFullPath($OutputFile)
if ([IntPtr]::Size -ne 4 -or [Threading.Thread]::CurrentThread.ApartmentState -ne 'STA') {
    $windowsPowerShell = Join-Path $env:WINDIR 'SysWOW64\WindowsPowerShell\v1.0\powershell.exe'
    if (-not (Test-Path -LiteralPath $windowsPowerShell)) {
        throw 'This build requires 32-bit Windows PowerShell in STA mode.'
    }
    & $windowsPowerShell -NoProfile -STA -ExecutionPolicy Bypass -File $PSCommandPath `
        -ToolDir $ToolDir -InputDir $InputDir -OutputFile $OutputFile
    if ($LASTEXITCODE -ne 0) { throw "ICL build failed (exit $LASTEXITCODE)." }
    return
}

# DGUS V7.651: use its actual ICL packer; DW_ICON.exe builds the older ICO format.
$dllPath = Join-Path $ToolDir 'DwinTerminal.dll'
$expectedHash = '0EF28FC0B2427B78173D7A1A0C0D2CE46A47FFE3299D5D7E3F1E9E8728B566F7'
if ((Get-FileHash -LiteralPath $dllPath -Algorithm SHA256).Hash -ne $expectedHash) {
    throw 'DwinTerminal.dll differs from the inspected DGUS V7.651 release.'
}
$libraryName = [IO.Path]::GetFileName($OutputFile)
if ($libraryName -notmatch '^(32|42)\.icl$') { throw 'Expected output 32.icl or 42.icl.' }
$expectedCount = if ($libraryName -eq '32.icl') { 3 } else { 62 }
$files = @(Get-ChildItem -LiteralPath $InputDir -File -Filter '*.png' | Sort-Object Name)
if ($files.Count -ne $expectedCount) { throw "Expected $expectedCount PNG files in $InputDir." }
for ($index = 0; $index -lt $files.Count; $index++) {
    if ($files[$index].Name -cne ('{0:D3}.png' -f $index)) {
        throw 'PNG names must be contiguous: 000.png, 001.png, ...'
    }
}

Add-Type -AssemblyName System.Drawing
$flags = [Reflection.BindingFlags]'Public,NonPublic,Instance,Static'
$resolver = [ResolveEventHandler] {
    param($sender, $eventArgs)
    $dependency = Join-Path $ToolDir (([Reflection.AssemblyName]$eventArgs.Name).Name + '.dll')
    if (Test-Path -LiteralPath $dependency) { return [Reflection.Assembly]::LoadFrom($dependency) }
    return $null
}
[AppDomain]::CurrentDomain.add_AssemblyResolve($resolver)
$assembly = [Reflection.Assembly]::LoadFrom($dllPath)
$iclType = $assembly.GetType('BizDraw.Utils.ICL', $true)
$crcType = $assembly.GetType('BizDraw.Utils.CRC16', $true)

function Read-BigEndian([byte[]]$Data, [int]$Offset, [int]$Count) {
    [long]$value = 0
    for ($i = 0; $i -lt $Count; $i++) { $value = ($value -shl 8) -bor [int]$Data[$Offset + $i] }
    return $value
}

function Test-Icl([string]$Path, $Images, [int]$StartIndex, $Packer) {
    $data = [IO.File]::ReadAllBytes($Path)
    if ($data.Length -le $StartIndex -or [Text.Encoding]::ASCII.GetString($data, 0, 6) -cne 'DGUS_3') {
        throw 'Invalid ICL header.'
    }
    if ($data[12] -ne 4 -or (Read-BigEndian $data 13 2) -ne ($Images.Count - 1)) {
        throw 'ICL format or maximum resource ID mismatch.'
    }
    # The vendor writes this field using 4096 even though its index area is 32768.
    # Preserve the vendor output; do not patch or independently encode ICL bytes.
    if ((Read-BigEndian $data 8 4) -ne ($data.Length - $StartIndex + 4096 - 8)) {
        throw 'ICL length field does not match the vendor algorithm.'
    }
    $crc = [Activator]::CreateInstance($crcType, $true)
    $crcMethod = $crcType.GetMethod('CalculateCrc16', $flags, $null, [type[]]@([byte[]], [int]), $null)
    $calculated = $crcMethod.Invoke($crc, [object[]]@($data, [int]8))
    if ($calculated[0] -ne $data[6] -or $calculated[1] -ne $data[7]) { throw 'ICL CRC mismatch.' }

    [long]$expectedOffset = $StartIndex
    for ($id = 0; $id -lt $Images.Count; $id++) {
        [long]$offset = Read-BigEndian $data (16 + 4 * $id) 4
        if ($offset -ne $expectedOffset -or $offset + 12 -gt $data.Length) {
            throw "Invalid ICL index for resource $id."
        }
        $width = Read-BigEndian $data $offset 2
        $height = Read-BigEndian $data ($offset + 2) 2
        if ($width -ne $Images[$id].Width -or $height -ne $Images[$id].Height) {
            throw "ICL resource $id dimensions differ from its PNG."
        }
        $jpegHeader = Read-BigEndian $data ($offset + 4) 2
        $jpegBody = Read-BigEndian $data ($offset + 6) 4
        [long]$recordLength = 10 + $jpegHeader + $jpegBody
        $expectedOffset = $offset + $recordLength
        if ($recordLength -le 12 -or $expectedOffset -gt $data.Length -or
            $data[$offset + 10] -ne 255 -or $data[$offset + 11] -ne 216) {
            throw "Invalid JPEG resource $id."
        }
        [byte[]]$jpeg = New-Object byte[] ([int]($recordLength - 10))
        [Buffer]::BlockCopy($data, [int]($offset + 10), $jpeg, 0, $jpeg.Length)
        $lastMarker = [Array]::LastIndexOf($jpeg, [byte]255)
        if ($lastMarker -lt 0 -or $lastMarker + 1 -ge $jpeg.Length -or $jpeg[$lastMarker + 1] -ne 217) {
            throw "ICL resource $id lacks its JPEG end marker."
        }
        # ICL aligns the JPEG scan header; the vendor's reader removes that padding.
        $editArguments = New-Object object[] 1
        $editArguments[0] = $jpeg
        $decodedJpeg = $iclType.GetMethod('editImage', $flags).Invoke($Packer, $editArguments)
        $stream = [IO.MemoryStream]::new([byte[]]$decodedJpeg, $false)
        $decoded = $null
        try {
            $decoded = [Drawing.Image]::FromStream($stream, $false, $true)
            if ($decoded.Width -ne $width -or $decoded.Height -ne $height) {
                throw "Decoded JPEG resource $id dimensions mismatch."
            }
        } finally {
            if ($null -ne $decoded) { $decoded.Dispose() }
            $stream.Dispose()
        }
    }
    if ($expectedOffset -ne $data.Length) { throw 'ICL has unexpected trailing resource data.' }
    for ($i = 16 + 4 * $Images.Count; $i -lt $StartIndex; $i++) {
        if ($data[$i] -ne 0) { throw 'ICL contains an unexpected additional resource index.' }
    }
}

$tempRoot = [IO.Path]::GetFullPath([IO.Path]::GetTempPath())
$workDir = Join-Path $tempRoot ('HT-DWIN-ICL-' + [Guid]::NewGuid().ToString('N'))
$images = @()
$packer = $null
try {
    [void][IO.Directory]::CreateDirectory($workDir)
    $bitmapDir = Join-Path $workDir 'images'
    [void][IO.Directory]::CreateDirectory($bitmapDir)
    foreach ($file in $files) {
        $source = [Drawing.Image]::FromFile($file.FullName)
        try {
            if ($libraryName -eq '32.icl' -and ($source.Width -ne 800 -or $source.Height -ne 480)) {
                throw "Page $($file.Name) must be 800 x 480."
            }
            $images += [PSCustomObject]@{ Width = $source.Width; Height = $source.Height }
            # Its JPEG quality search uses the source file size: BMP avoids overcompressing small PNGs.
            $source.Save((Join-Path $bitmapDir ($file.BaseName + '.bmp')), [Drawing.Imaging.ImageFormat]::Bmp)
        } finally { $source.Dispose() }
    }
    $builtFile = Join-Path $workDir $libraryName
    $packer = [Activator]::CreateInstance($iclType, $true)
    $iclType.GetField('savePath', $flags).SetValue($packer, $builtFile)
    $iclType.GetField('imgPath', $flags).SetValue($packer, $bitmapDir)
    [void]$iclType.GetMethod('run', $flags).Invoke($packer, [object[]]@($null))
    $startIndex = $iclType.GetField('startIndex', $flags).GetValue($packer)
    Test-Icl $builtFile $images $startIndex $packer
    [void][IO.Directory]::CreateDirectory([IO.Path]::GetDirectoryName($OutputFile))
    [IO.File]::Copy($builtFile, $OutputFile, $true)
    $dimensions = ($images | Group-Object Width, Height | ForEach-Object { "$($_.Count) x $($_.Name)" }) -join '; '
    Write-Output "$libraryName verified: $($images.Count) resources; dimensions [$dimensions]; $((Get-Item -LiteralPath $OutputFile).Length) bytes; CRC OK."
} finally {
    if ($null -ne $packer) {
        $items = $iclType.GetField('iconDic', $flags).GetValue($packer)
        foreach ($item in $items.Values) {
            if ($null -ne $item.Image) { $item.Image.Dispose() }
        }
    }
    [AppDomain]::CurrentDomain.remove_AssemblyResolve($resolver)
    # Only remove the unique temporary directory created by this invocation.
    $resolvedWork = [IO.Path]::GetFullPath($workDir)
    $tempPrefix = $tempRoot.TrimEnd('\') + '\'
    if (-not $resolvedWork.StartsWith($tempPrefix, [StringComparison]::OrdinalIgnoreCase) -or
        [IO.Path]::GetFileName($resolvedWork) -notmatch '^HT-DWIN-ICL-[0-9a-f]{32}$') {
        throw 'Refusing cleanup outside this build temporary directory.'
    }
    if (Test-Path -LiteralPath $resolvedWork) { Remove-Item -LiteralPath $resolvedWork -Recurse -Force }
}
