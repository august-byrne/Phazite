#Requires -Version 5.1
<#
.SYNOPSIS
    Draws the picture GitHub shows for links to the repository (docs/images/social-preview.png) from the
    picture of the window in the README (docs/images/phazite.png).

.DESCRIPTION
    A 1280 x 640 card in the dark theme's colours (src/ui/theme/DesignTokens.h) and Inter: the parts'
    colours, the name, what Phazite is, and the window bleeding off the right edge. Run it after a new
    docs/images/phazite.png (tests/ReadmePicture.cpp makes it), from the repository root:

        powershell -NoProfile -ExecutionPolicy Bypass -File tools\social-preview.ps1

    then upload the picture under the repository's Settings > General > Social preview (GitHub has no
    API for it).
#>
param(
    [string] $Repo = (Split-Path -Parent $PSScriptRoot),
    [string] $Out = (Join-Path (Split-Path -Parent $PSScriptRoot) 'docs\images\social-preview.png')
)
Add-Type -AssemblyName PresentationCore, PresentationFramework, WindowsBase
$ErrorActionPreference = 'Stop'
$W = 1280; $H = 640

function Brush([string] $hex)
{
    $b = New-Object System.Windows.Media.SolidColorBrush ([System.Windows.Media.ColorConverter]::ConvertFromString($hex))
    $b.Freeze()
    $b
}

$fontDir = New-Object System.Uri ((Join-Path $Repo 'resources\fonts\Inter') + '\')
$family = New-Object System.Windows.Media.FontFamily ($fontDir, './#Inter')
function Face($weight)
{
    $t = New-Object System.Windows.Media.Typeface ($family, [System.Windows.FontStyles]::Normal, $weight, [System.Windows.FontStretches]::Normal)
    $g = $null
    if (-not $t.TryGetGlyphTypeface([ref] $g)) { throw "Inter $weight wasn't found in resources\fonts\Inter." }
    $t
}
$semiBold = Face ([System.Windows.FontWeights]::SemiBold)
$regular = Face ([System.Windows.FontWeights]::Normal)
$medium = Face ([System.Windows.FontWeights]::Medium)

$culture = [System.Globalization.CultureInfo]::GetCultureInfo('en-us')
function Text([string] $s, $face, [double] $size, [string] $colour, [double] $maxWidth = 0, [double] $lineHeight = 0)
{
    $ft = New-Object System.Windows.Media.FormattedText ($s, $culture, [System.Windows.FlowDirection]::LeftToRight, $face, $size, (Brush $colour), 1.0)
    if ($maxWidth -gt 0) { $ft.MaxTextWidth = $maxWidth }
    if ($lineHeight -gt 0) { $ft.LineHeight = $lineHeight }
    $ft
}

# The dark palette's background, border, bright text, text and focus colour (VS Code's 2026 Dark), and the parts' colours.
$background = '#121314'; $border = '#2a2b2c'; $textColour = '#ededed'; $dimText = '#bfbfbf'; $accent = '#3994bc'
$parts = '#e5534b', '#f0883e', '#d4a72c', '#57ab5a', '#39c5cf', '#539bf5', '#986ee2', '#e275ad'

$shot = New-Object System.Windows.Media.Imaging.BitmapImage
$shot.BeginInit()
$shot.UriSource = New-Object System.Uri (Join-Path $Repo 'docs\images\phazite.png')
$shot.CacheOption = [System.Windows.Media.Imaging.BitmapCacheOption]::OnLoad
$shot.EndInit()

$visual = New-Object System.Windows.Media.DrawingVisual
[System.Windows.Media.RenderOptions]::SetBitmapScalingMode($visual, [System.Windows.Media.BitmapScalingMode]::HighQuality)
$dc = $visual.RenderOpen()
$dc.DrawRectangle((Brush $background), $null, (New-Object System.Windows.Rect 0, 0, $W, $H))

# The window, 1008 pixels wide, bleeding off the right and bottom edges.
$left = 64; $top = 150
$scale = 1008 / $shot.PixelWidth
$shotRect = New-Object System.Windows.Rect 548, $top, ($shot.PixelWidth * $scale), ($shot.PixelHeight * $scale)
$dc.PushClip((New-Object System.Windows.Media.RectangleGeometry ($shotRect, 14, 14)))
$dc.DrawImage($shot, $shotRect)
$dc.Pop()
$dc.DrawRoundedRectangle($null, (New-Object System.Windows.Media.Pen ((Brush $border), 2)), $shotRect, 14, 14)

$x = $left
foreach ($c in $parts)
{
    $dc.DrawRoundedRectangle((Brush $c), $null, (New-Object System.Windows.Rect $x, $top, 44, 8), 4, 4)
    $x += 52
}
$title = Text 'Phazite' $semiBold 104 $textColour
$dc.DrawText($title, (New-Object System.Windows.Point ($left - 6), ($top + 24)))
$tagline = Text 'A free, open-source DAW for making beats and finishing songs.' $regular 32 $dimText 430 44
$taglineTop = $top + 24 + $title.Height + 4
$dc.DrawText($tagline, (New-Object System.Windows.Point $left, $taglineTop))
$dot = [char] 0x00B7; $registered = [char] 0x00AE # Windows PowerShell 5.1 reads a script without a BOM as ANSI
$facts = Text "Windows  $dot  VST$($registered)3  $dot  AGPL-3.0" $medium 24 $accent
$dc.DrawText($facts, (New-Object System.Windows.Point $left, ($taglineTop + $tagline.Height + 36)))
$dc.Close()

$bitmap = New-Object System.Windows.Media.Imaging.RenderTargetBitmap ($W, $H, 96, 96, [System.Windows.Media.PixelFormats]::Pbgra32)
$bitmap.Render($visual)
$encoder = New-Object System.Windows.Media.Imaging.PngBitmapEncoder
$encoder.Frames.Add([System.Windows.Media.Imaging.BitmapFrame]::Create($bitmap))
$stream = [System.IO.File]::Create($Out)
try { $encoder.Save($stream) } finally { $stream.Dispose() }
Write-Host "Wrote $Out ($((Get-Item $Out).Length) bytes; GitHub takes up to 1 MB)."
