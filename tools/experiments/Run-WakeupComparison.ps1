param(
    [ValidateRange(15,120)][int]$Seconds = 20,
    [ValidateRange(1,5)][int]$Repetitions = 2,
    [ValidateRange(1,1000000)][int[]]$ActorCounts = @(10000,70000,140000,280000,560000),
    [ValidateRange(0,60000)][int]$TimerTraceThresholdMs = 50,
    [string]$OutputDirectory = 'artifacts/wakeup-comparison'
)
$repositoryRoot = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)


$ErrorActionPreference = 'Stop'
$root = Join-Path $repositoryRoot $OutputDirectory
if (Test-Path $root) { throw 'Choose a new output directory to preserve previous measurements' }
New-Item -ItemType Directory -Path $root | Out-Null
$archive = Join-Path $root 'AreaUpdateExample.exe'
Copy-Item (Join-Path $repositoryRoot 'Source/x64/Release/AreaUpdateExample.exe') $archive
foreach ($source in @('Source/Core/Worker.h','Source/Core/Worker.cpp','Source/Core/WakeSignal.h','Source/Core/WheelTimer.h')) {
    Copy-Item (Join-Path $repositoryRoot $source) (Join-Path $root ([IO.Path]::GetFileName($source)))
}
[ordered]@{
    recorded = Get-Date -Format o
    executableSha256 = (Get-FileHash $archive).Hash
    logicalProcessors = [Environment]::ProcessorCount
    seconds = $Seconds
    repetitions = $Repetitions
    actorCounts = $ActorCounts
    traceThresholdMs = $TimerTraceThresholdMs
    joinDurationMs = 5000
    joinIntervalMs = 1
    actorWork = 0
    mode = 'job'
} | ConvertTo-Json | Set-Content (Join-Path $root 'environment.json') -Encoding utf8

$results = @()
$plans = @()
foreach ($actors in $ActorCounts | Sort-Object -Unique) {
    $side = [int][math]::Ceiling(320 * [math]::Sqrt($actors/70000.0))
    $moves = [int][math]::Ceiling($actors * 2000.0 / 70000)
    [GC]::Collect()
    $memory = [GC]::GetGCMemoryInfo()
    $headroom = [math]::Max([int64]0, $memory.TotalAvailableMemoryBytes - $memory.MemoryLoadBytes)
    $estimate = [double]$actors * 1600 + [double]$side * $side * 600 + 64MB
    $allowed = $side -le 2048 -and $estimate -le $headroom * 0.7
    $plans += [pscustomobject]@{ actors=$actors; mapSide=$side; estimatedMB=$estimate/1MB; budgetMB=$headroom*0.7/1MB; allowed=$allowed }
    $plans | Export-Csv (Join-Path $root 'plan.csv') -NoTypeInformation
    if (-not $allowed) {
        Write-Warning "Stopped before $actors Actors: memory safety budget exceeded"
        break
    }

    foreach ($round in 1..$Repetitions) {
        $variants = if ($round % 2 -eq 1) { @('poll','signal') } else { @('signal','poll') }
        foreach ($variant in $variants) {
            $relative = "$OutputDirectory/actors-$actors/$variant-$round"
            & (Join-Path $repositoryRoot 'tools/benchmark/Measure-RuntimeCpu.ps1') -Seconds $Seconds -Actors $actors -MovesPerSecond $moves -MapSide $side -Mode job -TimerTraceThresholdMs $TimerTraceThresholdMs -WaitMode $variant -ExecutablePath "$OutputDirectory/AreaUpdateExample.exe" -OutputDirectory $relative | Out-Null
            $result = Get-Content (Join-Path $repositoryRoot "$relative/summary.json") -Raw | ConvertFrom-Json
            if ($result.waitMode -ne $variant -or $result.timerSlotMs -ne 5 -or $result.timerSlots -ne 256 -or $result.objectSchedule -ne 'fixed') {
                throw 'Scheduling configuration mismatch'
            }
            if ($variant -eq 'signal' -and ($result.signalWaits -eq 0 -or $result.notifiedWaits -eq 0)) {
                throw 'Signal waiting was not exercised'
            }
            if ($variant -eq 'poll' -and $result.signalWaits -ne 0) { throw 'Polling baseline used signal waiting' }
            if ($null -eq $result.steadyCpuPercent) { throw 'No steady-state CPU samples' }
            if ($TimerTraceThresholdMs -gt 0 -and $result.maxTimerDelayMs -ge $TimerTraceThresholdMs) {
                $traces = Import-Csv (Join-Path $repositoryRoot "$relative/timer-traces.csv")
                if (($traces | Measure-Object delay_ms -Maximum).Maximum -ne $result.maxTimerDelayMs) {
                    throw 'Largest timer delay was not retained'
                }
            }
            $workers = @()
            foreach ($line in Get-Content (Join-Path $repositoryRoot "$relative/runtime.log")) {
                if ($line -notmatch '^WORKER ') { continue }
                $row = [ordered]@{}
                foreach ($match in [regex]::Matches($line, '(\w+)=([^ ]+)')) {
                    $row[$match.Groups[1].Value] = [double]$match.Groups[2].Value
                }
                $workers += [pscustomobject]$row
            }
            $workers | Export-Csv (Join-Path $repositoryRoot "$relative/workers.csv") -NoTypeInformation
            $busyMean = ($workers | Measure-Object busy_ms -Average).Average
            $busyMax = ($workers | Measure-Object busy_ms -Maximum).Maximum
            $result | Add-Member -NotePropertyName workerMaxToMeanBusy -NotePropertyValue $(if ($busyMean -gt 0) { $busyMax / $busyMean } else { 0 })
            $result | Add-Member -NotePropertyName round -NotePropertyValue $round
            $results += $result
            $results | Export-Csv (Join-Path $root 'results.csv') -NoTypeInformation
            Write-Output ("actors={0} wait={1} round={2} FPS={3:F3} CPU={4:F2}% p99={5}ms max={6}ms" -f $actors,$variant,$round,$result.actorFps,$result.steadyCpuPercent,$result.timerP99Ms,$result.maxTimerDelayMs)
        }
    }
}
