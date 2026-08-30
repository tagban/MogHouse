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

$env:MOGHOUSE_SCREENSHOT = $bmp
$env:MOGHOUSE_TIME = $Time
if ($Character) { $env:MOGHOUSE_CHARACTER = $Character } else { $env:MOGHOUSE_CHARACTER = $null }
if ($Look) { $env:MOGHOUSE_LOOK = $Look } else { $env:MOGHOUSE_LOOK = $null }
if ($CharacterAt) { $env:MOGHOUSE_CHARACTER_AT = $CharacterAt } else { $env:MOGHOUSE_CHARACTER_AT = $null }
if ($Facing) { $env:MOGHOUSE_CHARACTER_FACING = $Facing } else { $env:MOGHOUSE_CHARACTER_FACING = $null }
if ($Animation) { $env:MOGHOUSE_ANIMATION = $Animation } else { $env:MOGHOUSE_ANIMATION = $null }
if ($Sequence) { $env:MOGHOUSE_SCREENSHOT_SEQUENCE = $Sequence } else { $env:MOGHOUSE_SCREENSHOT_SEQUENCE = $null }
if ($Frame) { $env:MOGHOUSE_FRAME = $Frame } else { $env:MOGHOUSE_FRAME = $null }
if ($Camera) { $env:MOGHOUSE_CAMERA = $Camera } else { $env:MOGHOUSE_CAMERA = $null }
if ($CameraLook) { $env:MOGHOUSE_CAMERA_LOOK = $CameraLook } else { $env:MOGHOUSE_CAMERA_LOOK = $null }

& "$PSScriptRoot\build-renderer\moghouse-renderer.exe" $Zone 2>&1 | Select-String "character|playing|no animation|wrote|could not|webgpu error"

# A sequence writes one file per frame and is left as BMPs; only a single
# still is worth converting here.
if ($Sequence) { return }

Add-Type -AssemblyName System.Drawing
$image = [System.Drawing.Bitmap]::FromFile($bmp)
$image.Save($Out, [System.Drawing.Imaging.ImageFormat]::Png)
$image.Dispose()
Remove-Item $bmp
