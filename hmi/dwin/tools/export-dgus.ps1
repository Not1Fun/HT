# @brief 从 HT 界面契约创建全新 DGUS 工程，并调用官方编译器导出屏资源。
param(
    [string]$Manifest = (Join-Path $PSScriptRoot '../ui.json'),
    [string]$ToolDir = (Join-Path $PSScriptRoot '../../../.tools/dwin-tools/DGUS_V7651/DGUS_V7651'),
    [string]$OutputDir = (Join-Path $PSScriptRoot '../project'),
    [string]$FontFile = (Join-Path $PSScriptRoot '../project/DWIN_SET/0_DWIN_ASC.HZK'),
    [switch]$SkipIcl
)

$ErrorActionPreference = 'Stop'
$Manifest = [IO.Path]::GetFullPath($Manifest)
$ToolDir = [IO.Path]::GetFullPath($ToolDir)
$OutputDir = [IO.Path]::GetFullPath($OutputDir)
$FontFile = [IO.Path]::GetFullPath($FontFile)
if ([IntPtr]::Size -ne 4 -or $PSVersionTable.PSEdition -eq 'Core' -or
    [Threading.Thread]::CurrentThread.ApartmentState -ne 'STA') {
    $host32 = Join-Path $env:WINDIR 'SysWOW64/WindowsPowerShell/v1.0/powershell.exe'
    $args32 = @('-NoProfile', '-STA', '-ExecutionPolicy', 'Bypass', '-File', $PSCommandPath,
        '-Manifest', $Manifest, '-ToolDir', $ToolDir, '-OutputDir', $OutputDir, '-FontFile', $FontFile)
    if ($SkipIcl) { $args32 += '-SkipIcl' }
    & $host32 @args32
    exit $LASTEXITCODE
}

$ui = Get-Content -LiteralPath $Manifest -Raw -Encoding UTF8 | ConvertFrom-Json
$sourceDir = Split-Path -Parent $Manifest
if ($ui.width -ne 800 -or $ui.height -ne 480 -or $ui.touch) { throw 'Expected an 800 x 480 non-touch project.' }
if ($ui.version -ne 8 -or @($ui.pages).Count -ne 5 -or
    $ui.pages[0].id -ne 0 -or $ui.pages[1].id -ne 1 -or
    $ui.pages[2].id -ne 2 -or $ui.pages[3].id -ne 3 -or
    $ui.pages[4].id -ne 4) { throw 'Expected HT v8 status/settings/logs/debug/DAC pages.' }
if (@($ui.fields).Count -ne 27 -or @($ui.icons).Count -ne 13 -or
    @($ui.animations).Count -ne 0) { throw 'Expected 27 fields, 13 icon controls and no animation.' }
$usedVp = @{}
foreach ($spec in @($ui.fields) + @($ui.icons)) {
    $words = if ($null -ne $spec.words) { [int]$spec.words } else { 1 }
    if ($words -lt 1 -or $spec.vp -lt 0x1000 -or $spec.vp + $words -gt 0xff00) {
        throw "Invalid VP range: $($spec.name)"
    }
    for ($vp = [int]$spec.vp; $vp -lt $spec.vp + $words; $vp++) {
        if ($usedVp.ContainsKey($vp)) { throw "VP overlap: $($spec.name) / $($usedVp[$vp])" }
        $usedVp[$vp] = $spec.name
    }
}
if (-not (Test-Path -LiteralPath $FontFile -PathType Leaf) -or
    (Get-Item -LiteralPath $FontFile).Length -eq 0) { throw "ASCII font missing or empty: $FontFile" }
foreach ($dll in @('DwinTerminal.dll', 'CoreDll.dll', 'DWINUI.dll')) {
    if (-not (Test-Path -LiteralPath (Join-Path $ToolDir $dll))) { throw "Missing official tool: $dll" }
}
foreach ($folder in @($OutputDir, "$OutputDir/DWIN_SET", "$OutputDir/TFT", "$OutputDir/image")) {
    [IO.Directory]::CreateDirectory($folder) | Out-Null
}

$previousDir = [Environment]::CurrentDirectory
Push-Location -LiteralPath $ToolDir
[Environment]::CurrentDirectory = $ToolDir
Add-Type -AssemblyName System.Windows.Forms
Add-Type -AssemblyName System.Drawing
$resolver = [ResolveEventHandler] {
    param($sender, $event)
    $path = Join-Path $ToolDir (([Reflection.AssemblyName]$event.Name).Name + '.dll')
    if (Test-Path -LiteralPath $path) { return [Reflection.Assembly]::LoadFrom($path) }
    return $null
}
[AppDomain]::CurrentDomain.add_AssemblyResolve($resolver)
$assembly = [Reflection.Assembly]::LoadFrom((Join-Path $ToolDir 'DwinTerminal.dll'))
$rectangleType = $assembly.GetType('BizDraw.Objects.DrawRectangle')
$flags = [Reflection.BindingFlags]'Public,NonPublic,Instance'

function Set-Word([byte[]]$Record, [int]$Offset, [int]$Value) {
    if ($Value -lt 0 -or $Value -gt 65535) { throw "Word out of range: $Value" }
    $Record[$Offset] = [byte]($Value -shr 8)
    $Record[$Offset + 1] = [byte]($Value -band 255)
}

function New-Record([int]$Type, [int]$Vp, [int]$Length) {
    $data = New-Object byte[] 32
    Set-Word $data 0 (0x5a00 + $Type)
    Set-Word $data 2 0xffff
    Set-Word $data 4 $Length
    Set-Word $data 6 $Vp
    return ,$data
}

function Add-Control($Document, $Spec, [string]$Kind) {
    if ($Spec.x -lt 0 -or $Spec.y -lt 0 -or $Spec.width -le 0 -or $Spec.height -le 0 -or
        $Spec.x + $Spec.width -gt $ui.width -or $Spec.y + $Spec.height -gt $ui.height) {
        throw "Control outside screen: $($Spec.name)"
    }
    if ($Spec.vp -lt 0x1000 -or $Spec.vp -ge 0xff00) { throw "Invalid user VP: $($Spec.name)" }
    switch ($Kind) {
        'text' {
            if ($Spec.words -ne 16 -or $Spec.encoding -ne 'ascii' -or
                $Spec.display_encoding -ne 'gbk' -or $Spec.font_width -lt 4 -or
                $Spec.font_width -gt 64 -or $Spec.font_height -ne 2 * $Spec.font_width -or
                $Spec.max_chars -lt 1 -or $Spec.max_chars -gt 9 -or
                $Spec.max_chars * $Spec.font_width -gt $Spec.width -or
                $Spec.font_height -gt $Spec.height) {
                throw "Invalid ASCII field: $($Spec.name)"
            }
            $typeId = 0x11
            $config = New-Object BizDraw.ConfigShow.TextShow
            $data = New-Record $typeId $Spec.vp 13
            $color = [Drawing.ColorTranslator]::FromHtml($Spec.color)
            $rgb565 = (([int]$color.R -shr 3) -shl 11) -bor
                (([int]$color.G -shr 2) -shl 5) -bor ([int]$color.B -shr 3)
            Set-Word $data 12 $rgb565
            Set-Word $data 14 $Spec.x
            Set-Word $data 16 $Spec.y
            Set-Word $data 18 ($Spec.x + $Spec.width)
            Set-Word $data 20 ($Spec.y + $Spec.height)
            Set-Word $data 22 ($Spec.words * 2)
            # The built-in ASCII font uses GBK mode; ASCII width is half the configured X dots.
            $data[26] = [byte]($Spec.font_width * 2)
            $data[27] = [byte]$Spec.font_height
            $data[28] = 0x82 # GBK, fixed ASCII width, left/top alignment.
            $initial = '--'
        }
        'icon' {
            $typeId = 0
            $config = New-Object BizDraw.ConfigShow.IconShow
            $data = New-Record $typeId $Spec.vp 10
            Set-Word $data 14 ($Spec.last - $Spec.first)
            Set-Word $data 16 $Spec.first
            Set-Word $data 18 $Spec.last
            $data[20] = [byte]$ui.icon_library
            $data[21] = 1
            if ($Spec.initial -lt 0 -or $Spec.initial -gt $Spec.last - $Spec.first) {
                throw "Icon initial value out of range: $($Spec.name)"
            }
            $initial = [string]$Spec.initial
        }
        'animation' {
            if ($Spec.reserved_vp -ne $Spec.vp + 1 -or $Spec.frame_ms % 20 -or
                $Spec.frame_ms -lt 20 -or $Spec.frame_ms -gt 5100 -or $Spec.last -gt 255) {
                throw "Invalid animation: $($Spec.name)"
            }
            $typeId = 1
            $config = New-Object BizDraw.ConfigShow.AnimateShow
            $data = New-Record $typeId $Spec.vp 13
            Set-Word $data 12 1
            Set-Word $data 14 0
            Set-Word $data 16 1
            Set-Word $data 18 $Spec.stop
            Set-Word $data 20 $Spec.first
            Set-Word $data 22 $Spec.last
            $data[24] = [byte]$ui.icon_library
            $data[25] = 1
            $data[29] = [byte]($Spec.frame_ms / 20)
            $initial = '0'
        }
        default { throw "Unsupported control: $Kind" }
    }
    Set-Word $data 8 $Spec.x
    Set-Word $data 10 $Spec.y
    $config.CreatFromByte($data)
    $config.VarStrPoint = '{0:X4}' -f $Spec.vp
    $config.Var_Name = $Spec.name
    $config.Pic_Id = [uint16]$Document.ImageIndex
    $config.DataConfig = $initial
    $config.DataLen = if ($Kind -eq 'text') { 32 } elseif ($Kind -eq 'animation') { 4 } else { 2 }
    if ($Kind -ne 'text') { $config.ICOFileName = "$($ui.icon_library).icl" }

    # DrawRectangle has case-conflicting members; reflection avoids PowerShell's adapter ambiguity.
    $rectangle = [Activator]::CreateInstance($rectangleType)
    $bounds = New-Object Drawing.Rectangle($Spec.x, $Spec.y, $Spec.width, $Spec.height)
    $rectangleType.GetProperty('Rectangle').SetValue($rectangle, $bounds, $null)
    $rectangleType.GetProperty('ConfigObject').SetValue($rectangle, $config, $null)
    $rectangleType.GetProperty('f13Type').SetValue($rectangle, (100 + $typeId), $null)
    $rectangleType.GetProperty('f14Type').SetValue($rectangle, $typeId, $null)
    $Document.Items.Add($rectangle)
    return ,$data
}

$grid = $null
try {
    [BizDraw.Globel]::Width = $ui.width
    [BizDraw.Globel]::Height = $ui.height
    [BizDraw.Globel]::VarCount = 64
    [BizDraw.Globel]::VersionBh = 0
    [BizDraw.Globel]::ProjectPath = $OutputDir
    [BizDraw.Globel]::PrjFilePath = $OutputDir
    [BizDraw.Globel]::FileName_13 = '\13TouchFile.bin'
    [BizDraw.Globel]::FileName_14 = '\14ShowFile.bin'

    # Only fresh data objects are allocated; no existing TFT is deserialized and no window is shown.
    $space = [Runtime.Serialization.FormatterServices]::GetUninitializedObject([BizDraw.Controls.TDocumentSpace])
    $space.strJiamiCode = ''
    $space.GetType().GetField('iniData_Touch', $flags).SetValue($space,
        (New-Object 'System.Collections.Generic.Dictionary[UInt16,String]'))
    $space.m_taskList = [Runtime.Serialization.FormatterServices]::GetUninitializedObject([BizDraw.Controls.DummyTaskList])
    $grid = New-Object Windows.Forms.DataGridView
    $space.m_taskList.dgvPictureList = $grid
    $grid.AllowUserToAddRows = $false
    foreach ($name in @('id', 'image', 'document')) { [void]$grid.Columns.Add($name, $name) }
    $imageList = New-Object 'System.Collections.Generic.List[String]'
    $documents = @()
    $expectedRecords = @{}
    $ini = @('[INIT]', 'PICFIX=1', 'VARCount=64', 'Version=0', 'ProVersion=651',
        'SCREENDSIZE=800X480', 'SPADDRESS=5000', '[IMG]')
    foreach ($page in $ui.pages) {
        $imageName = '{0:D3}.png' -f [int]$page.id
        # The official editor rebuilds this path from [IMG]; it does not use the saved backImagePath.
        $imagePath = Join-Path $OutputDir "DWIN_SET/$imageName"
        Copy-Item -LiteralPath (Join-Path $sourceDir $page.image) -Destination $imagePath -Force
        Copy-Item -LiteralPath $imagePath -Destination (Join-Path $OutputDir "image/$imageName") -Force
        $editorImagePath = $OutputDir + [BizDraw.Globel]::ProjectPathName + $imageName
        if (-not (Test-Path -LiteralPath $editorImagePath)) { throw "Editor background missing: $imageName" }
        $background = [Drawing.Bitmap]::FromFile($editorImagePath)
        try {
            if ($background.Width -ne $ui.width -or $background.Height -ne $ui.height) {
                throw "Background size mismatch: $imageName"
            }
        } finally { $background.Dispose() }
        $imageList.Add($imageName)
        $ini += "$($page.id)=$imageName"
        $doc = New-Object BizDraw.Core.Document
        $doc.Width = $ui.width
        $doc.Height = $ui.height
        $doc.Name = $page.name
        $doc.FilePath = $OutputDir
        $doc.filename = $imageName
        $doc.backImagePath = $imagePath
        $doc.ImageIndex = [uint16]$page.id
        $doc.Picpix = 1
        $doc.SPAddress = '5000'
        $doc.VarAddress = '1000'
        $doc.AutoVP = $false
        $records = New-Object 'System.Collections.Generic.List[byte[]]'
        foreach ($spec in $ui.icons) { if ($page.id -in $spec.pages) { $records.Add((Add-Control $doc $spec 'icon')) } }
        foreach ($spec in $ui.animations) { if ($page.id -in $spec.pages) { $records.Add((Add-Control $doc $spec 'animation')) } }
        foreach ($spec in $ui.fields) { if ($page.id -in $spec.pages) { $records.Add((Add-Control $doc $spec 'text')) } }
        $expectedRecords[[int]$page.id] = $records
        $row = $grid.Rows.Add()
        $grid.Rows[$row].Cells[0].Value = [string]$page.id
        $grid.Rows[$row].Cells[1].Value = $imageName
        $grid.Rows[$row].Cells[2].Value = $doc
        $documents += $doc
    }
    foreach ($doc in $documents) {
        $doc.PicList = $imageList
        [BizDraw.IO.DocumentManager]::Save($doc, "$OutputDir/TFT/$($doc.filename).tft")
    }
    [BizDraw.IO.DocumentManager]::Save($documents[0], "$OutputDir/DWprj.tft")
    [IO.File]::WriteAllLines("$OutputDir/DWprj.hmi", $ini, [Text.Encoding]::ASCII)
    if (-not $space.BuildInputBinFile("$OutputDir/DWprj.hmi", 0)) { throw 'Official touch export failed.' }
    # Official mode 2 emits compact DGUS_2 without file encryption, matching the supplied factory format.
    $result = $space.BuildShowBinFile("$OutputDir/DWprj.hmi", 2)
    if ($result -ne 0) { throw "Official display export failed: $result" }
    $display = [IO.File]::ReadAllBytes("$OutputDir/DWIN_SET/14ShowFile.bin")
    if ([Text.Encoding]::ASCII.GetString($display, 1, 6) -ne 'DGUS_2') { throw 'Unexpected official display format.' }
    foreach ($page in $ui.pages) {
        $entry = 16 + 4 * $page.id
        $count = [int]$display[$entry]
        $offset = ([int]$display[$entry + 1] -shl 16) -bor
            ([int]$display[$entry + 2] -shl 8) -bor [int]$display[$entry + 3]
        $records = $expectedRecords[[int]$page.id]
        if ($count -ne $records.Count -or $offset -lt 16384 -or $offset + 32 * $count -gt $display.Length) {
            throw "Official page index mismatch: $($page.id)"
        }
        for ($index = 0; $index -lt $count; $index++) {
            for ($byte = 0; $byte -lt 32; $byte++) {
                if ($display[$offset + 32 * $index + $byte] -ne $records[$index][$byte]) {
                    throw "Official record mismatch: page $($page.id), control $index, byte $byte"
                }
            }
        }
    }
    $initialData = [IO.File]::ReadAllBytes("$OutputDir/DWIN_SET/22_Config.bin")
    if ($initialData.Length -ne 131076) { throw 'Unexpected official initialization format.' }
    foreach ($icon in $ui.icons) {
        $offset = 2 * $icon.vp
        $value = ([int]$initialData[$offset] -shl 8) -bor [int]$initialData[$offset + 1]
        if ($value -ne $icon.initial) { throw "Icon initial value mismatch: $($icon.name)" }
    }
    foreach ($field in $ui.fields) {
        for ($index = 0; $index -lt 32; $index++) {
            $expected = if ($index -lt 2) { 45 } else { 0 }
            if ($initialData[2 * $field.vp + $index] -ne $expected) {
                throw "Text initial value mismatch: $($field.name)"
            }
        }
    }
    $fontTarget = [IO.Path]::GetFullPath("$OutputDir/DWIN_SET/0_DWIN_ASC.HZK")
    if (-not [string]::Equals($FontFile, $fontTarget, [StringComparison]::OrdinalIgnoreCase)) {
        Copy-Item -LiteralPath $FontFile -Destination $fontTarget -Force
    }
    Write-Output "DGUS project: $OutputDir/DWprj.hmi"
    Write-Output "Official display records exported for $($documents.Count) pages."
} finally {
    if ($null -ne $grid) { $grid.Dispose() }
    [AppDomain]::CurrentDomain.remove_AssemblyResolve($resolver)
    [Environment]::CurrentDirectory = $previousDir
    Pop-Location
}

if (-not $SkipIcl) {
    $iclScript = Join-Path $PSScriptRoot 'build-icl.ps1'
    if (-not (Test-Path -LiteralPath $iclScript)) { throw 'ICL exporter is required; use -SkipIcl for controls only.' }
    & $iclScript -ToolDir $ToolDir -InputDir (Join-Path $sourceDir 'assets/pages') -OutputFile "$OutputDir/DWIN_SET/$($ui.background_library).icl" -ExpectedCount @($ui.pages).Count
    $iconCount = [int](($ui.icons | Measure-Object -Property last -Maximum).Maximum) + 1
    & $iclScript -ToolDir $ToolDir -InputDir (Join-Path $sourceDir 'assets/icons') -OutputFile "$OutputDir/DWIN_SET/$($ui.icon_library).icl" -ExpectedCount $iconCount
}
