param(
    [ValidateRange(15,120)][int]$Seconds = 20,
    [ValidateRange(1,1000000)][int[]]$ActorCounts = @(10000,70000,140000,280000,560000),
    [string]$OutputDirectory = 'artifacts/current-model-comparison',
    [string]$SourceBinaryDirectory = 'Source/x64/Release',
    [string]$MemoryReferenceCsv = ''
)
$repositoryRoot = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)


$ErrorActionPreference = 'Stop'
$root = Join-Path $repositoryRoot $OutputDirectory
if (Test-Path $root) { throw 'Choose a new output directory; preserve previous measurements' }
New-Item -ItemType Directory "$root/bin" -Force | Out-Null
foreach ($name in @('AreaUpdateExample','UpdateComparison')) {
    foreach ($extension in @('exe','pdb')) {
        Copy-Item "$repositoryRoot/$SourceBinaryDirectory/$name.$extension" "$root/bin/"
    }
}

$configuration = @{
    AREAUPDATE_RADIUS_PROFILE='0'; AREAUPDATE_RADIUS_BATCH='64'; AREAUPDATE_RADIUS_SORT_TIMERS='1'
    AREAUPDATE_RADIUS_TIMER_BATCH='1'; AREAUPDATE_RADIUS_PUBLISH_BATCH='1'; AREAUPDATE_RADIUS_FAST_BUCKET='1'
    AREAUPDATE_RADIUS_MERGE_NOTIFY='1'; AREAUPDATE_RADIUS_QUEUE_DIAGNOSTICS='0'; AREAUPDATE_RADIUS_LATENCY_STRIDE='0'
}
$saved = @{}
$results = @()
$plans = @()
$reference = @()
if ($MemoryReferenceCsv -ne '') {
    $reference = @(Import-Csv (Join-Path $repositoryRoot $MemoryReferenceCsv))
    $hash = (Get-FileHash "$root/bin/AreaUpdateExample.exe").Hash
    foreach ($row in $reference) {
        if ($row.executableSha256 -ne $hash -or $row.processExitCode -ne 0 -or
            [double]$row.peakSamplePrivateMB -le 0 -or [int]$row.mapSide -le 0 -or [int]$row.actors -le 0) {
            throw 'Memory reference must contain successful measurements of the same executable'
        }
    }
    foreach ($mode in @('area','legacy','job')) {
        if (@($reference | Where-Object { $_.mode -eq $mode }).Count -lt 2) {
            throw "Memory reference requires at least two runs of $mode"
        }
    }
}
try {
    foreach ($key in $configuration.Keys) {
        $saved[$key] = [Environment]::GetEnvironmentVariable($key,'Process')
        [Environment]::SetEnvironmentVariable($key,$configuration[$key],'Process')
    }
    [ordered]@{
        recorded=(Get-Date -Format o); seconds=$Seconds; seeds=@(42,43); actorCounts=$ActorCounts
        executableSha256=(Get-FileHash "$root/bin/AreaUpdateExample.exe").Hash
        logicalProcessors=[Environment]::ProcessorCount; joinDurationMs=1000; joinIntervalMs=1
        actorWork=0; waitMode='signal'; timerTraceThresholdMs=0; radiusEnvironment=$configuration
        memoryReferenceCsv=$MemoryReferenceCsv; memoryReferenceMargin=1.35; memoryReferenceReserveMB=128
        models=@{area='partition';legacy='optimized radius lock';job='per Actor TaskExecuter'}
    } | ConvertTo-Json -Depth 5 | Set-Content "$root/environment.json" -Encoding utf8

    foreach ($mode in @('area','legacy','job')) {
        foreach ($layout in @('uniform','boundary')) {
            & "$root/bin/UpdateComparison.exe" --motion-probe $mode 3 5000 42 verify $layout none 64 0 0 0 *> "$root/verify-$mode-$layout.log"
            if ($LASTEXITCODE -ne 0) { throw "Motion verification failed: $mode $layout" }
        }
    }

    $index = 0
    foreach ($actors in $ActorCounts | Sort-Object -Unique) {
        $side = [int][math]::Ceiling(320 * [math]::Sqrt($actors / 70000.0))
        $moves = [int][math]::Ceiling($actors * 2000.0 / 70000)
        [GC]::Collect()
        $memory = [GC]::GetGCMemoryInfo()
        $budget = [math]::Max([int64]0,$memory.TotalAvailableMemoryBytes - $memory.MemoryLoadBytes) * 0.7
        $estimate = [double]$actors * 1800 + [double]$side * $side * 900 + 128MB
        if ($MemoryReferenceCsv -ne '') {
            $scaled = @($reference | ForEach-Object {
                $factor = [math]::Max(1.0,[math]::Max($actors / [double]$_.actors,
                    [double]$side * $side / ([double]$_.mapSide * [double]$_.mapSide)))
                [double]$_.peakSamplePrivateMB * 1MB * $factor
            })
            $estimate = ($scaled | Measure-Object -Maximum).Maximum * 1.35 + 128MB
        }
        $allowed = $side -le 2048 -and $estimate -le $budget
        $plans += [pscustomobject]@{actors=$actors;mapSide=$side;movesPerSecond=$moves;estimatedMB=$estimate/1MB;budgetMB=$budget/1MB;allowed=$allowed;memoryReferenceCsv=$MemoryReferenceCsv}
        $plans | Export-Csv "$root/plan.csv" -NoTypeInformation
        if ($allowed -eq $false) {
            Write-Warning "Stopped before $actors Actors: conservative memory safety budget; completed results are preserved"
            break
        }

        $baseOrder = switch ($index % 3) {
            0 { @('area','legacy','job') }
            1 { @('legacy','job','area') }
            2 { @('job','area','legacy') }
        }
        foreach ($seed in @(42,43)) {
            $order = @($baseOrder)
            if ($seed -eq 43) { [array]::Reverse($order) }
            foreach ($mode in $order) {
                $name = "$actors-$seed-$mode"
                & "$repositoryRoot/tools/benchmark/Measure-RuntimeCpu.ps1" -Seconds $Seconds -Actors $actors -MovesPerSecond $moves `
                    -JoinDurationMs 1000 -JoinIntervalMs 1 -ActorWork 0 -MapSide $side -Mode $mode -Seed $seed `
                    -TimerTraceThresholdMs 0 -WaitMode signal -AreaAdmissionMode unlimited -AllowStarvation `
                    -MaxPrivateMemoryMB ([int][math]::Floor($budget/1MB)) `
                    -ExecutablePath "$OutputDirectory/bin/AreaUpdateExample.exe" -OutputDirectory "$OutputDirectory/$name" | Out-Null
                $row = Get-Content "$root/$name/summary.json" -Raw | ConvertFrom-Json
                $policy = if ($mode -eq 'job') { $row.objectSchedule } else { $row.areaSchedule }
                if ($row.processExitCode -notin @(0,3) -or $row.tasksLive -ne 0 -or
                    $row.taskStatisticsEnabled -ne $true -or $row.waitMode -ne 'signal' -or $policy -ne 'fixed' -or
                    $row.timerSlotMs -ne 5 -or $row.timerSlots -ne 256 -or $null -eq $row.steadyCpuPercent -or
                    $row.traceThresholdMs -ne 0 -or $row.executableSha256 -ne (Get-FileHash "$root/bin/AreaUpdateExample.exe").Hash) {
                    throw "Runtime/configuration validation failed: $name"
                }
                $row | Add-Member caseName $name
                $results += $row
                $results | Export-Csv "$root/results.csv" -NoTypeInformation
                "$name FPS=$($row.steadyActorFps) CPU=$($row.steadyCpuPercent) minFPS=$($row.steadyMinActorFps) maxGap=$($row.maxGapMs)"
            }
        }
        ++$index
    }
} finally {
    foreach ($key in $saved.Keys) { [Environment]::SetEnvironmentVariable($key,$saved[$key],'Process') }
}

$results | Group-Object actors,mode | ForEach-Object {
    $group = $_.Group
    [pscustomobject]@{
        actors=$group[0].actors; mode=$group[0].mode; runs=$group.Count
        meanFps=($group | Measure-Object steadyActorFps -Average).Average
        minRunFps=($group | Measure-Object steadyActorFps -Minimum).Minimum
        maxRunFps=($group | Measure-Object steadyActorFps -Maximum).Maximum
        meanCpuPercent=($group | Measure-Object steadyCpuPercent -Average).Average
        worstMinActorFps=($group | Measure-Object steadyMinActorFps -Minimum).Minimum
        worstP99GapUpperMs=($group | Measure-Object p99GapBucketMs -Maximum).Maximum
        worstMaxGapMs=($group | Measure-Object maxGapMs -Maximum).Maximum
        neverUpdated=($group | Measure-Object neverUpdated -Sum).Sum
    }
} | Export-Csv "$root/aggregate.csv" -NoTypeInformation
