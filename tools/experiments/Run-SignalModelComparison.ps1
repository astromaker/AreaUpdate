param(
    [ValidateRange(15,120)][int]$Seconds = 20,
    [ValidateRange(1,5)][int]$Repetitions = 3,
    [ValidateRange(1,1000000)][int[]]$ActorCounts = @(10000,70000,140000,280000),
    [string]$ExecutablePath = 'artifacts/signal-default-comparison/after/AreaUpdateExample.exe',
    [string]$OutputDirectory = 'artifacts/signal-default-comparison/measurements'
)
$repositoryRoot = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)


$ErrorActionPreference = 'Stop'
$root = Join-Path $repositoryRoot $OutputDirectory
if (Test-Path $root) { throw 'Preserve existing signal comparison results' }
$exe = Join-Path $repositoryRoot $ExecutablePath
if (-not (Test-Path $exe)) { throw 'Missing frozen signal executable' }
New-Item -ItemType Directory -Path $root | Out-Null
[ordered]@{
    recorded = Get-Date -Format o
    executableSha256 = (Get-FileHash $exe).Hash
    seconds = $Seconds
    repetitions = $Repetitions
    actorCounts = $ActorCounts
    logicalProcessors = [Environment]::ProcessorCount
    waitMode = 'signal'
    taskStatisticsEnabled = $true
    areaAndObjectPolicy = 'fixed'
    timerSlotMs = 5
    timerSlots = 256
    allOptionalProfiles = $false
    joinDurationMs = 5000
    joinIntervalMs = 1
    actorWork = 0
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
    $plans += [pscustomobject]@{ actors=$actors; mapSide=$side; movesPerSecond=$moves; estimatedMB=$estimate/1MB; budgetMB=$headroom*0.7/1MB; allowed=$allowed }
    $plans | Export-Csv (Join-Path $root 'plan.csv') -NoTypeInformation
    if (-not $allowed) {
        Write-Warning "Stopped before $actors Actors: memory safety budget"
        break
    }

    foreach ($round in 1..$Repetitions) {
        $modes = if ($round % 2 -eq 1) { @('area','job') } else { @('job','area') }
        foreach ($mode in $modes) {
            $relative = "$OutputDirectory/actors-$actors/$mode-$round"
            # Omit WaitMode deliberately to exercise Measure-RuntimeCpu's signal default.
            & (Join-Path $repositoryRoot 'tools/benchmark/Measure-RuntimeCpu.ps1') -Seconds $Seconds -Actors $actors -MovesPerSecond $moves -MapSide $side -Mode $mode -ExecutablePath $ExecutablePath -OutputDirectory $relative | Out-Null
            $result = Get-Content (Join-Path $repositoryRoot "$relative/summary.json") -Raw | ConvertFrom-Json
            $policy = if ($mode -eq 'area') { $result.areaSchedule } else { $result.objectSchedule }
            if ($result.waitMode -ne 'signal' -or $policy -ne 'fixed' -or $result.taskStatisticsEnabled -ne $true -or $result.timerSlotMs -ne 5 -or $result.timerSlots -ne 256) { throw 'Signal comparison configuration mismatch' }
            if ($result.signalWaits -eq 0 -or $result.traceThresholdMs -ne 0 -or $result.objectSampleStride -ne 0 -or $result.dispatcherProfileStride -ne 0) { throw 'Signal path inactive or profiling unexpectedly enabled' }
            if ($null -eq $result.steadyCpuPercent) { throw 'No steady-state CPU samples' }
            $result | Add-Member -NotePropertyName round -NotePropertyValue $round
            $results += $result
            $results | Export-Csv (Join-Path $root 'results.csv') -NoTypeInformation
            Write-Output ("actors={0} mode={1} round={2} FPS={3:F3} CPU={4:F2}%" -f $actors,$mode,$round,$result.actorFps,$result.steadyCpuPercent)
        }
    }
}
