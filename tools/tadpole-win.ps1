# Tadpole on Windows -- run a title under glasspole with the viewer attached.
#
#   .\tools\tadpole-win.ps1 -List                 what is installed
#   .\tools\tadpole-win.ps1 -App "Pet Pad"        run by package name or ID
#   .\tools\tadpole-win.ps1 -Swf <guest path>     run a .swf directly
#
# The Windows spelling of `./tadpole.sh --app`, and deliberately the same
# shape: match the package in LF/Bulk/ProgramFiles by directory name or
# meta.inf Name, read its AppSo entry, and hand a .swf to saplayer or a
# native App.so to AppManager with a player signed in.
#
# What tadpole.sh gets from Linux for free and this must arrange by hand:
#  * the event and audio "FIFOs" are plain pre-created files here -- one
#    writer (the viewer), one reader (the shim), coherent through the page
#    cache, and glasspole's mkfifo honestly refuses so the files must exist
#    BEFORE the guest looks for them;
#  * guest-absolute env paths are spelled drive-relative (/Users/...) so the
#    emulator's sysroot fallthrough opens them natively and ':' stays free
#    to separate LD_LIBRARY_PATH entries.

param(
    [switch]$List,
    [string]$App,
    [string]$Swf,
    [int]$Scale = 2,
    [switch]$NoViewer
)

$ErrorActionPreference = "Stop"
$Proj = Split-Path -Parent $PSScriptRoot
$Sysroot = Join-Path $Proj "runtime\sysroot"
$PF = Join-Path $Sysroot "LF\Bulk\ProgramFiles"
$Glasspole = Join-Path $Proj "glasspole\build\glasspole.exe"
$Viewer = Join-Path $Proj "tadpole\viewer\build\tadpole-view.exe"

# Drive-relative spelling of a full path: C:\x\y -> /x/y  (see header)
function DriveRel([string]$p) { return ($p -replace '^[A-Za-z]:', '' -replace '\\', '/') }

if ($List) {
    "{0,-28} {1,-24} {2}" -f "PACKAGE", "NAME", "ENTRY"
    Get-ChildItem $PF -Directory | ForEach-Object {
        $meta = Join-Path $_.FullName "meta.inf"
        if (Test-Path $meta) {
            $t = Get-Content $meta -Raw
            $name  = if ($t -match 'Name="([^"]*)"')  { $Matches[1] } else { "" }
            $entry = if ($t -match 'AppSo="([^"]*)"') { $Matches[1] } else { "" }
            "{0,-28} {1,-24} {2}" -f $_.Name, $name, $entry
        }
    }
    exit 0
}

# ---- resolve what to run --------------------------------------------------
$guestProg = $null; $guestArg = $null; $title = $null
if ($Swf) {
    $guestProg = "/LF/Base/Flash/bin/saplayer"; $guestArg = $Swf; $title = $Swf
} elseif ($App) {
    $found = $null
    foreach ($d in Get-ChildItem $PF -Directory) {
        $meta = Join-Path $d.FullName "meta.inf"
        if (-not (Test-Path $meta)) { continue }
        $t = Get-Content $meta -Raw
        $name = if ($t -match 'Name="([^"]*)"') { $Matches[1] } else { "" }
        if ($d.Name -like "*$App*" -or $name -like "*$App*") { $found = $d; $foundName = $name; $foundMeta = $t; break }
    }
    if (-not $found) { Write-Error "no app matching '$App' -- try -List" }
    if (-not ($foundMeta -match 'AppSo="([^"]*)"')) { Write-Error "no AppSo in $($found.Name)/meta.inf" }
    $entry = $Matches[1]
    $guestPath = "/LF/Bulk/ProgramFiles/$($found.Name)/$entry"
    $title = $foundName
    if ($entry -like "*.swf") {
        $guestProg = "/LF/Base/Flash/bin/saplayer"; $guestArg = $guestPath
    } else {
        # Native: AppManager runs it, with a player signed in (see tadpole.sh
        # for why -- profile reads segfault without one).
        $player = "1"
        $local = Join-Path $Sysroot "LF\Bulk\Data\Local"
        $prof = Get-ChildItem $local -Directory -ErrorAction SilentlyContinue |
                Where-Object { $_.Name -match '^[0-9]+$' } | Select-Object -First 1
        if ($prof) { $player = $prof.Name }
        $guestProg = "/LF/Base/bin/AppManager"; $guestArg = @($guestPath, $player)
        $title = "$foundName (native)"
    }
} else {
    Write-Host "usage: tadpole-win.ps1 -List | -App <name> | -Swf <guest path>"
    exit 2
}

# ---- the shared directory -------------------------------------------------
# DELIBERATELY NO ev0..ev5 AND NO audio FILE. Making the event nodes openable
# sends libEvent down its raw-interface path, which arms a poll timer with
# setitimer(2) -- ENOSYS in glasspole today, and on that failure the whole
# event manager shuts down and the title exits. Absent nodes take the proven
# no-input path instead: the title runs and renders, touchless, until
# setitimer and signal delivery exist in the emulator. The audio file is
# omitted for the same shape of reason -- with no viewer-side reader the
# shim's paced discard is correct, and an ever-growing file is not.
$RunDir = Join-Path $Proj "build\tadpole-run"
New-Item -ItemType Directory -Force $RunDir | Out-Null
foreach ($n in 0..5) {
    $f = Join-Path $RunDir "ev$n"
    if (Test-Path $f) { Remove-Item $f -Force }
}
$audio = Join-Path $RunDir "audio"
if (Test-Path $audio) { Remove-Item $audio -Force }

$R = DriveRel $Proj
$env:TADPOLE_DIR = (DriveRel $RunDir)

Write-Host "=== $title ==="
$gpArgs = @("--sysroot", "runtime/sysroot",
    "-E", "LD_LIBRARY_PATH=$R/runtime/shimlibs-gl:$R/runtime/shimlibs-z:$R/runtime/shimlibs:$R/runtime/libs",
    "-E", "TADPOLE_DIR=$($env:TADPOLE_DIR)",
    "-E", "TSLIB_CONFFILE=/nonexistent-ts.conf",
    "-E", "TADPOLE_SYSROOT=$R/runtime/sysroot",
    $guestProg) + $guestArg
$guest = Start-Process -FilePath $Glasspole -ArgumentList $gpArgs `
    -WorkingDirectory $Proj -PassThru `
    -RedirectStandardOutput (Join-Path $RunDir "guest-out.log") `
    -RedirectStandardError  (Join-Path $RunDir "guest-err.log")
Write-Host "guest pid $($guest.Id); logs in build\tadpole-run\"

if ($NoViewer) { exit 0 }

Start-Sleep 3          # let the shim create state.bin/fb0.bin first
$view = Start-Process -FilePath $Viewer -ArgumentList @("-s", "$Scale") `
    -WorkingDirectory $Proj -PassThru
Write-Host "viewer pid $($view.Id) -- close its window to stop both"
Wait-Process -Id $view.Id
if (-not $guest.HasExited) {
    $guest.CloseMainWindow() | Out-Null
    Start-Sleep 3
    if (-not $guest.HasExited) { Stop-Process -Id $guest.Id -Force }
}
Write-Host "stopped."
