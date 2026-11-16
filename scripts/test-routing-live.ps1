[CmdletBinding()]
param(
    [Parameter(Mandatory)][string] $InternetAdapter,
    [string] $LocalAdapter,
    [string] $BuildDirectory = 'build',
    [string] $Url = 'https://example.com/',
    [string] $DisconnectAdapter,
    [switch] $CheckCodex,
    [switch] $CheckSplitDns,
    [string] $SecondDnsAdapter,
    [string] $PrimaryDnsProbeDomain = 'example.com'
)

$ErrorActionPreference = 'Stop'
$principal = [Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()
if (!$principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    throw 'Run in an elevated PowerShell.'
}
function Get-ActiveTun([string] $Alias = 'QuickDNS TUN*') {
    [Net.NetworkInformation.NetworkInterface]::GetAllNetworkInterfaces() |
        Where-Object { $_.Name -like $Alias -and $_.OperationalStatus -eq 'Up' }
}
if (Get-ActiveTun) {
    throw 'Stop the existing QuickDNS TUN session first.'
}
if ($InternetAdapter -eq $LocalAdapter -or ($DisconnectAdapter -and $DisconnectAdapter -in @($InternetAdapter, $LocalAdapter))) {
    throw 'Use a distinct interface for each adapter parameter.'
}
if ($CheckSplitDns) {
    if (!$SecondDnsAdapter -or $InternetAdapter -eq $SecondDnsAdapter -or $CheckCodex -or $DisconnectAdapter) {
        throw 'For a DNS-only run specify distinct -InternetAdapter and -SecondDnsAdapter values.'
    }
} elseif (!$LocalAdapter) {
    throw 'Specify -LocalAdapter for the application routing tests.'
}

$build = (Resolve-Path -LiteralPath $BuildDirectory).Path
$generator = Get-ChildItem -LiteralPath $build -Filter test_dialog_app_routing.exe -Recurse |
    Select-Object -First 1 -ExpandProperty FullName
if (!$generator) { throw 'Build test_dialog_app_routing first.' }
$engine = (Resolve-Path -LiteralPath (Join-Path $build 'routing-engine/sing-box.exe')).Path
$work = Join-Path ([IO.Path]::GetTempPath()) ('quickdns-live-' + [guid]::NewGuid())
New-Item -ItemType Directory -Path $work | Out-Null
Write-Host "Results: $work"
$target = Join-Path $work 'target.exe'
$control = Join-Path $work 'control.exe'
$curl = Join-Path ([Environment]::SystemDirectory) 'curl.exe'
Copy-Item -LiteralPath $curl -Destination $target
Copy-Item -LiteralPath $curl -Destination $control

function Invoke-Probe($Executable, $Label, $PinnedAddress) {
    $arguments = @('-q', '-4', '--noproxy', '*', '--silent', '--show-error', '--fail',
        '--connect-timeout', '4', '--max-time', '6', '--output', 'NUL', '--write-out', '%{remote_ip}')
    if ($PinnedAddress) {
        $uri = [uri]$Url
        $arguments += @('--resolve', ('{0}:{1}:{2}' -f $uri.DnsSafeHost, $uri.Port, $PinnedAddress))
    }
    $stderr = Join-Path $work "$Label.stderr.txt"
    $stdout = Join-Path $work "$Label.stdout.txt"
    $arguments += ('"{0}"' -f $Url)
    $probe = Start-Process -FilePath $Executable -ArgumentList $arguments -WindowStyle Hidden -PassThru -Wait `
        -RedirectStandardError $stderr -RedirectStandardOutput $stdout
    $remote = Get-Content -LiteralPath $stdout -Raw
    $code = $probe.ExitCode
    $result = [pscustomobject]@{ label = $Label; exitCode = $code; remote = [string]$remote }
    $result | ConvertTo-Json -Compress | Add-Content -LiteralPath (Join-Path $work 'results.jsonl')
    Write-Host "$Label returned $code"
    return $result
}

function Invoke-CodexProbe($Label, [bool] $ExpectOnline = $true) {
    $stdout = Join-Path $work "$Label.json"
    $stderr = Join-Path $work "$Label.stderr.txt"
    $probe = Start-Process -FilePath $codex -ArgumentList 'doctor --network-only --json' -WindowStyle Hidden -PassThru `
        -RedirectStandardError $stderr -RedirectStandardOutput $stdout
    if (!$probe.WaitForExit(40000)) {
        $probe.Kill()
        $probe.WaitForExit()
        throw "Codex network diagnostics timed out. See $stderr"
    }
    $report = Get-Content -LiteralPath $stdout -Raw | ConvertFrom-Json
    $http = $report.checks.'network.provider_reachability'.status
    $ws = $report.checks.'network.websocket_reachability'.status
    if (!$http -or !$ws) { throw "Codex network report is incomplete. See $stdout" }
    Write-Host "$Label returned HTTP=$http WebSocket=$ws"
    return (($http -eq 'ok') -eq $ExpectOnline -and ($ws -eq 'ok') -eq $ExpectOnline)
}

function Test-DnsRoute([string] $LogPath, [string] $Suffix, [bool] $ExpectSecondary) {
    $name = 'quickdns-' + [guid]::NewGuid().ToString('N') + '.' + $Suffix
    try {
        Resolve-DnsName -Name ($name + '.') -Type A -DnsOnly -NoHostsFile -QuickTimeout | Out-Null
    } catch {
        # A probe uses a fresh name. NXDOMAIN is a valid upstream response.
        if ($_.FullyQualifiedErrorId -notlike 'DNS_ERROR_RCODE_NAME_ERROR*') { throw }
    }
    $exchange = Select-String -LiteralPath $LogPath -Pattern ('\[(\d+) \d+ms\] dns: exchange ' + [regex]::Escape($name) + '\. IN A$') |
        Select-Object -Last 1
    if (!$exchange) { throw "The Windows DNS probe did not reach TUN. See $LogPath" }
    $id = $exchange.Matches[0].Groups[1].Value
    $secondary = Select-String -LiteralPath $LogPath -Pattern ("\[$id .*dns: match.*=> route\(secondary-dns\)") -Quiet
    if ($secondary -ne $ExpectSecondary) { throw "Wrong DNS route for $Suffix. See $LogPath" }
    Write-Host "DNS for $Suffix selected the expected adapter"
}

$failed = 0
$cases = if ($CheckSplitDns) {
    @{ phase = 'split-dns'; primary = $InternetAdapter; secondary = $SecondDnsAdapter }
} else {
    $baseline = Invoke-Probe $control 'baseline'
    if ($baseline.exitCode -ne 0 -or !$baseline.remote) { throw 'Baseline HTTPS failed.' }
    @(
        @{ phase = 'internet-secondary'; primary = $LocalAdapter; secondary = $InternetAdapter; target = $true; control = $false },
        @{ phase = 'internet-primary'; primary = $InternetAdapter; secondary = $LocalAdapter; target = $false; control = $true }
    )
}
if ($DisconnectAdapter) {
    $cases += @{ phase = 'disconnect-secondary'; primary = $InternetAdapter; secondary = $DisconnectAdapter; target = $true; control = $true }
}
if ($CheckCodex) {
    $codex = Get-Process -Name codex -ErrorAction SilentlyContinue |
        Where-Object { $_.Path -like "$env:LOCALAPPDATA\OpenAI\Codex\bin\*\codex.exe" } |
        Select-Object -First 1 -ExpandProperty Path
    $package = powershell.exe -NoProfile -Command '(Get-AppxPackage -Name OpenAI.Codex).InstallLocation'
    if (!$codex -or !$package) { throw 'Open the installed Codex desktop app before using -CheckCodex.' }
    if (!(Invoke-CodexProbe 'codex-baseline')) { throw 'Codex baseline connectivity failed.' }
    $cases += @{ phase = 'codex'; primary = $LocalAdapter; secondary = $InternetAdapter; target = $true; control = $false }
    if ($DisconnectAdapter) {
        $cases += @{ phase = 'codex-disconnect'; primary = $InternetAdapter; secondary = $DisconnectAdapter; target = $true; control = $true }
    }
}
foreach ($case in $cases) {
    $phase = $case.phase
    $config = Join-Path $work "$phase.json"
    $log = Join-Path $work "$phase.log"
    $isCodex = $phase -like 'codex*'
    if ($isCodex) {
        & $generator --write-live-config $case.primary $case.secondary $package $config $log 'OpenAI.Codex_2p2nqsd0c76g0'
    } elseif ($phase -eq 'split-dns') {
        & $generator --write-live-dns-config $case.primary $case.secondary $target $config $log
    } else {
        & $generator --write-live-config $case.primary $case.secondary $target $config $log
    }
    if ($LASTEXITCODE -ne 0) { throw 'Config generation failed.' }
    $sessionConfig = Get-Content -LiteralPath $config -Raw | ConvertFrom-Json
    $alias = $sessionConfig.inbounds[0].interface_name
    & $engine check -c $config
    if ($LASTEXITCODE -ne 0) { throw 'sing-box rejected the generated config.' }

    $process = Start-Process -FilePath $engine -ArgumentList ('run -c "{0}"' -f $config) -WindowStyle Hidden -PassThru
    try {
        $deadline = [Diagnostics.Stopwatch]::StartNew()
        while ($deadline.Elapsed.TotalSeconds -lt 12) {
            if ($process.HasExited) { throw "sing-box exited. See $log" }
            $tun = Get-ActiveTun $alias
            if ($tun -and (Test-Path -LiteralPath $log) -and (Select-String -LiteralPath $log -Pattern 'sing-box started' -Quiet)) {
                break
            }
            Start-Sleep -Milliseconds 250
        }
        if ($deadline.Elapsed.TotalSeconds -ge 12) { throw "TUN did not start. See $log" }
        if ($phase -eq 'split-dns') {
            Test-DnsRoute $log 'chatgpt.com' $true
            Test-DnsRoute $log $PrimaryDnsProbeDomain $false
            continue
        }
        $selected = if ($isCodex) {
            Invoke-CodexProbe "$phase-target"
        } else {
            (Invoke-Probe $target "$phase-target-ip" $baseline.remote).exitCode -eq 0
        }
        $ordinary = Invoke-Probe $control "$phase-control-ip" $baseline.remote
        $named = if ($case.target -and !$isCodex) { Invoke-Probe $target "$phase-target-name" }

        if ($selected -ne $case.target -or
            ($ordinary.exitCode -eq 0) -ne $case.control -or
            ($null -ne $named -and $named.exitCode -ne 0)) {
            $failed++
            continue
        }
        if ($phase -like '*disconnect*') {
            $adapter = Get-NetAdapter | Where-Object Name -eq $case.secondary
            if (!$adapter -or $adapter.Status -ne 'Up') { throw 'The secondary adapter is no longer up.' }
            try {
                $adapter | Disable-NetAdapter -Confirm:$false
                $blocked = if ($isCodex) {
                    Invoke-CodexProbe "$phase-target-offline" $false
                } else {
                    (Invoke-Probe $target "$phase-target-offline" $baseline.remote).exitCode -ne 0
                }
                $available = Invoke-Probe $control "$phase-control-online" $baseline.remote
                if (!$blocked -or $available.exitCode -ne 0 -or $process.HasExited) { $failed++ }
            } finally {
                $adapter | Enable-NetAdapter -Confirm:$false
            }
            $recovery = [Diagnostics.Stopwatch]::StartNew()
            do {
                $restored = if ($isCodex) {
                    Invoke-CodexProbe "$phase-target-restored"
                } else {
                    (Invoke-Probe $target "$phase-target-restored" $baseline.remote).exitCode -eq 0
                }
                if ($restored) { break }
                Start-Sleep -Seconds 1
            } while ($recovery.Elapsed.TotalSeconds -lt 30)
            if (!$restored) { $failed++ }
        }
    } finally {
        if (!$process.HasExited) {
            & $generator --stop-live-engine $process.Id
            if (!$process.WaitForExit(5000)) { $process.Kill() }
            $process.WaitForExit()
        }
        if (Get-ActiveTun $alias) {
            throw "TUN remained after sing-box stopped. See $log"
        }
    }
}

if (!$CheckSplitDns -and (Invoke-Probe $control 'after-stop' $baseline.remote).exitCode -ne 0) { $failed++ }
if ($failed) { throw "Routing checks failed. See $work" }
