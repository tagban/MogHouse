# Opens the renderer on a zone, optionally with a character standing in it.
#
# The counterpart to shot.ps1, which captures one frame and exits. This one
# just runs.
#
#   .\run.ps1                                  East Sarutabaruta, no character
#   .\run.ps1 -Look 1,0,49,30,30,30,30         a hume male in samurai armour
#   .\run.ps1 -ZoneId 235 -Look 8,0,0,5,5,5,5  a galka in Bastok Markets
#
# If the machine refuses to run local scripts, this needs no settings
# change:
#
#   powershell -ExecutionPolicy Bypass -File .\run.ps1 -ZoneId 235
#
# In the window: wasd walks, drag looks, c drops the character where you are
# standing, tab orbits, p prints the position, escape quits.
param(
    # race,face,head,body,hands,legs,feet - see docs/characters.md.
    #
    # Typed as an array so an unquoted 1,0,0,1,1,1,1 works: PowerShell parses
    # that as seven values, and binding it to a [string] would silently join
    # them with spaces instead of commas.
    [string[]]$Look = @(),
    # An NPC that lives in one DAT, as a path. Ignored if -Look is given.
    [string]$Character = "",
    [string]$Animation = "idl0",
    # A zone DAT path, or use -ZoneId instead.
    [string]$Zone = "",
    # FFXI zone id. East Sarutabaruta is 116, Bastok Markets 235.
    [int]$ZoneId = 116,
    [string]$At = "",
    [string]$Facing = "",
    # Vana'diel clock, as hhmm. Unset lets the day run.
    [string]$Time = "",
    [string]$Install = "C:\Program Files (x86)\PlayOnline\SquareEnix\FINAL FANTASY XI"
)

$ErrorActionPreference = "Stop"
Set-Location $PSScriptRoot

# The key tables are lifted from the retail client and never committed, so
# generate them on first run rather than failing with a bare "could not read".
$keys = Join-Path $PSScriptRoot "keys"
if (-not (Test-Path (Join-Path $keys "mzb_key_table.bin"))) {
    Write-Host "generating key tables..."
    python tools\keytables.py
}

if (-not $Zone) {
    # A zone's MZB file id is its zone id plus 100.
    $fileId = $ZoneId + 100
    $Zone = python -c @"
import sys
sys.path.insert(0, 'tools')
from filetable import FileTable
path = FileTable(r'$Install').path($fileId)
print(path if path else '')
"@
    if (-not $Zone) {
        throw "zone $ZoneId (file $fileId) is not installed"
    }
    Write-Host "zone $ZoneId -> $Zone"
}

$env:MOGHOUSE_FFXI_KEYTABLE = Join-Path $keys "mzb_key_table.bin"
$env:MOGHOUSE_FFXI_KEYTABLE2 = Join-Path $keys "mmb_key_table2.bin"
$env:MOGHOUSE_FFXI_INSTALL = $Install
$lookValue = $Look -join ","
$env:MOGHOUSE_LOOK = if ($lookValue) { $lookValue } else { $null }
$env:MOGHOUSE_CHARACTER = if ($Character) { $Character } else { $null }
$env:MOGHOUSE_ANIMATION = if ($lookValue -or $Character) { $Animation } else { $null }
$env:MOGHOUSE_CHARACTER_AT = if ($At) { $At } else { $null }
$env:MOGHOUSE_CHARACTER_FACING = if ($Facing) { $Facing } else { $null }
$env:MOGHOUSE_TIME = if ($Time) { $Time } else { $null }

# Left over from a shot.ps1 run in the same window these would silently turn
# this into a screenshot that exits immediately.
$env:MOGHOUSE_SCREENSHOT = $null
$env:MOGHOUSE_SCREENSHOT_SEQUENCE = $null
$env:MOGHOUSE_FRAME = $null
$env:MOGHOUSE_CAMERA = $null
$env:MOGHOUSE_CAMERA_LOOK = $null

& "$PSScriptRoot\build-renderer\moghouse-renderer.exe" $Zone
