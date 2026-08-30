# Renders one framed still and converts it to a PNG.
#
# The renderer writes a BMP because that needs no encoder; System.Drawing turns
# it into something anything else can open.
param(
    [Parameter(Mandatory = $true)][string]$Out,
    [string]$Zone = "C:\Program Files (x86)\PlayOnline\SquareEnix\FINAL FANTASY XI\ROM\1\0.DAT",
    [string]$Character = "",
    [string]$Look = "",
    [string]$CharacterAt = "",
    [string]$Facing = "",
    [string]$Camera = "",
    [string]$CameraLook = "",
    [string]$Animation = "",
    [string]$Frame = "",
    [string]$Sequence = "",
    [string]$Time = "1200"
)

$scratch = Split-Path -Parent $Out
$bmp = [System.IO.Path]::ChangeExtension($Out, ".bmp")

$env:PORTJEUNO_SCREENSHOT = $bmp
$env:PORTJEUNO_TIME = $Time
if ($Character) { $env:PORTJEUNO_CHARACTER = $Character } else { $env:PORTJEUNO_CHARACTER = $null }
if ($Look) { $env:PORTJEUNO_LOOK = $Look } else { $env:PORTJEUNO_LOOK = $null }
if ($CharacterAt) { $env:PORTJEUNO_CHARACTER_AT = $CharacterAt } else { $env:PORTJEUNO_CHARACTER_AT = $null }
if ($Facing) { $env:PORTJEUNO_CHARACTER_FACING = $Facing } else { $env:PORTJEUNO_CHARACTER_FACING = $null }
if ($Animation) { $env:PORTJEUNO_ANIMATION = $Animation } else { $env:PORTJEUNO_ANIMATION = $null }
if ($Sequence) { $env:PORTJEUNO_SCREENSHOT_SEQUENCE = $Sequence } else { $env:PORTJEUNO_SCREENSHOT_SEQUENCE = $null }
if ($Frame) { $env:PORTJEUNO_FRAME = $Frame } else { $env:PORTJEUNO_FRAME = $null }
if ($Camera) { $env:PORTJEUNO_CAMERA = $Camera } else { $env:PORTJEUNO_CAMERA = $null }
if ($CameraLook) { $env:PORTJEUNO_CAMERA_LOOK = $CameraLook } else { $env:PORTJEUNO_CAMERA_LOOK = $null }

& "$PSScriptRoot\build-renderer\portjeuno-renderer.exe" $Zone 2>&1 | Select-String "character|playing|no animation|wrote|could not|webgpu error"

# A sequence writes one file per frame and is left as BMPs; only a single
# still is worth converting here.
if ($Sequence) { return }

Add-Type -AssemblyName System.Drawing
$image = [System.Drawing.Bitmap]::FromFile($bmp)
$image.Save($Out, [System.Drawing.Imaging.ImageFormat]::Png)
$image.Dispose()
Remove-Item $bmp
