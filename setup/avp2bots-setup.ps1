# avp2bots quick install / uninstall for an AvP2 dedicated server (Windows).
# Run through install.bat or uninstall.bat, or: powershell -ExecutionPolicy Bypass -File avp2bots-setup.ps1 install [game folder]
param(
    [Parameter(Mandatory = $true)][ValidateSet("install", "uninstall")][string]$Action,
    [string]$GameDir = ""
)
$ErrorActionPreference = "Stop"
$here = $PSScriptRoot
$backupName = "customrez.txt.before-avp2bots"

function Say([string]$text) { Write-Host $text }
function Fail([string]$text) { Write-Host ""; Write-Host "ERROR: $text" -ForegroundColor Red; exit 1 }

function Test-GameDir([string]$dir) {
    return $dir -and (Test-Path -LiteralPath (Join-Path $dir "AvP2Serv.exe"))
}

function Find-GameDir {
    if ($GameDir) {
        if (Test-GameDir $GameDir) { return (Resolve-Path -LiteralPath $GameDir).Path }
        Fail "No AvP2Serv.exe in '$GameDir'."
    }
    # unzipped into the game folder, or in a folder inside it
    foreach ($d in @($here, (Split-Path $here -Parent))) {
        if (Test-GameDir $d) { return $d }
    }
    $candidates = @(
        (Join-Path ${env:ProgramFiles(x86)} "Aliens vs. Predator 2"),
        (Join-Path $env:ProgramFiles "Aliens vs. Predator 2"),
        "C:\Games\Aliens vs. Predator 2",
        "C:\AvP2"
    )
    foreach ($d in $candidates) {
        if (Test-GameDir $d) {
            $answer = Read-Host "Game folder found: $d  - use it? (Y/N)"
            if ($answer -match '^[Yy]') { return $d }
        }
    }
    # ask with a folder picker
    try {
        Add-Type -AssemblyName System.Windows.Forms
        $dlg = New-Object System.Windows.Forms.FolderBrowserDialog
        $dlg.Description = "Select the Aliens vs. Predator 2 folder (the one with AvP2Serv.exe)"
        if ($dlg.ShowDialog() -eq [System.Windows.Forms.DialogResult]::OK -and (Test-GameDir $dlg.SelectedPath)) {
            return $dlg.SelectedPath
        }
    } catch { }
    $typed = Read-Host "Path of the Aliens vs. Predator 2 folder (the one with AvP2Serv.exe)"
    $typed = $typed.Trim('"', ' ')
    if (Test-GameDir $typed) { return $typed }
    Fail "No AvP2Serv.exe found. Run again and give the game folder."
}

function Assert-ServerStopped {
    if (Get-Process -Name "AvP2Serv" -ErrorAction SilentlyContinue) {
        Fail "AvP2Serv.exe is running. Close the dedicated server first, then run this again."
    }
}

function Assert-Writable([string]$dir) {
    $probe = Join-Path $dir "avp2bots-write-test.tmp"
    try { Set-Content -LiteralPath $probe -Value "x"; Remove-Item -LiteralPath $probe }
    catch { Fail "Cannot write into '$dir'. Right-click the .bat file and choose 'Run as administrator'." }
}

function Read-Lines([string]$path) {
    if (Test-Path -LiteralPath $path) { return @(Get-Content -LiteralPath $path) }
    return @()
}

function Write-Lines([string]$path, $lines) {
    # CRLF, plain ASCII, as the game's own files
    [System.IO.File]::WriteAllText($path, (($lines -join "`r`n") + "`r`n"), [System.Text.Encoding]::ASCII)
}

function Install([string]$game) {
    $srcLto = Join-Path $here "botmod\object.lto"
    $srcNav = Join-Path $here "botnav"
    if (-not (Test-Path -LiteralPath $srcLto)) { Fail "botmod\object.lto is missing next to this script: unzip the whole release first." }

    # 1. files
    $botmod = Join-Path $game "botmod"
    $botnav = Join-Path $game "botnav"
    if ($here -ne $game) {
        New-Item -ItemType Directory -Force -Path $botmod | Out-Null
        New-Item -ItemType Directory -Force -Path $botnav | Out-Null
        Copy-Item -LiteralPath $srcLto -Destination $botmod -Force
        $navs = Get-ChildItem -LiteralPath $srcNav -Filter "*.nav"
        foreach ($n in $navs) { Copy-Item -LiteralPath $n.FullName -Destination $botnav -Force }
        $cfg = Join-Path $botnav "bots.cfg"
        if (Test-Path -LiteralPath $cfg) {
            # an update keeps the admin's settings; the new defaults go next to them
            Copy-Item -LiteralPath (Join-Path $srcNav "bots.cfg") -Destination (Join-Path $botnav "bots.cfg.default") -Force
            Say "Kept your botnav\bots.cfg (the new defaults are in botnav\bots.cfg.default)."
        } else {
            Copy-Item -LiteralPath (Join-Path $srcNav "bots.cfg") -Destination $cfg
        }
        Say ("Copied botmod\object.lto and {0} map files into botnav\." -f $navs.Count)
    }

    # 2. customrez.txt: load botmod instead of the Master Server Patch server code
    $rez = Join-Path $game "customrez.txt"
    $lines = Read-Lines $rez
    if ($lines | Where-Object { $_ -match '^\s*Rez\d+\s*=\s*"botmod"' }) {
        Say "customrez.txt already loads botmod."
    } else {
        $backup = Join-Path $game $backupName
        if ((Test-Path -LiteralPath $rez) -and -not (Test-Path -LiteralPath $backup)) {
            Copy-Item -LiteralPath $rez -Destination $backup
            Say "Saved the original customrez.txt as $backupName."
        }
        $replaced = $false
        $out = @()
        foreach ($l in $lines) {
            if (-not $replaced -and $l -match '^(\s*Rez\d+\s*=\s*)"lithserver\.rez"\s*$') {
                $out += ($Matches[1] + '"botmod"'); $replaced = $true
            } else { $out += $l }
        }
        if (-not $replaced) {
            if (-not ($out | Where-Object { $_ -match '^\s*\[Rez\]' })) { $out += "[Rez]" }
            $n = @($out | Where-Object { $_ -match '^\s*Rez\d+\s*=' }).Count
            $out += ('Rez{0}="botmod"' -f $n)
        }
        Write-Lines $rez $out
        Say "customrez.txt now loads botmod."
    }

    # 3. what the server needs to read Master Server Patch 2.4 players
    if (-not (Test-Path -LiteralPath (Join-Path $game "lithserver.rez"))) {
        Write-Host "WARNING: lithserver.rez (Master Server Patch 2.4) is not in the game folder: MSP 2.4 players will be called NoName." -ForegroundColor Yellow
    }
    Say ""
    Say "Done. Start AvP2Serv.exe as usual with a Deathmatch or Team Deathmatch game; type /bothelp in the server window."
    Say "Settings: botnav\bots.cfg (see ADMIN.md). To remove the bots: uninstall.bat."
}

function Uninstall([string]$game) {
    $answer = Read-Host "Remove the bots from '$game'? (Y/N)"
    if ($answer -notmatch '^[Yy]') { Say "Nothing changed."; exit 0 }

    $rez = Join-Path $game "customrez.txt"
    $backup = Join-Path $game $backupName
    if (Test-Path -LiteralPath $backup) {
        Copy-Item -LiteralPath $backup -Destination $rez -Force
        Remove-Item -LiteralPath $backup
        Say "Restored the original customrez.txt."
    } else {
        $lines = Read-Lines $rez
        $out = @()
        foreach ($l in $lines) {
            if ($l -match '^(\s*Rez\d+\s*=\s*)"botmod"\s*$') { $out += ($Matches[1] + '"lithserver.rez"') } else { $out += $l }
        }
        Write-Lines $rez $out
        Say 'customrez.txt loads lithserver.rez again.'
    }
    foreach ($d in @("botmod", "botnav")) {
        $p = Join-Path $game $d
        if (Test-Path -LiteralPath $p) { Remove-Item -LiteralPath $p -Recurse -Force; Say "Removed $d\." }
    }
    Say ""
    Say "Done: the server is back to the normal Master Server Patch server. The bot logs (avp2bots_*.log, avp2bots_crash.txt) were left in the game folder."
}

$game = Find-GameDir
Say "Game folder: $game"
Assert-ServerStopped
Assert-Writable $game
if ($Action -eq "install") { Install $game } else { Uninstall $game }
