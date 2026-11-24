#requires -Version 7.0
using namespace System.Drawing
using namespace System.Drawing.Drawing2D
using namespace System.Drawing.Imaging
using namespace System.Globalization

param(
    [Parameter(Mandatory)][string] $EditorFrames,
    [Parameter(Mandatory)][string] $RegionFrames,
    [Parameter(Mandatory)][string] $OutputPath
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Drawing.Common
$ffmpeg = (Get-Command ffmpeg -CommandType Application).Source
$width = 1080
$height = 714
$frameRate = 18
$closingFrames = 23

function Read-Frames([string] $Directory) {
    $Directory = (Resolve-Path -LiteralPath $Directory).Path
    $lines = @(Get-Content -LiteralPath (Join-Path $Directory 'frames.txt'))
    if (!$lines.Count -or $lines[0] -cne '# freezecut-frame-coordinates: png-pixels') {
        throw "Unknown frame coordinate space in $Directory. Record new frames with freezecut_shots."
    }

    $previous = -1
    for ($i = 1; $i -lt $lines.Count; $i++) {
        $line = $lines[$i]
        $record = [regex]::Match($line, '^(\d+)\s+(\d+(?:\.\d+)?)\s+(\d+(?:\.\d+)?)\s+(cross|arrow)$')
        if (!$record.Success) {
            throw "Invalid frame record in $Directory : $line"
        }

        $index = [int] $record.Groups[1].Value
        if ($index -le $previous) {
            throw "Frame $index in $Directory is out of order."
        }
        $previous = $index
        $image = Join-Path $Directory ('{0:D4}.png' -f $index)
        if (!(Test-Path -LiteralPath $image -PathType Leaf)) {
            throw "Missing capture frame $image"
        }

        [pscustomobject]@{
            Index = $index
            X = [double]::Parse($record.Groups[2].Value, [CultureInfo]::InvariantCulture)
            Y = [double]::Parse($record.Groups[3].Value, [CultureInfo]::InvariantCulture)
            Shape = $record.Groups[4].Value
            Image = $image
            Directory = $Directory
        }
    }
}

function Assert-Sequence([object[]] $Frames, [int] $First, [int] $Last, [string] $Name) {
    if (!$Frames.Count) {
        throw "No $Name frames found."
    }
    for ($i = 0; $i -lt $Frames.Count; $i++) {
        $expected = $First + $i
        if ($expected -gt $Last -or $Frames[$i].Index -ne $expected) {
            throw "Unexpected frame $($Frames[$i].Index) in $Name."
        }
    }
}

$editor = @(Read-Frames $EditorFrames)
$intro = @($editor | Where-Object Index -lt 100)
$annotations = @($editor | Where-Object Index -ge 1000)
if ($editor.Count -ne $intro.Count + $annotations.Count) {
    throw 'The editor log contains region frame numbers.'
}
$region = @(Read-Frames $RegionFrames)
Assert-Sequence $intro 0 99 'editor intro'
Assert-Sequence $region 100 999 'region'
Assert-Sequence $annotations 1000 ([int]::MaxValue) 'editor annotation'
$frames = $intro + $region + $annotations

$output = [IO.Path]::GetFullPath($OutputPath, $PWD.Path)
if ([IO.Path]::GetExtension($output) -ine '.gif') {
    throw 'OutputPath must have a .gif extension.'
}
$scratch = Join-Path ([IO.Path]::GetTempPath()) ('freezecut-demo-' + [Guid]::NewGuid().ToString('N'))
[void] [IO.Directory]::CreateDirectory($scratch)
$arrow = [GraphicsPath]::new()
$arrow.AddPolygon([Point[]]@([Point]::new(0, 0), [Point]::new(1, 19), [Point]::new(6, 14),
    [Point]::new(10, 21), [Point]::new(13, 20), [Point]::new(9, 13), [Point]::new(16, 12)))
$arrowBorder = [Pen]::new([Color]::FromArgb(248, 248, 250), 1.35)
$arrowBorder.LineJoin = [LineJoin]::Round
$arrowFill = [SolidBrush]::new([Color]::FromArgb(24, 25, 33))
$crossBorder = [Pen]::new([Color]::FromArgb(246, 246, 249), 3)
$crossCore = [Pen]::new([Color]::FromArgb(24, 25, 33), 1)
$sizes = @{}

try {
    for ($i = 0; $i -lt $frames.Count; $i++) {
        $frame = $frames[$i]
        $source = [Image]::FromFile($frame.Image)
        $resized = $null
        $graphics = $null
        try {
            if ($sizes.ContainsKey($frame.Directory) -and $source.Size -ne $sizes[$frame.Directory]) {
                throw "Frame $($frame.Image) changed capture size."
            }
            $sizes[$frame.Directory] = $source.Size
            if ($frame.X -ge $source.Width -or $frame.Y -ge $source.Height) {
                throw "Pointer is outside frame $($frame.Image)."
            }

            $resized = [Bitmap]::new($width, $height, [PixelFormat]::Format24bppRgb)
            $graphics = [Graphics]::FromImage($resized)
            $graphics.InterpolationMode = [InterpolationMode]::HighQualityBicubic
            $graphics.PixelOffsetMode = [PixelOffsetMode]::HighQuality
            $graphics.SmoothingMode = [SmoothingMode]::AntiAlias
            $graphics.DrawImage($source, 0, 0, $width, $height)

            # The source frame and its pointer hotspot use the same pixel space.
            $x = [single] ($frame.X * $width / $source.Width)
            $y = [single] ($frame.Y * $height / $source.Height)
            $graphics.TranslateTransform($x, $y)
            if ($frame.Shape -eq 'cross') {
                foreach ($pen in @($crossBorder, $crossCore)) {
                    $graphics.DrawLine($pen, -8, 0, 8, 0)
                    $graphics.DrawLine($pen, 0, -8, 0, 8)
                }
            } else {
                $graphics.FillPath($arrowFill, $arrow)
                $graphics.DrawPath($arrowBorder, $arrow)
            }

            $graphics.Dispose()
            $graphics = $null
            $resized.Save((Join-Path $scratch ('{0:D4}.png' -f $i)), [ImageFormat]::Png)
        } finally {
            if ($graphics) { $graphics.Dispose() }
            if ($resized) { $resized.Dispose() }
            $source.Dispose()
        }
    }

    $lastFrame = Join-Path $scratch ('{0:D4}.png' -f ($frames.Count - 1))
    for ($i = $frames.Count; $i -lt $frames.Count + $closingFrames; $i++) {
        Copy-Item -LiteralPath $lastFrame -Destination (Join-Path $scratch ('{0:D4}.png' -f $i))
    }

    $gif = Join-Path $scratch 'demo.gif'
    $filters = '[0:v]split[video][palette_input];[palette_input]palettegen=stats_mode=diff[palette];[video][palette]paletteuse=dither=bayer:bayer_scale=4:diff_mode=rectangle'
    & $ffmpeg -hide_banner -loglevel error -framerate $frameRate -start_number 0 -i (Join-Path $scratch '%04d.png') -filter_complex $filters -loop 0 $gif
    if ($LASTEXITCODE -ne 0) {
        throw "FFmpeg could not encode the demo (exit $LASTEXITCODE)."
    }
    [void] [IO.Directory]::CreateDirectory([IO.Path]::GetDirectoryName($output))
    Copy-Item -LiteralPath $gif -Destination $output -Force
    Write-Output "Wrote $($frames.Count) captured frames and $closingFrames closing frames to $output"
} finally {
    foreach ($resource in @($arrow, $arrowBorder, $arrowFill, $crossBorder, $crossCore)) {
        $resource.Dispose()
    }
    foreach ($asset in Get-ChildItem -File -LiteralPath $scratch) {
        Remove-Item -LiteralPath $asset.FullName
    }
    Remove-Item -LiteralPath $scratch
}
