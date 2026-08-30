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
    [string]$Time = "1200"
)

$scratch = Split-Path -Parent $Out
$bmp = [System.IO.Path]::ChangeExtension($Out, ".bmp")

$env:PORTJEUNO_SCREENSHOT = $bmp
$env:PORTJEUNO_TIME = $Time
if ($Character) { $env:PORTJEUNO_CHARACTER = $Character } else { Remove-Item env:PORTJEUNO_CHARACTER -EA SilentlyContinue }
if ($Look) { $env:PORTJEUNO_LOOK = $Look } else { Remove-Item env:PORTJEUNO_LOOK -EA SilentlyContinue }
if ($CharacterAt) { $env:PORTJEUNO_CHARACTER_AT = $CharacterAt } else { Remove-Item env:PORTJEUNO_CHARACTER_AT -EA SilentlyContinue }
if ($Facing) { $env:PORTJEUNO_CHARACTER_FACING = $Facing } else { Remove-Item env:PORTJEUNO_CHARACTER_FACING -EA SilentlyContinue }
if ($Animation) { $env:PORTJEUNO_ANIMATION = $Animation } else { Remove-Item env:PORTJEUNO_ANIMATION -EA SilentlyContinue }
if ($Frame) { $env:PORTJEUNO_FRAME = $Frame } else { Remove-Item env:PORTJEUNO_FRAME -EA SilentlyContinue }
if ($Camera) { $env:PORTJEUNO_CAMERA = $Camera } else { Remove-Item env:PORTJEUNO_CAMERA -EA SilentlyContinue }
if ($CameraLook) { $env:PORTJEUNO_CAMERA_LOOK = $CameraLook } else { Remove-Item env:PORTJEUNO_CAMERA_LOOK -EA SilentlyContinue }

& "$PSScriptRoot\build-renderer\portjeuno-renderer.exe" $Zone 2>&1 | Select-String "character|playing|no animation|wrote|could not|webgpu error"

Add-Type -AssemblyName System.Drawing
$image = [System.Drawing.Bitmap]::FromFile($bmp)
$image.Save($Out, [System.Drawing.Imaging.ImageFormat]::Png)
$image.Dispose()
Remove-Item $bmp
