param(
    [ValidateRange(15,120)][int]$Seconds = 20,
    [ValidateRange(1,5)][int]$Repetitions = 2,
    [ValidateRange(1,1000000)][int[]]$ActorCounts = @(70000,140000),
    [string]$ExecutablePath = 'artifacts/object-actor-task/profiled/AreaUpdateExample.exe',
    [string]$OutputDirectory = 'artifacts/object-actor-task/profiling'
)
$repositoryRoot = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)


$ErrorActionPreference = 'Stop'
$root = Join-Path $repositoryRoot $OutputDirectory
if (Test-Path $root) { throw 'Preserve existing profile results' }
$exe = Join-Path $repositoryRoot $ExecutablePath
if (-not (Test-Path $exe)) { throw 'Missing frozen profile executable' }
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
    sampleStrides = @(0,256)
    timerTraceThresholdMs = 0
} | ConvertTo-Json | Set-Content (Join-Path $root 'environment.json') -Encoding utf8

$results = @()
foreach ($actors in $ActorCounts | Sort-Object -Unique) {
    $side = [int][math]::Ceiling(320 * [math]::Sqrt($actors/70000.0))
    $moves = [int][math]::Ceiling($actors * 2000.0 / 70000)
    [GC]::Collect()
    $memory = [GC]::GetGCMemoryInfo()
    $headroom = [math]::Max([int64]0, $memory.TotalAvailableMemoryBytes - $memory.MemoryLoadBytes)
    $estimate = [double]$actors * 1600 + [double]$side * $side * 600 + 64MB
    if ($estimate -gt $headroom * 0.7 -or $side -gt 2048) { throw "Memory safety budget exceeded: $actors" }

    foreach ($round in 1..$Repetitions) {
        $strides = if ($round % 2 -eq 1) { @(0,256) } else { @(256,0) }
        foreach ($stride in $strides) {
            $relative = "$OutputDirectory/actors-$actors/stride-$stride-$round"
            & (Join-Path $repositoryRoot 'tools/benchmark/Measure-RuntimeCpu.ps1') -Seconds $Seconds -Actors $actors -MovesPerSecond $moves -MapSide $side -Mode job -WaitMode poll -TimerTraceThresholdMs 0 -ObjectSampleStride $stride -ExecutablePath $ExecutablePath -OutputDirectory $relative | Out-Null
            $result = Get-Content (Join-Path $repositoryRoot "$relative/summary.json") -Raw | ConvertFrom-Json
            if ($result.taskStatisticsEnabled -ne $true -or $result.timerSlotMs -ne 5 -or $result.timerSlots -ne 256 -or $result.objectSchedule -ne 'fixed') { throw 'Profile scheduling mismatch' }
            if ($stride -gt 0 -and ($result.objectSamples -eq 0 -or $result.sampledActors -eq 0)) { throw 'No object samples' }
            if ($stride -eq 0 -and $result.objectSamples -ne 0) { throw 'Disabled profiler recorded samples' }
            $workers = @()
            foreach ($line in Get-Content (Join-Path $repositoryRoot "$relative/runtime.log")) {
                if ($line -match '^OBJECT_SAMPLE_WORKER timer_worker=(\d+) samples=(\d+)$') {
                    $workers += [pscustomobject]@{ worker=[int]$Matches[1]; samples=[uint64]$Matches[2] }
                }
            }
            if ($workers.Count -ne 8 -or ($workers | Measure-Object samples -Sum).Sum -ne $result.objectSamples) { throw 'Worker sample aggregation mismatch' }
            if ($stride -gt 0 -and $actors -ge 70000 -and @($workers | Where-Object samples -eq 0).Count -ne 0) { throw 'Sampling missed a timer Worker' }
            $workers | Export-Csv (Join-Path $repositoryRoot "$relative/sample-workers.csv") -NoTypeInformation
            $result | Add-Member -NotePropertyName round -NotePropertyValue $round
            $results += $result
            $results | Export-Csv (Join-Path $root 'results.csv') -NoTypeInformation
            Write-Output ("actors={0} sample_stride={1} round={2} FPS={3:F3} CPU={4:F2}% queue_mean={5}us queue_p99={6}ms timer_mean={7}ms actor_mean={8}us" -f $actors,$stride,$round,$result.actorFps,$result.steadyCpuPercent,$result.objectQueueMeanUs,$result.objectQueueP99Ms,$result.objectTimerMeanMs,$result.objectActorMeanUs)
        }
    }
}
