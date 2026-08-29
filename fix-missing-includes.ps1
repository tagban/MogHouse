# Adds the standard headers the engine's module fragments are missing.
#
# These files leaned on the Vulkan module's `export import std;` for headers
# they never included themselves. Removing that import (to escape the MSVC
# std-module bug) exposed every omission at once. Each fix is a real one: the
# file genuinely uses the type and genuinely should include the header.
$map = @{
  'string'          = 'string';      'wstring'        = 'string'
  'runtime_error'   = 'stdexcept';   'invalid_argument' = 'stdexcept'
  'logic_error'     = 'stdexcept';   'out_of_range'   = 'stdexcept'
  'length_error'    = 'stdexcept';   'domain_error'   = 'stdexcept'
  'vector'          = 'vector';      'array'          = 'array'
  'unordered_map'   = 'unordered_map'; 'unordered_set' = 'unordered_set'
  'map'             = 'map';         'set'            = 'set'
  'optional'        = 'optional';    'variant'        = 'variant'
  'function'        = 'functional';  'shared_ptr'     = 'memory'
  'unique_ptr'      = 'memory';      'make_shared'    = 'memory'
  'make_unique'     = 'memory';      'weak_ptr'       = 'memory'
  'thread'          = 'thread';      'mutex'          = 'mutex'
  'atomic'          = 'atomic';      'chrono'         = 'chrono'
  'span'            = 'span';        'string_view'    = 'string_view'
  'filesystem'      = 'filesystem';  'stringstream'   = 'sstream'
  'ostringstream'   = 'sstream';     'cout'           = 'iostream'
  'accumulate'      = 'numeric';     'iota'           = 'numeric'
  'ranges'          = 'ranges';      'format'         = 'format'
  'bit_cast'        = 'bit';         'byte'           = 'cstddef'
  'numeric_limits'  = 'limits';      'tuple'          = 'tuple'
  'pair'            = 'utility';     'move'           = 'utility'
}

for ($pass = 1; $pass -le 25; $pass++) {
    $out = & "$PSScriptRoot\build-engine.bat" 2>&1 | Out-String
    $errors = [regex]::Matches($out, "(?m)^(.+?)\((\d+)\): error C\d+: '([A-Za-z_]+)': is not a member of 'std'")
    if ($errors.Count -eq 0) {
        if ($out -match 'BUILD EXIT: 0') { Write-Output "PASS $pass : BUILD SUCCEEDED"; break }
        Write-Output "PASS $pass : no missing-std errors left; other errors remain"
        [regex]::Matches($out, "(?m)^.+error C\d+:.*$") | Select-Object -First 6 | ForEach-Object { Write-Output ("   " + $_.Value.Trim()) }
        break
    }

    $added = 0
    foreach ($g in ($errors | Group-Object { $_.Groups[1].Value })) {
        $file = $g.Name
        if (-not (Test-Path $file)) { continue }
        $needed = $g.Group | ForEach-Object { $map[$_.Groups[3].Value] } | Where-Object { $_ } | Select-Object -Unique
        $text = Get-Content $file -Raw
        foreach ($h in $needed) {
            if ($text -match "#include <$h>") { continue }
            $text = $text -replace "(?m)^module;\r?\n", "module;`r`n`r`n#include <$h>`r`n"
            $added++
            Write-Output ("  + <{0}> -> {1}" -f $h, (Split-Path $file -Leaf))
        }
        Set-Content $file $text -NoNewline
    }

    Write-Output "PASS $pass : added $added include(s)"
    if ($added -eq 0) { Write-Output "  (no progress - stopping)"; break }
}
