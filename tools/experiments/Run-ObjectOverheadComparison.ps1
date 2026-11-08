param(
    [ValidateRange(15,120)][int]$Seconds = 20,
    [ValidateRange(1,5)][int]$Repetitions = 2,
    [ValidateRange(1,1000000)][int[]]$ActorCounts = @(70000,140000),
    [ValidateNotNullOrEmpty()][string[]]$Variants = @('baseline-on','baseline-off','wrapper-on','final-on','final-off'),
    [string]$BinaryDirectory = '',
    [string]$OutputDirectory = 'artifacts/object-overhead'
)
$repositoryRoot = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)


$ErrorActionPreference = 'Stop'
$root = Join-Path $repositoryRoot $OutputDirectory
if (Test-Path (Join-Path $root 'results.csv')) { throw 'Preserve existing comparison results' }
if ([string]::IsNullOrWhiteSpace($BinaryDirectory)) { $BinaryDirectory = $OutputDirectory }
$binaryRoot = Join-Path $repositoryRoot $BinaryDirectory
$binaries = @()
foreach ($variant in $variants) {
    $path = Join-Path $binaryRoot "$variant/AreaUpdateExample.exe"
    if (-not (Test-Path $path)) { throw "Missing comparison executable: $variant" }
    $binaries += [pscustomobject]@{ variant=$variant; sha256=(Get-FileHash $path).Hash }
}
New-Item -ItemType Directory -Path $root -Force | Out-Null
$binaries | ConvertTo-Json | Set-Content (Join-Path $root 'binaries.json') -Encoding utf8
[ordered]@{
    recorded = Get-Date -Format o
    seconds = $Seconds
    repetitions = $Repetitions
    actorCounts = $ActorCounts
    variants = $Variants
    binaryDirectory = $BinaryDirectory
    logicalProcessors = [Environment]::ProcessorCount
    mode = 'job'
    waitMode = 'poll'
    timerSlotMs = 5
    timerSlots = 256
    timerTraceThresholdMs = 0
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
    $allowed = $side -le 2048 -and $estimate -le $headroom * 0.7
    $plans += [pscustomobject]@{ actors=$actors; mapSide=$side; estimatedMB=$estimate/1MB; budgetMB=$headroom*0.7/1MB; allowed=$allowed }
    $plans | Export-Csv (Join-Path $root 'plan.csv') -NoTypeInformation
    if (-not $allowed) {
        Write-Warning "Stopped before $actors Actors: memory safety budget"
        break
    }

    foreach ($round in 1..$Repetitions) {
        $order = @($Variants)
        if ($round % 2 -eq 0) { [Array]::Reverse($order) }
        foreach ($variant in $order) {
            $relative = "$OutputDirectory/actors-$actors/$variant-$round"
            & (Join-Path $repositoryRoot 'tools/benchmark/Measure-RuntimeCpu.ps1') -Seconds $Seconds -Actors $actors -MovesPerSecond $moves -MapSide $side -Mode job -WaitMode poll -TimerTraceThresholdMs 0 -ExecutablePath "$BinaryDirectory/$variant/AreaUpdateExample.exe" -OutputDirectory $relative | Out-Null
            $result = Get-Content (Join-Path $repositoryRoot "$relative/summary.json") -Raw | ConvertFrom-Json
            $statistics = $variant -notlike '*-off'
            if ($result.taskStatisticsEnabled -ne $statistics -or $result.taskLifetimeVerified -ne $statistics) {
                throw 'Task statistics configuration mismatch'
            }
            if ($result.waitMode -ne 'poll' -or $result.timerSlotMs -ne 5 -or $result.timerSlots -ne 256 -or $result.objectSchedule -ne 'fixed') {
                throw 'Scheduling configuration mismatch'
            }
            if ($null -eq $result.steadyCpuPercent) { throw 'No steady-state CPU samples' }
            $result | Add-Member -NotePropertyName variant -NotePropertyValue $variant
            $result | Add-Member -NotePropertyName round -NotePropertyValue $round
            $results += $result
            $results | Export-Csv (Join-Path $root 'results.csv') -NoTypeInformation
            Write-Output ("actors={0} variant={1} round={2} FPS={3:F3} CPU={4:F2}%" -f $actors,$variant,$round,$result.actorFps,$result.steadyCpuPercent)
        }
    }
}
