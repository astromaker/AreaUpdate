param([ValidateRange(1,10)][int]$Repeats = 3, [int]$WarmupTicks = 5, [string]$Only = '', [switch]$SummarizeOnly, [switch]$WithOriginal, [switch]$Resume)
$repositoryRoot = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)

$ErrorActionPreference = 'Stop'
$root = $repositoryRoot
$exe = Join-Path $root 'Source/x64/Release/UpdateComparison.exe'
$output = Join-Path $root 'artifacts/scenarios-optimized'
if ($WithOriginal) { $output = Join-Path $root 'artifacts/scenarios-three-way' }
New-Item -ItemType Directory -Path $output -Force | Out-Null
$culture = [Globalization.CultureInfo]::InvariantCulture
$scenarios = @(
    @{ Id='S01'; Layout='uniform'; Actors=5000; Interactions=0; Work=1000; Workers=8; Period=0 },
    @{ Id='S02'; Layout='uniform'; Actors=5000; Interactions=8; Work=1000; Workers=8; Period=0 },
    @{ Id='S03'; Layout='clustered'; Actors=5000; Interactions=0; Work=1000; Workers=8; Period=0 },
    @{ Id='S04'; Layout='clustered'; Actors=5000; Interactions=8; Work=1000; Workers=8; Period=0 },
    @{ Id='S05'; Layout='clustered'; Actors=5000; Interactions=32; Work=1000; Workers=8; Period=0 },
    @{ Id='S06'; Layout='hotspot'; Actors=2000; Interactions=32; Work=1000; Workers=8; Period=0 },
    @{ Id='S07'; Layout='clustered'; Actors=5000; Interactions=8; Work=20000; Workers=1; Period=0 },
    @{ Id='S08'; Layout='clustered'; Actors=5000; Interactions=8; Work=20000; Workers=2; Period=0 },
    @{ Id='S09'; Layout='clustered'; Actors=5000; Interactions=8; Work=20000; Workers=4; Period=0 },
    @{ Id='S10'; Layout='clustered'; Actors=5000; Interactions=8; Work=20000; Workers=8; Period=0 },
    @{ Id='S11'; Layout='hotspot'; Actors=2000; Interactions=8; Work=20000; Workers=8; Period=0 },
    @{ Id='S12'; Layout='clustered'; Actors=10000; Interactions=8; Work=1000; Workers=8; Period=0 },
    @{ Id='S13'; Layout='clustered'; Actors=5000; Interactions=8; Work=1000; Workers=8; Period=50 },
    @{ Id='S14'; Layout='uniform'; Actors=20000; Interactions=8; Work=1000; Workers=8; Period=0; MapSide=640 },
    @{ Id='S15'; Layout='uniform'; Actors=20000; Interactions=8; Work=20000; Workers=8; Period=0; MapSide=640 }
)
if ($Only) { $scenarios = @($scenarios | Where-Object { $_.Id -in $Only.Split(',') }) }
if (-not $scenarios.Count) { throw 'No matching scenario' }

$metadata = [ordered]@{
    ExeSHA256 = (Get-FileHash -LiteralPath $exe -Algorithm SHA256).Hash
    OS = [Environment]::OSVersion.VersionString
    LogicalProcessors = [Environment]::ProcessorCount
    Repeats = $Repeats; WarmupTicks = $WarmupTicks; Ticks = 100; Seed = 42
    OriginalCommit = 'b713018'; WithOriginal = [bool]$WithOriginal
    ScenarioMaps = @($scenarios | ForEach-Object { @{ Id=$_.Id; MapSide=$(if ($_.MapSide) { $_.MapSide } else { 320 }) } })
}
$rows = @()
if ($SummarizeOnly -or $Resume) { $rows = @(Import-Csv -LiteralPath (Join-Path $output 'runs.csv')) }
else { $metadata | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $output 'environment.json') -Encoding utf8 }
if ($Resume) {
    $saved = Get-Content -LiteralPath (Join-Path $output 'environment.json') -Raw | ConvertFrom-Json
    if ($saved.ExeSHA256 -ne $metadata.ExeSHA256 -or $saved.Repeats -ne $Repeats -or $saved.WarmupTicks -ne $WarmupTicks) {
        throw 'Resume configuration or binary mismatch'
    }
}
foreach ($scenario in $scenarios) {
    if ($SummarizeOnly) { break }
    foreach ($repeat in 1..$Repeats) {
        $order = if ($repeat % 2) { @('job','area') } else { @('area','job') }
        if ($WithOriginal) {
            $modes = @('original','area','job')
            $order = @(0..2 | ForEach-Object { $modes[($_ + $repeat - 1) % 3] })
        }
        if ($Resume) {
            $completed = @($rows | Where-Object { $_.Scenario -eq $scenario.Id -and [int]$_.Repeat -eq $repeat })
            if ($completed.Count -eq $order.Count) { continue }
            if ($completed.Count) { throw 'Partial repeat in saved CSV' }
        }
        $pair = @()
        foreach ($mode in $order) {
            $name = "$($scenario.Id)-$repeat-$mode"
            $log = Join-Path $output "$name.log"
            $errors = Join-Path $output "$name.stderr.log"
            $arguments = @($mode, $scenario.Actors, 100, $scenario.Interactions, $scenario.Work, $scenario.Workers, 42, $scenario.Layout, $scenario.Period, $WarmupTicks)
            if ($scenario.MapSide) { $arguments += $scenario.MapSide }
            $timer = [Diagnostics.Stopwatch]::StartNew()
            $process = Start-Process -FilePath $exe -ArgumentList $arguments -WindowStyle Hidden -PassThru -RedirectStandardOutput $log -RedirectStandardError $errors
            $peakPrivate = $null
            $nextProgress = 30
            while (-not $process.WaitForExit(250)) {
                $process.Refresh()
                if ($process.HasExited) { break }
                $current = $process.PrivateMemorySize64 / 1MB
                if ($null -eq $peakPrivate -or $current -gt $peakPrivate) { $peakPrivate = $current }
                if ($timer.Elapsed.TotalSeconds -ge $nextProgress) {
                    Write-Output "$name running $([int]$timer.Elapsed.TotalSeconds)s"
                    $nextProgress += 30
                }
                if ($timer.Elapsed.TotalSeconds -gt 600) { $process.Kill(); throw "Watchdog timeout: $name" }
            }
            $process.WaitForExit()
            if ($process.ExitCode -ne 0) { throw "Failed $name with exit $($process.ExitCode): $errors" }
            $line = Get-Content -LiteralPath $log | Where-Object { $_ -like 'mode=*' } | Select-Object -Last 1
            if (-not $line) { throw "Missing result: $name" }
            $values = @{}
            foreach ($match in [regex]::Matches($line, '(\w+)=([^ ]+)')) { $values[$match.Groups[1].Value] = $match.Groups[2].Value }
            if ($values.mode -ne $mode -or $values.violations -ne '0') { throw "Invalid result: $name" }
            $pair += [pscustomobject][ordered]@{
                Scenario=$scenario.Id; Repeat=$repeat; Order=($order -join ','); Mode=$mode
                Actors=$scenario.Actors; Ticks=100; Interactions=$scenario.Interactions; Work=$scenario.Work
                Workers=$scenario.Workers; Layout=$scenario.Layout; PeriodMs=$scenario.Period
                MapSide=$(if ($scenario.MapSide) { $scenario.MapSide } else { 320 })
                Seconds=[double]::Parse($values.seconds,$culture)
                UpdatesPerSecond=[double]::Parse($values.updates_per_sec,$culture)
                P99TickMs=[double]::Parse($values.p99_tick_ms,$culture)
                MissedTicks=[int]$values.missed_ticks
                CpuSeconds=[double]::Parse($values.cpu_seconds,$culture)
                CpuOneCorePercent=[double]::Parse($values.cpu_one_core_percent,$culture)
                PrivatePeakMB=$peakPrivate; ProcessSeconds=$timer.Elapsed.TotalSeconds
                Events=$values.events; Checksum=$values.checksum; Violations=$values.violations
            }
        }
        foreach ($candidate in $pair) {
            if ($pair[0].Checksum -ne $candidate.Checksum -or $pair[0].Events -ne $candidate.Events) { throw "State mismatch: $($scenario.Id) repeat $repeat" }
        }
        $rows += $pair
        $rows | Export-Csv -LiteralPath (Join-Path $output 'runs.csv') -NoTypeInformation
        Write-Output "$($scenario.Id) repeat=$repeat verified $($order -join '/')"
    }
}

function Median([double[]]$Values) {
    $sorted = @($Values | Sort-Object)
    if (!$sorted.Count) { return $null }
    if ($sorted.Count % 2) { return $sorted[[int][math]::Floor($sorted.Count / 2)] }
    return ($sorted[$sorted.Count / 2 - 1] + $sorted[$sorted.Count / 2]) / 2
}
function MemoryMedian([object[]]$Values) {
    $measured = @($Values | Where-Object { -not [string]::IsNullOrWhiteSpace([string]$_) })
    return Median $measured
}
$summary = foreach ($scenario in $scenarios) {
    $job = @($rows | Where-Object { $_.Scenario -eq $scenario.Id -and $_.Mode -eq 'job' })
    $area = @($rows | Where-Object { $_.Scenario -eq $scenario.Id -and $_.Mode -eq 'area' })
    $original = @($rows | Where-Object { $_.Scenario -eq $scenario.Id -and $_.Mode -eq 'original' })
    if ($WithOriginal -and $original.Count -ne $Repeats) { throw "Incomplete original scenario: $($scenario.Id)" }
    if ($job.Count -ne $Repeats -or $area.Count -ne $Repeats) { throw "Incomplete scenario: $($scenario.Id)" }
    $jobSeconds = Median $job.Seconds
    $areaSeconds = Median $area.Seconds
    [pscustomobject][ordered]@{
        Scenario=$scenario.Id; Actors=$scenario.Actors; Workers=$scenario.Workers; Layout=$scenario.Layout
        MapSide=$(if ($scenario.MapSide) { $scenario.MapSide } else { 320 })
        Interactions=$scenario.Interactions; Work=$scenario.Work; PeriodMs=$scenario.Period
        JobSeconds=$jobSeconds; AreaSeconds=$areaSeconds
        OriginalSeconds=$(if ($WithOriginal) { Median $original.Seconds } else { $null })
        AreaSpeedup=$(if ($scenario.Period -eq 0) { $jobSeconds / $areaSeconds } else { $null })
        JobP99Ms=(Median $job.P99TickMs); AreaP99Ms=(Median $area.P99TickMs)
        OriginalP99Ms=$(if ($WithOriginal) { Median $original.P99TickMs } else { $null })
        JobCpuSeconds=(Median $job.CpuSeconds); AreaCpuSeconds=(Median $area.CpuSeconds)
        JobMissedTicks=(Median $job.MissedTicks); AreaMissedTicks=(Median $area.MissedTicks)
        JobPrivatePeakMB=(MemoryMedian $job.PrivatePeakMB)
        AreaPrivatePeakMB=(MemoryMedian $area.PrivatePeakMB)
    }
}
$summary | Export-Csv -LiteralPath (Join-Path $output 'summary.csv') -NoTypeInformation
$summary | Format-Table Scenario, OriginalSeconds, JobSeconds, AreaSeconds, AreaSpeedup, JobP99Ms, AreaP99Ms -AutoSize
