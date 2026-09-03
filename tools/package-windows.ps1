<#
.SYNOPSIS
    Builds a MogHouse alpha someone else can unzip and run.

.DESCRIPTION
    The goal is a folder someone opens and understands: one executable, their
    own settings beside it, and everything else out of the way.

        MogHouse XI.exe          what you run
        moghouse-settings.json   volume, draw distance - written on first run
        ffxi-server-profiles.json  the servers you have added
        ffxi-install.json        where you told it the game is
        README.txt
        data\                    hidden; the runtime and its files

    Everything the client needs travels with it except the game itself, which
    the player already has.

    In data\:

      MogHouse XI.exe is published as a single file, so the .NET runtime and
      every managed library are inside it rather than beside it. Self-contained
      on purpose: a tester who has to install .NET 10 first is a tester who
      does not test tonight.

      moghouse_interop.dll, SDL3.dll
          The renderer. Dawn is linked into the first of those, so there is no
          third library to lose. These cannot go inside the single file,
          because the renderer looks for its own assets beside whichever
          directory the library was loaded from - so they live in data\ and
          the assets live with them.

      assets\  - the glyph atlas, the interior table, and the water
      res\     - compress.dat and decompress.dat, the protocol's Huffman
                 tables. Required: without them the client cannot talk to any
                 server. Not in a retail install; they come from
                 LandSandBoat's own res directory.
      keys\    - the MZB and MMB key tables. Without these no zone decrypts.
      zones\   - zone lines, if -ZoneData was given.

    What does not ship:

      The game's DATs. The client finds an existing install and reads it.

      The navmeshes - 422MB, and they only feed the launcher's flat map. The
      world's own collision comes from the DATs, so movement is unaffected.

.EXAMPLE
    pwsh tools\package-windows.ps1 -Version 0.1.2

.EXAMPLE
    pwsh tools\package-windows.ps1 -Version 0.1.2 -NoWater -ZoneData C:\LandSandBoat\data\zones
#>
[CmdletBinding()]
param(
    # Stamped into the folder and zip names.
    #
    # The window title is set separately, in kWindowTitle in renderer/viewer.h,
    # and this does not change it - so the two can disagree. They should not:
    # a tester reporting a bug reads the title bar, not the zip they unpacked.
    [string]$Version = "0.1.2",

    # Where the staging folder and the zip are written.
    [string]$Output = "dist",

    # LandSandBoat's res directory, holding compress.dat and decompress.dat.
    [string]$Res = "C:\Users\Gaming\Desktop\LandSandBoat\res",

    # LandSandBoat's data\zones, for zone lines. Omitted by default: 34MB, and
    # the client works without it.
    [string]$ZoneData = "",

    # Leave the water out. It is about 50MB of surfaces derived from the
    # server's collision meshes; without it, canals and seas are dry.
    [switch]$NoWater,

    # Skip the native build and use whatever is already in build-renderer.
    [switch]$NoBuild
)

$ErrorActionPreference = "Stop"

$root = Split-Path -Parent $PSScriptRoot
$staging = Join-Path $root "$Output\MogHouse-XI-Alpha-$Version"
$zip = Join-Path $root "$Output\MogHouse-XI-Alpha-$Version-win-x64.zip"

function Step($text) { Write-Host "==> $text" -ForegroundColor Cyan }
function Warn($text) { Write-Host "    ! $text" -ForegroundColor Yellow }

# --- the native renderer -----------------------------------------------------

if (-not $NoBuild) {
    Step "Building the renderer"
    $vs = "C:\Program Files\Microsoft Visual Studio\18\Community"
    $cmake = "$vs\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
    if (-not (Test-Path $cmake)) {
        throw "Visual Studio's cmake was not found at $cmake. Pass -NoBuild to use the existing build."
    }
    # Through vcvars: without it the compiler is not on PATH and cmake reports
    # success having done nothing, which leaves a stale DLL in the package.
    cmd /c "`"$vs\VC\Auxiliary\Build\vcvars64.bat`" >nul 2>&1 && `"$cmake`" --build `"$root\build-renderer`"" | Out-Null
    if ($LASTEXITCODE -ne 0) { throw "The renderer did not build." }
}

$native = Join-Path $root "build-renderer\moghouse_interop"
foreach ($needed in @("moghouse_interop.dll", "SDL3.dll")) {
    if (-not (Test-Path (Join-Path $native $needed))) {
        throw "$needed is missing from $native - build the renderer first."
    }
}

# --- the managed client ------------------------------------------------------

Step "Publishing the client"
if (Test-Path $staging) { Remove-Item $staging -Recurse -Force }
New-Item -ItemType Directory -Force -Path $staging | Out-Null

# Into data\ first. Single file, so this produces one executable and a handful
# of native libraries rather than two hundred assemblies; the executable is
# then moved up to where the player will look for it, and everything it needs
# stays down here.
$data = Join-Path $staging "data"
New-Item -ItemType Directory -Force -Path $data | Out-Null

dotnet publish (Join-Path $root "src\MogHouse.App\MogHouse.App.csproj") `
    -c Release -r win-x64 --self-contained true `
    -p:PublishSingleFile=true `
    -p:IncludeNativeLibrariesForSelfExtract=true `
    -p:EnableCompressionInSingleFile=true `
    -p:DebugType=none -p:GenerateDocumentationFile=false `
    -o $data --nologo -v quiet
if ($LASTEXITCODE -ne 0) { throw "The client did not publish." }

# Debug symbols are not worth 80MB to someone who cannot read them.
Get-ChildItem $data -Filter *.pdb -Recurse -ErrorAction SilentlyContinue | Remove-Item -Force

$exe = Join-Path $data "MogHouse XI.exe"
if (-not (Test-Path $exe)) { throw "The published executable is not where it was expected: $exe" }
Move-Item $exe (Join-Path $staging "MogHouse XI.exe") -Force

# --- everything it needs beside it -------------------------------------------

Step "Copying the renderer"
Copy-Item (Join-Path $native "moghouse_interop.dll") $data -Force
Copy-Item (Join-Path $native "SDL3.dll") $data -Force

Step "Copying assets"
$assets = Join-Path $data "assets"
New-Item -ItemType Directory -Force -Path $assets | Out-Null
Copy-Item (Join-Path $root "renderer\assets\font.*") $assets -Force
Copy-Item (Join-Path $root "renderer\assets\subrooms.txt") $assets -Force
Copy-Item (Join-Path $root "renderer\assets\hidden-models.txt") $assets -Force
Copy-Item (Join-Path $root "renderer\assets\burrowers.txt") $assets -Force

# --- where bug reports go -----------------------------------------------------

# `/bug` posts to a Discord webhook and a tester cannot configure one, so a
# shipped build carries it. In the build, not in the repository: anything
# shipped can be taken apart, which a webhook survives - the worst it allows is
# posting into one channel, revoked in a click - while a repository is
# permanently searchable and keeps what you delete.
#
# Without one, /bug still writes its local file and says nobody has seen it.
Step "Bug report webhook"
$webhook = $env:MOGHOUSE_BUG_WEBHOOK
if (-not $webhook) {
    $local = Join-Path $env:LOCALAPPDATA "MogHouse\bug-webhook.txt"
    if (Test-Path $local) { $webhook = (Get-Content $local -Raw).Trim() }
}
if ($webhook) {
    Set-Content -Path (Join-Path $data "bug-webhook.txt") -Value $webhook -NoNewline
    Write-Host "    bug-webhook.txt (reports will reach the channel)"
} else {
    Warn "No bug webhook: /bug will write its local file and go no further. Set MOGHOUSE_BUG_WEBHOOK to include one."
}


if ($NoWater) {
    Warn "No water: canals and seas will be dry."
} else {
    $water = Join-Path $root "renderer\assets\water"
    $files = @(Get-ChildItem $water -Filter *.water -ErrorAction SilentlyContinue)
    if ($files.Count -eq 0) {
        Warn "No .water files found - run tools\makewater.py first, or pass -NoWater."
    } else {
        New-Item -ItemType Directory -Force -Path (Join-Path $assets "water") | Out-Null
        Copy-Item "$water\*.water" (Join-Path $assets "water") -Force
        Write-Host "    $($files.Count) zones of water"
    }
}

Step "Copying the key tables"
$keys = Join-Path $data "keys"
New-Item -ItemType Directory -Force -Path $keys | Out-Null
Copy-Item (Join-Path $root "keys\*.bin") $keys -Force

Step "Copying the compression tables"
$resOut = Join-Path $data "res"
New-Item -ItemType Directory -Force -Path $resOut | Out-Null
foreach ($table in @("compress.dat", "decompress.dat")) {
    $from = Join-Path $Res $table
    if (-not (Test-Path $from)) {
        throw "$table was not found in $Res. The client cannot connect to anything without it; point -Res at LandSandBoat's res directory."
    }
    Copy-Item $from $resOut -Force
}

if ($ZoneData) {
    Step "Copying zone data"
    if (-not (Test-Path $ZoneData)) { throw "No zone data at $ZoneData." }
    $zonesOut = Join-Path $data "zones"
    Copy-Item $ZoneData $zonesOut -Recurse -Force
    Write-Host "    $((Get-ChildItem $zonesOut -Directory).Count) zones"
} else {
    Warn "No zone data: walking to the edge of a zone will not change zones. Use !zone, or pass -ZoneData."
}

# --- what to do with it ------------------------------------------------------

Step "Writing README"
@"
MogHouse XI - Alpha $Version
============================

A from-scratch Final Fantasy XI client. This is an alpha: it is missing a
great deal, and the parts that are here are the parts that have been built so
far rather than the parts you would miss least.

What you need
-------------

  * A Final Fantasy XI installation, updated to the AUGUST 2026 patch. The
    client finds it and reads the game's own files - models, textures, zones,
    music. Nothing here replaces them and no game data is included.
  * A private server to connect to, running that same version, and an account
    on it.

This is not backwards compatible. An older install, or an older server, will
not work, and it will not always fail in an obvious way: file ids move between
versions, so the wrong model loads, and packet layouts shift, so fields are
read from the wrong place. If something is odd in a way this README does not
explain, check the version first.

Running it
----------

  1. Unzip anywhere. There is no installer and nothing is written outside this
     folder.
  2. Run "MogHouse XI.exe".
  3. Confirm where the game is, enter your server's address, then log in.

The data folder beside it holds the renderer and the files it reads. Nothing
in there needs touching, which is why it is hidden; delete the whole folder to
remove the client completely.

Settings live in moghouse-settings.json beside the exe, and the servers you
add live beside that. Both are plain text and safe to edit while the client is
closed. bodyDrawDistance is the one worth knowing: 0 draws every character the
client can, and a smaller number is how a machine short of headroom keeps up.

Controls
--------

  WASD          walk                    Shift   run
  Mouse drag    look                    Space   jump
  R             auto-run                Tab     orbit
  M             hold the map north-up   U       back out if collision traps you
  + and -       music volume            P       print position to the log
  / and !       open chat, with the key already typed

Known missing, so you do not report what is already known
---------------------------------------------------------

  * Combat. You can walk, talk, zone and look at the world; you cannot fight.
  * Telepoint and Homepoint crystals are invisible.
  * Some creatures have no model and do not appear.
  * Hair colour is wrong for some faces.
  * There is no full-screen map yet.

If something is wrong
---------------------

There are two logs beside the exe: moghouse.log for the client and
moghouse.log.renderer for the world. Both are plain text, and between them
they usually say what happened. Attaching them to a bug report is the single
most useful thing you can do.

Report bugs from inside the game with the link in the top-left corner, or at
the GitHub issues page. There is a Discord link beside it.
"@ | Set-Content (Join-Path $staging "README.txt") -Encoding utf8

# --- zip ---------------------------------------------------------------------

# data\ is deliberately *not* hidden here. Twice wrong if it is:
# Compress-Archive silently skips hidden folders, so the zip came out holding
# an executable and a README and nothing else - and a zip carries no Windows
# attributes anyway, so it would not have arrived hidden regardless. The client
# hides the folder itself on first run, where it actually sticks.

Step "Compressing"
if (Test-Path $zip) { Remove-Item $zip -Force }
Compress-Archive -Path "$staging\*" -DestinationPath $zip -CompressionLevel Optimal

$size = [math]::Round((Get-Item $zip).Length / 1MB, 1)
Write-Host ""
Write-Host "$zip" -ForegroundColor Green
Write-Host "    $size MB" -ForegroundColor Green
