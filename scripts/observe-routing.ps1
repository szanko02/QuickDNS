[CmdletBinding()]
param(
    [ValidateRange(2, 1800)]
    [int] $DurationSeconds = 300,
    [ValidateRange(1, 10)]
    [int] $IntervalSeconds = 2,
    [string[]] $AdditionalProcessNames = @()
)

$ErrorActionPreference = 'Stop'
$runName = 'observer-{0}-{1}' -f (Get-Date -Format 'yyyyMMdd-HHmmss'), $PID
$runDirectory = Join-Path ([IO.Path]::GetTempPath()) "quickdns-$runName"
New-Item -ItemType Directory -Path $runDirectory | Out-Null
$samples = Join-Path $runDirectory 'samples.jsonl'
$status = Join-Path $runDirectory 'status.txt'
$stop = Join-Path $runDirectory 'stop'
$programFiles = [Environment]::GetFolderPath([Environment+SpecialFolder]::ProgramFiles)
$clock = [Diagnostics.Stopwatch]::StartNew()
$lastCopies = @{}
$processNames = @('QuickDNS*', 'sing-box', 'codex', 'ChatGPT', 'OpenAI.Codex') + $AdditionalProcessNames

"Running $(Get-Date -Format o)" | Set-Content -LiteralPath $status
try {
    while ($clock.Elapsed.TotalSeconds -lt $DurationSeconds -and !(Test-Path -LiteralPath $stop)) {
        $time = Get-Date -Format o
        $dnsClientId = $null
        foreach ($line in (sc.exe queryex Dnscache)) {
            if ($line -match '^\s*PID\s*:\s*([1-9][0-9]*)\s*$') {
                $dnsClientId = [int]$Matches[1]
                break
            }
        }
        $matchedProcesses = @(Get-Process -Name $processNames -ErrorAction SilentlyContinue)
        if ($null -ne $dnsClientId) {
            $matchedProcesses += @(Get-Process -Id $dnsClientId -ErrorAction SilentlyContinue)
        }
        $processes = @($matchedProcesses | Sort-Object Id -Unique |
            ForEach-Object {
                $started = try { $_.StartTime.ToString('o') } catch { $null }
                [ordered]@{ id = $_.Id; name = $_.ProcessName; path = $_.Path; started = $started }
            })
        $ownerIds = @($processes | ForEach-Object id)
        $connections = @(netstat.exe -ano | Where-Object {
            $_ -match '^\s*(TCP|UDP)\s+\S+\s+\S+\s+.*?\b(\d+)\s*$' -and
            [int]$Matches[2] -in $ownerIds
        } | ForEach-Object { $_.Trim() })

        $interfaces = @([Net.NetworkInformation.NetworkInterface]::GetAllNetworkInterfaces() |
            ForEach-Object {
                $ip = $_.GetIPProperties()
                [ordered]@{
                    name = $_.Name
                    id = $_.Id
                    status = $_.OperationalStatus.ToString()
                    addresses = @($ip.UnicastAddresses | ForEach-Object { $_.Address.ToString() })
                    gateways = @($ip.GatewayAddresses | ForEach-Object { $_.Address.ToString() })
                    dns = @($ip.DnsAddresses | ForEach-Object { $_.ToString() })
                }
            })

        $copyErrors = @()
        foreach ($session in Get-ChildItem -LiteralPath $programFiles -Directory -Filter 'QuickDNS Routing*') {
            $sessionDirectory = Join-Path $runDirectory $session.Name
            foreach ($name in 'routing.json', 'routing.log') {
                $source = Join-Path $session.FullName $name
                try {
                    $file = Get-Item -LiteralPath $source -ErrorAction SilentlyContinue
                    if ($null -eq $file) { continue }
                    $stamp = '{0}:{1}' -f $file.Length, $file.LastWriteTimeUtc.Ticks
                    if ($lastCopies[$source] -eq $stamp) { continue }
                    New-Item -ItemType Directory -Force -Path $sessionDirectory | Out-Null
                    $copy = Join-Path $sessionDirectory $name
                    $pending = "$copy.pending"
                    Copy-Item -LiteralPath $source -Destination $pending
                    Move-Item -LiteralPath $pending -Destination $copy -Force
                    $lastCopies[$source] = $stamp
                } catch {
                    $copyErrors += "$name : $($_.Exception.Message)"
                }
            }
        }

        [ordered]@{
            time = $time
            processes = $processes
            dnsClientId = $dnsClientId
            sockets = $connections
            interfaces = $interfaces
            copyErrors = $copyErrors
        } | ConvertTo-Json -Depth 6 -Compress | Add-Content -LiteralPath $samples -Encoding UTF8

        if ($clock.Elapsed.TotalSeconds + $IntervalSeconds -lt $DurationSeconds) {
            Start-Sleep -Seconds $IntervalSeconds
        } else {
            break
        }
    }
} finally {
    "Stopped $(Get-Date -Format o)" | Add-Content -LiteralPath $status
}

$runDirectory
