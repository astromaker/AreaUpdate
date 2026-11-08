param(
    [Parameter(Mandatory)][ValidateSet('windows','queue','std','sleep')][string]$Stage,
    [ValidateRange(1,100)][int]$Repeats = 3,
    [switch]$BenchmarkOnly,
    [switch]$RuntimeOnly,
    [ValidateRange(1,1000000)][int]$RuntimeActors = 70000,
    [ValidatePattern('^[a-zA-Z0-9_-]+$')][string]$BatchTag = 'initial'
)
$repositoryRoot = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)

$prefix = if ($BatchTag -eq 'initial') { '' } else { "$BatchTag-" }
if ($RuntimeActors -ne 10000) { $prefix += "actors$RuntimeActors-" }

$ErrorActionPreference = 'Stop'
$directory = Join-Path $repositoryRoot "artifacts/std-port/$Stage"
$rows = @()
$cases = @(
    @{ Name = 'cluster-light'; Exe = 'UpdateComparison'; Args = @('job',5000,100,8,1000,8,42,'clustered',0,5,320) },
    @{ Name = 'cluster-interaction'; Exe = 'UpdateComparison'; Args = @('job',5000,100,32,1000,8,42,'clustered',0,5,320) },
    @{ Name = 'cluster-heavy'; Exe = 'UpdateComparison'; Args = @('job',5000,40,8,20000,8,42,'clustered',0,5,320) },
    @{ Name = 'hotspot'; Exe = 'UpdateComparison'; Args = @('job',2000,100,32,1000,8,42,'hotspot',0,5,320) },
    @{ Name = 'runtime-staggered'; Exe = 'AreaUpdateExample'; Args = @(10,$RuntimeActors,2000,42,5000) },
    @{ Name = 'runtime-bulk'; Exe = 'AreaUpdateExample'; Args = @(10,$RuntimeActors,2000,42,0) }
)

if ($BenchmarkOnly -and $RuntimeOnly) { throw 'Choose BenchmarkOnly or RuntimeOnly, not both' }
if ($BenchmarkOnly) { $cases = @($cases | Where-Object { $_.Exe -eq 'UpdateComparison' }) }
if ($RuntimeOnly) { $cases = @($cases | Where-Object { $_.Exe -eq 'AreaUpdateExample' }) }

foreach ($case in $cases) {
    foreach ($repeat in 1..$Repeats) {
        $log = Join-Path $directory "$prefix$($case.Name)-$repeat.log"
        $errorLog = Join-Path $directory "$prefix$($case.Name)-$repeat.stderr.log"
        $process = Start-Process -FilePath (Join-Path $directory "$($case.Exe).exe") -ArgumentList $case.Args -WindowStyle Hidden -PassThru -RedirectStandardOutput $log -RedirectStandardError $errorLog
        $watch = [Diagnostics.Stopwatch]::StartNew()
        while (-not $process.WaitForExit(1000)) {
            if ($watch.Elapsed.TotalSeconds -gt 180) {
                $process.Kill()
                throw "Timeout: $Stage $($case.Name)"
            }
        }
        $process.WaitForExit()
        if ($process.ExitCode -ne 0) { throw "Failed: $Stage $($case.Name), see $log" }
        $processCpuSeconds = $null
        try { $processCpuSeconds = $process.TotalProcessorTime.TotalSeconds } catch { }
        $values = @{}
        foreach ($line in Get-Content -LiteralPath $log) {
            if ($line -match '^(mode=|SUMMARY |RUNTIME )') {
                foreach ($match in [regex]::Matches($line, '(\w+)=([^ ]+)')) {
                    $values[$match.Groups[1].Value] = $match.Groups[2].Value
                }
            }
        }
        if ([double]$values['seconds'] -le 0) { throw "Missing duration: $log" }
        if ($case.Exe -eq 'UpdateComparison') {
            if ($values['violations'] -ne '0') { throw "Validation failed: $log" }
        }
        else {
            foreach ($key in @('overlaps','never_updated','tasks_live','tasks_pending')) {
                if ($values[$key] -ne '0') { throw "Runtime validation failed: $key, $log" }
            }
            if ($values['actors'] -ne [string]$RuntimeActors) { throw "Actor count mismatch: $log" }
        }
        $row = [ordered]@{ stage=$Stage; scenario=$case.Name; repeat=$repeat; batch=$BatchTag }
        $row['process_cpu_seconds'] = $processCpuSeconds
        foreach ($key in @('seconds','updates_per_sec','p99_tick_ms','cpu_seconds','violations','checksum',
                'actors','moves','updates','actor_fps','overlaps','never_updated','max_gap_ms','p99_gap_bucket_ms',
                'target_actors','join_duration_ms','join_interval_ms','join_steps','max_join_batch',
                'max_queue_delay_ms','max_timer_delay_ms','busy_ms','idle_ms','tasks_live','tasks_pending')) {
            $row[$key] = $values[$key]
        }
        $rows += [pscustomobject]$row
        $rows | Export-Csv -LiteralPath (Join-Path $directory "${prefix}runs.csv") -NoTypeInformation
        Write-Output "$Stage $($case.Name) $repeat completed"
    }
}
