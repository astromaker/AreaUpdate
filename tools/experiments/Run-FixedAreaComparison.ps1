param(
    [ValidateRange(15,120)][int]$Seconds = 20,
    [ValidateRange(1,5)][int]$Repetitions = 2,
    [ValidateRange(1,1000000)][int[]]$ActorCounts = @(10000,70000,140000,280000),
    [string]$BinaryDirectory = 'artifacts/area-fixed-comparison',
    [string]$OutputDirectory = 'artifacts/area-fixed-comparison/measurements'
)
$repositoryRoot = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)


$ErrorActionPreference = 'Stop'
$root = Join-Path $repositoryRoot $OutputDirectory
if (Test-Path $root) { throw 'Preserve existing fixed Area comparison' }
$cases = @(
    [pscustomobject]@{ name='relative-area'; build='before'; mode='area'; policy='relative' },
    [pscustomobject]@{ name='fixed-area'; build='after'; mode='area'; policy='fixed' },
    [pscustomobject]@{ name='object'; build='after'; mode='job'; policy='fixed' }
)
$binaries = @()
foreach ($build in @('before','after')) {
    $path = Join-Path $repositoryRoot "$BinaryDirectory/$build/AreaUpdateExample.exe"
    if (-not (Test-Path $path)) { throw "Missing frozen executable: $build" }
    $binaries += [pscustomobject]@{ build=$build; sha256=(Get-FileHash $path).Hash }
}
New-Item -ItemType Directory -Path $root | Out-Null
$binaries | ConvertTo-Json | Set-Content (Join-Path $root 'binaries.json') -Encoding utf8
[ordered]@{
    recorded = Get-Date -Format o
    seconds = $Seconds
    repetitions = $Repetitions
    actorCounts = $ActorCounts
    logicalProcessors = [Environment]::ProcessorCount
    taskStatisticsEnabled = $true
    waitMode = 'poll'
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
        $order = @($cases)
        if ($round % 2 -eq 0) { [Array]::Reverse($order) }
        foreach ($case in $order) {
            $relative = "$OutputDirectory/actors-$actors/$($case.name)-$round"
            & (Join-Path $repositoryRoot 'tools/benchmark/Measure-RuntimeCpu.ps1') -Seconds $Seconds -Actors $actors -MovesPerSecond $moves -MapSide $side -Mode $case.mode -WaitMode poll -TimerTraceThresholdMs 0 -ObjectSampleStride 0 -DispatcherProfileStride 0 -ExecutablePath "$BinaryDirectory/$($case.build)/AreaUpdateExample.exe" -OutputDirectory $relative | Out-Null
            $result = Get-Content (Join-Path $repositoryRoot "$relative/summary.json") -Raw | ConvertFrom-Json
            $policy = if ($case.mode -eq 'area') { $result.areaSchedule } else { $result.objectSchedule }
            if ($policy -ne $case.policy -or $result.taskStatisticsEnabled -ne $true -or $result.timerSlotMs -ne 5 -or $result.timerSlots -ne 256 -or $result.waitMode -ne 'poll') { throw 'Scheduling configuration mismatch' }
            if ($result.traceThresholdMs -ne 0 -or $result.objectSampleStride -ne 0 -or $result.dispatcherProfileStride -ne 0) { throw 'Unexpected profiling enabled' }
            if ($null -eq $result.steadyCpuPercent) { throw 'No steady-state CPU samples' }
            $result | Add-Member -NotePropertyName variant -NotePropertyValue $case.name
            $result | Add-Member -NotePropertyName round -NotePropertyValue $round
            $results += $result
            $results | Export-Csv (Join-Path $root 'results.csv') -NoTypeInformation
            Write-Output ("actors={0} variant={1} round={2} FPS={3:F3} CPU={4:F2}%" -f $actors,$case.name,$round,$result.actorFps,$result.steadyCpuPercent)
        }
    }
}
