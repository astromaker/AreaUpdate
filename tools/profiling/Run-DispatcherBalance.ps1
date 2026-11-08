param(
    [ValidateRange(15,120)][int]$Seconds = 20,
    [ValidateRange(1,5)][int]$Repetitions = 2,
    [ValidateRange(1,1000000)][int[]]$ActorCounts = @(70000,140000),
    [ValidateRange(1,1000000)][int]$ProfileStride = 256,
    [string]$ExecutablePath = 'artifacts/dispatcher-balance/isolated/AreaUpdateExample.exe',
    [string]$OutputDirectory = 'artifacts/dispatcher-balance/measurements-isolated'
)
$repositoryRoot = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)


$ErrorActionPreference = 'Stop'
$root = Join-Path $repositoryRoot $OutputDirectory
if (Test-Path $root) { throw 'Preserve existing Dispatcher measurements' }
$exe = Join-Path $repositoryRoot $ExecutablePath
if (-not (Test-Path $exe)) { throw 'Missing frozen Dispatcher executable' }
New-Item -ItemType Directory -Path $root | Out-Null
[ordered]@{
    recorded = Get-Date -Format o
    executableSha256 = (Get-FileHash $exe).Hash
    seconds = $Seconds
    repetitions = $Repetitions
    actorCounts = $ActorCounts
    logicalProcessors = [Environment]::ProcessorCount
    mode = 'job'
    waitMode = 'poll'
    dispatcherProfileStride = $ProfileStride
    objectSampleStride = 0
    timerTraceThresholdMs = 0
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
    $allowed = $estimate -le $headroom * 0.7 -and $side -le 2048
    $plans += [pscustomobject]@{ actors=$actors; mapSide=$side; estimatedMB=$estimate/1MB; budgetMB=$headroom*0.7/1MB; allowed=$allowed }
    $plans | Export-Csv (Join-Path $root 'plan.csv') -NoTypeInformation
    if (-not $allowed) {
        Write-Warning "Stopped before $actors Actors: memory safety budget"
        break
    }

    foreach ($round in 1..$Repetitions) {
        $strides = if ($round % 2 -eq 1) { @(0,$ProfileStride) } else { @($ProfileStride,0) }
        foreach ($stride in $strides) {
            $relative = "$OutputDirectory/actors-$actors/stride-$stride-$round"
            & (Join-Path $repositoryRoot 'tools/benchmark/Measure-RuntimeCpu.ps1') -Seconds $Seconds -Actors $actors -MovesPerSecond $moves -MapSide $side -Mode job -WaitMode poll -TimerTraceThresholdMs 0 -ObjectSampleStride 0 -DispatcherProfileStride $stride -ExecutablePath $ExecutablePath -OutputDirectory $relative | Out-Null
            $result = Get-Content (Join-Path $repositoryRoot "$relative/summary.json") -Raw | ConvertFrom-Json
            if ($result.taskStatisticsEnabled -ne $true -or $result.timerSlotMs -ne 5 -or $result.timerSlots -ne 256 -or $result.objectSchedule -ne 'fixed') { throw 'Scheduling configuration mismatch' }
            if ($null -eq $result.steadyCpuPercent) { throw 'No steady-state CPU samples' }
            if ($stride -gt 0) {
                $workers = Import-Csv (Join-Path $repositoryRoot "$relative/dispatcher-workers.csv")
                if ($workers.Count -ne 8 -or @($workers | Where-Object { [double]$_.samples -le 0 }).Count -ne 0) { throw 'Dispatcher sampling missed a Worker' }
                $counts = $workers | ForEach-Object { [double]$_.dispatch_tasks } | Measure-Object -Minimum -Maximum
                if ($counts.Maximum - $counts.Minimum -gt 1) { throw 'Round-robin distribution changed' }
                foreach ($worker in $workers) {
                    if ([double]$worker.busy_ms + [double]$worker.idle_ms -gt [double]$worker.elapsed_ms + 0.01) { throw 'Invalid Worker time accounting' }
                }
            }
            elseif ($result.dispatcherProfileAvailable -or $result.dispatcherProfileSamples -ne 0) { throw 'Disabled Dispatcher profile recorded samples' }
            $result | Add-Member -NotePropertyName round -NotePropertyValue $round
            $results += $result
            $results | Export-Csv (Join-Path $root 'results.csv') -NoTypeInformation
            Write-Output ("actors={0} stride={1} round={2} FPS={3:F3} CPU={4:F2}% busy_max/mean={5} queue_mean_max={6}ms worker_p99_max={7}ms" -f $actors,$stride,$round,$result.actorFps,$result.steadyCpuPercent,$result.dispatcherMaxToMeanBusy,$result.dispatcherMaxQueueMeanMs,$result.dispatcherMaxWorkerQueueP99Ms)
        }
    }
}
