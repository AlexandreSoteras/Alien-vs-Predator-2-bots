<#
.SYNOPSIS
  Start, query or stop the isolated AvP2 test dedicated server in avp2bots\testserver.

.DESCRIPTION
  The test server folder holds hard links to the game's binaries and .REZ archives plus its own
  configs (ServerData\botstest.txt: LAN only, deathmatch on dm_compound). Nothing in the real game
  folder is touched.

  customrez.txt in the test folder decides which server game code is loaded last:
    -Code msp     -> lithserver.rez (the Master Server Patch's own server code)
    -Code retail  -> nothing extra (the stock 1.0.9.6 object.lto from the archives)
    -Code bots    -> botmod  (a folder or .rez holding our object.lto, avp2bots\testserver\botmod)

.EXAMPLE
  .\testserver.ps1 start -Code retail
  .\testserver.ps1 status
  .\testserver.ps1 stop
#>
param(
    [Parameter(Mandatory = $true, Position = 0)][ValidateSet('start', 'stop', 'status', 'restart')] [string]$Action,
    [ValidateSet('msp', 'retail', 'bots')] [string]$Code = 'retail',
    [string]$Config = 'botstest',
    [int]$WaitSeconds = 10
)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$ts = Join-Path $root 'testserver'
$exe = Join-Path $ts 'AvP2Serv.exe'
$query = Join-Path $root 'navgen\query.py'

function Get-TestServer {
    Get-Process AvP2Serv -ErrorAction SilentlyContinue | Where-Object { $_.Path -eq $exe }
}

function Stop-TestServer {
    $p = Get-TestServer
    if ($p) { $p | Stop-Process -Force; Start-Sleep -Seconds 1; "stopped pid $($p.Id -join ',')" } else { 'not running' }
}

function Set-Code([string]$which) {
    $line = switch ($which) {
        'msp' { "[Rez]`r`nRez0=`"lithserver.rez`"`r`n" }
        'retail' { "[Rez]`r`n" }
        'bots' { "[Rez]`r`nRez0=`"botmod`"`r`n" }
    }
    Set-Content -Path (Join-Path $ts 'customrez.txt') -Value $line -NoNewline -Encoding ascii
}

function Show-Status {
    $p = Get-TestServer
    if (-not $p) { 'server: not running'; return }
    "server: running, pid $($p.Id), started $($p.StartTime)"
    & py -I $query 127.0.0.1 27888 | Select-String -Pattern 'reply|hostname|mapname|gametype|numplayers|mspatch' | ForEach-Object { $_.Line }
}

switch ($Action) {
    'stop' { Stop-TestServer }
    'status' { Show-Status }
    { $_ -in 'start', 'restart' } {
        if (Get-TestServer) { Stop-TestServer | Out-Null }
        Set-Code $Code
        $p = Start-Process -FilePath $exe -ArgumentList "-config $Config" -WorkingDirectory $ts -PassThru
        Start-Sleep -Seconds $WaitSeconds
        $p.Refresh()
        if ($p.HasExited) { "server exited early with code $($p.ExitCode)"; exit 1 }
        "started pid $($p.Id) with code '$Code'"
        Show-Status
    }
}
