param(
    [ValidateRange(10,3600)][int]$Seconds = 30,
    [ValidateRange(1,1000000)][int]$Actors = 70000,
    [ValidateRange(0,1000000)][int]$MovesPerSecond = 2000,
    [ValidateRange(0,3600000)][int]$JoinDurationMs = 5000,
    [ValidateRange(1,10000)][int]$JoinIntervalMs = 1,
    [ValidateRange(0,1000000)][int]$ActorWork = 0,
    [ValidateRange(0,2048)][int]$MapSide = 0,
    [ValidateSet('area','original','job','legacy')][string]$Mode = 'area',
    [ValidateRange(0,60000)][int]$TimerTraceThresholdMs = 0,
    [ValidateSet('signal','poll')][string]$WaitMode = 'signal',
    [ValidateRange(0,1000000)][int]$ObjectSampleStride = 0,
    [ValidateRange(0,1000000)][int]$DispatcherProfileStride = 0,
    [ValidateRange(0,1000000)][int]$AreaProfileStride = 0,
    [ValidateSet('adaptive','fixed','unlimited')][string]$AreaAdmissionMode = 'unlimited',
    [ValidateRange(1,1000)][int]$AdmissionSliceMs = 1,
    [ValidateRange(1,1000)][int]$AdmissionWaitMs = 10,
    [ValidateRange(8,1024)][int]$AdmissionMax = 16,
    [ValidateSet('uniform','clustered','hotspot','partition-skew','boundary')][string]$Layout = 'uniform',
    [uint32]$Seed = 42,
    [switch]$AllowStarvation,
    [ValidateRange(0,1048576)][int]$MaxPrivateMemoryMB = 0,
    [string]$ExecutablePath = 'Source/x64/Release/AreaUpdateExample.exe',
    [string]$OutputDirectory = 'artifacts/actors-70000/cpu'
)
$repositoryRoot = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)


$ErrorActionPreference = 'Stop'
$output = Join-Path $repositoryRoot $OutputDirectory
New-Item -ItemType Directory -Path $output -Force | Out-Null
if (Test-Path (Join-Path $output 'summary.json')) { throw 'Output already contains a measurement; choose another directory' }
$exe = Join-Path $repositoryRoot $ExecutablePath
$logicalProcessors = [Environment]::ProcessorCount
$watch = [Diagnostics.Stopwatch]::StartNew()
$process = Start-Process -FilePath $exe -ArgumentList @($Seconds,$Actors,$MovesPerSecond,$Seed,$JoinDurationMs,$JoinIntervalMs,$ActorWork,$MapSide,$Mode,$TimerTraceThresholdMs,$WaitMode,$ObjectSampleStride,$DispatcherProfileStride,$AreaProfileStride,$AreaAdmissionMode,$AdmissionSliceMs,$AdmissionWaitMs,$AdmissionMax,$Layout) -WindowStyle Hidden -PassThru -RedirectStandardOutput (Join-Path $output 'runtime.log') -RedirectStandardError (Join-Path $output 'stderr.log')
$samples = @()
$previousCpu = 0.0
$previousElapsed = 0.0
while (-not $process.WaitForExit(250)) {
    $process.Refresh()
    if ($process.HasExited) { break }
    if ($MaxPrivateMemoryMB -gt 0 -and $process.PrivateMemorySize64 -gt $MaxPrivateMemoryMB * 1MB) {
        $process.Kill()
        $process.WaitForExit()
        $process.Dispose()
        throw "Runtime exceeded private memory safety limit: $MaxPrivateMemoryMB MiB"
    }
    $cpu = $process.TotalProcessorTime.TotalSeconds
    $elapsed = $watch.Elapsed.TotalSeconds
    $interval = $elapsed - $previousElapsed
    $samples += [pscustomobject]@{
        elapsedSeconds = $elapsed
        intervalSeconds = $interval
        cpuSeconds = $cpu
        cpuDeltaSeconds = $cpu - $previousCpu
        cpuPercent = 100 * ($cpu - $previousCpu) / $interval / $logicalProcessors
        privateBytes = $process.PrivateMemorySize64
        workingSetBytes = $process.WorkingSet64
    }
    $previousCpu = $cpu
    $previousElapsed = $elapsed
    if ($elapsed -gt $Seconds + 60) {
        $process.Kill()
        $process.WaitForExit()
        throw 'Runtime exceeded watchdog deadline'
    }
}
$process.WaitForExit()
$elapsed = $watch.Elapsed.TotalSeconds
$cpu = $process.TotalProcessorTime.TotalSeconds
$samples | Export-Csv -LiteralPath (Join-Path $output 'samples.csv') -NoTypeInformation
if ($process.ExitCode -ne 0 -and -not ($AllowStarvation -and $process.ExitCode -eq 3)) {
    throw "Runtime failed: $($process.ExitCode)"
}

$values = @{}
$traceRows = @()
$dispatcherRows = @()
$areaProfileRows = @()
$areaSecondRows = @()
$areaEpochRows = @()
$areaVisitRows = @()
$areaPartitionRows = @()
$schedulingRows = @()
$waitReturnRows = @()
foreach ($line in Get-Content (Join-Path $output 'runtime.log')) {
    if ($line -match '^WAIT_RETURN ') {
        $row = [ordered]@{}
        foreach ($match in [regex]::Matches($line, '(\w+)=([^ ]+)')) {
            $key = $match.Groups[1].Value
            $row[$key] = if ($key -in @('scope','kind')) { $match.Groups[2].Value } else { [double]$match.Groups[2].Value }
        }
        $waitReturnRows += [pscustomobject]$row
    }
    if ($line -match '^AREA_SCHEDULING_PARTITION ') {
        $row = [ordered]@{}
        foreach ($match in [regex]::Matches($line, '(\w+)=([^ ]+)')) {
            $row[$match.Groups[1].Value] = [double]$match.Groups[2].Value
        }
        $schedulingRows += [pscustomobject]$row
    }
    if ($line -match '^(AREA_PROFILE |AREA_SECOND |AREA_PROFILE_SECOND |AREA_PROFILE_EPOCH |AREA_PROFILE_PARTITION )') {
        $row = [ordered]@{}
        foreach ($match in [regex]::Matches($line, '(\w+)=([^ ]+)')) {
            $key = $match.Groups[1].Value
            $value = $match.Groups[2].Value
            $row[$key] = if ($key -eq 'metric') {$value} else {[double]::Parse($value, [Globalization.CultureInfo]::InvariantCulture)}
        }
        if ($line.StartsWith('AREA_PROFILE ')) { $areaProfileRows += [pscustomobject]$row }
        elseif ($line.StartsWith('AREA_SECOND ')) { $areaSecondRows += [pscustomobject]$row }
        elseif ($line.StartsWith('AREA_PROFILE_EPOCH ')) { $areaVisitRows += [pscustomobject]$row }
        elseif ($line.StartsWith('AREA_PROFILE_PARTITION ')) { $areaPartitionRows += [pscustomobject]$row }
        else { $areaEpochRows += [pscustomobject]$row }
    }
    if ($line -match '^DISPATCHER_WORKER ') {
        $worker = [ordered]@{}
        foreach ($match in [regex]::Matches($line, '(\w+)=([^ ]+)')) {
            $worker[$match.Groups[1].Value] = [double]$match.Groups[2].Value
        }
        $dispatcherRows += [pscustomobject]$worker
    }
    if ($line -match '^TIMER_TRACE ') {
        $trace = [ordered]@{}
        foreach ($match in [regex]::Matches($line, '(\w+)=([^ ]+)')) {
            $trace[$match.Groups[1].Value] = [int64]$match.Groups[2].Value
        }
        $traceRows += [pscustomobject]$trace
    }
    if ($line -match '^(SUMMARY |RUNTIME |RUNTIME_DIAGNOSTICS |WAIT_RETURN_CONFIG |WAIT_POLICY |ACTIVE_WAIT |TARGET |SLEEP |WAIT |WORLD |BACKEND |RADIUS |TIMER_PROFILE |DIAGNOSTICS |OBJECT_PROFILE |DISPATCHER_PROFILE |AREA_DIAGNOSTICS |AREA_SCHEDULING |STEADY |STEADY_FAIRNESS )') {
        foreach ($match in [regex]::Matches($line, '(\w+)=([^ ]+)')) {
            $values[$match.Groups[1].Value] = $match.Groups[2].Value
        }
    }
}
if ($waitReturnRows.Count -gt 0) {
    $waitReturnRows | Export-Csv (Join-Path $output 'wait-returns.csv') -NoTypeInformation
}
if ($traceRows.Count -gt 0) {
    $traceRows | Export-Csv -LiteralPath (Join-Path $output 'timer-traces.csv') -NoTypeInformation
}
if ($dispatcherRows.Count -gt 0) {
    $dispatcherRows | Export-Csv -LiteralPath (Join-Path $output 'dispatcher-workers.csv') -NoTypeInformation
}
if ($areaProfileRows.Count -gt 0) {
    $areaProfileRows | Export-Csv (Join-Path $output 'area-profile.csv') -NoTypeInformation
    $areaSecondRows | Export-Csv (Join-Path $output 'area-seconds.csv') -NoTypeInformation
    $areaEpochRows | Export-Csv (Join-Path $output 'area-profile-seconds.csv') -NoTypeInformation
    $areaVisitRows | Export-Csv (Join-Path $output 'area-epochs.csv') -NoTypeInformation
    $areaPartitionRows | Export-Csv (Join-Path $output 'area-partitions.csv') -NoTypeInformation
}
if ($schedulingRows.Count -gt 0) {
    $schedulingRows | Export-Csv (Join-Path $output 'scheduling-partitions.csv') -NoTypeInformation
}
$taskStatisticsEnabled = -not $values.ContainsKey('task_statistics') -or $values['task_statistics'] -eq '1'
foreach ($key in @('overlaps','never_updated','tasks_live','tasks_pending')) {
    if ($key -eq 'tasks_live' -and -not $taskStatisticsEnabled) { continue }
    if ($key -eq 'never_updated' -and $AllowStarvation) { continue }
    if ($values[$key] -ne '0') { throw "Validation failed: $key" }
}
if ($values['actors'] -ne [string]$Actors) { throw 'Not all Actors joined' }
if ($values['actor_work'] -ne [string]$ActorWork) { throw 'Actor work option or diagnostic output mismatch' }
if ($MapSide -gt 0 -and $values['map_side'] -ne [string]$MapSide) { throw 'Map side mismatch' }
if ($values.ContainsKey('layout') -and $values['layout'] -ne $Layout) { throw 'Layout mismatch' }
if ($Layout -ne 'uniform' -and -not $values.ContainsKey('layout')) { throw 'Executable does not support layout selection' }
if ($values.ContainsKey('admission_max') -and $values['admission_max'] -ne [string]$AdmissionMax) { throw 'Admission maximum mismatch' }
if ($values.ContainsKey('admission_wait_ms') -and $values['admission_wait_ms'] -ne [string]$AdmissionWaitMs) { throw 'Admission wait mismatch' }
if ($values.ContainsKey('admission_slice_ms') -and $values['admission_slice_ms'] -ne [string]$AdmissionSliceMs) { throw 'Admission slice mismatch' }
if ($TimerTraceThresholdMs -gt 0 -and $values['trace_threshold_ms'] -ne [string]$TimerTraceThresholdMs) { throw 'Timer trace configuration mismatch' }
if ($values.ContainsKey('mode') -and $values['mode'] -ne $Mode) { throw 'Runtime mode mismatch' }
if ($values.ContainsKey('wait_mode') -and $values['wait_mode'] -ne $WaitMode) { throw 'Wait mode mismatch' }
if ($ObjectSampleStride -gt 0 -and $values['object_sample_stride'] -ne [string]$ObjectSampleStride) { throw 'Object sample configuration mismatch' }
if ($values.ContainsKey('object_sample_stride') -and $values['object_sample_stride'] -ne [string]$ObjectSampleStride) { throw 'Object sample stride mismatch' }
if ($DispatcherProfileStride -gt 0 -and $values['dispatcher_profile_stride'] -ne [string]$DispatcherProfileStride) { throw 'Dispatcher profile configuration mismatch' }
if ($AreaProfileStride -gt 0 -and $values['area_profile_stride'] -ne [string]$AreaProfileStride) { throw 'Area profile configuration mismatch' }
if ($values.ContainsKey('area_admission_mode') -and $values['area_admission_mode'] -ne $AreaAdmissionMode) { throw 'Area admission mode mismatch' }
if ($values.ContainsKey('dispatcher_profile_stride') -and $values['dispatcher_profile_stride'] -ne [string]$DispatcherProfileStride) { throw 'Dispatcher profile stride mismatch' }
if ($DispatcherProfileStride -gt 0) {
    if ($dispatcherRows.Count -ne [int]$values['dispatcher_workers']) { throw 'Dispatcher Worker count mismatch' }
    foreach ($field in @('dispatch_tasks','executer_tasks','samples')) {
        $total = ($dispatcherRows | Measure-Object -Property $field -Sum).Sum
        if ($total -ne [double]$values["dispatcher_$field"]) { throw "Dispatcher total mismatch: $field" }
    }
    if ([uint64]$values['dispatcher_samples'] -eq 0) { throw 'Dispatcher profile has no samples' }
}
if ($Mode -ne 'area' -and -not $values.ContainsKey('mode')) { throw 'Executable does not support runtime modes' }

$steadyStart = if ($values.ContainsKey('joined_all_ms')) { [double]$values['joined_all_ms']/1000.0 + 1 } else { $JoinDurationMs/1000.0 + 1 }
$steady = @($samples | Where-Object { $_.elapsedSeconds - $_.intervalSeconds -ge $steadyStart })
$steadyWall = ($steady | Measure-Object intervalSeconds -Sum).Sum
$steadyCpu = ($steady | Measure-Object cpuDeltaSeconds -Sum).Sum
$timerHistogramAvailable = [uint64]$values['trace_tasks'] -gt 0
$dispatcherProfileAvailable = $DispatcherProfileStride -gt 0 -and $dispatcherRows.Count -gt 0
$dispatcherMeanBusyMs = ($dispatcherRows | Measure-Object busy_ms -Average).Average
$dispatcherMaxBusyMs = ($dispatcherRows | Measure-Object busy_ms -Maximum).Maximum
$dispatcherMeanTasks = ($dispatcherRows | Measure-Object dispatch_tasks -Average).Average
$dispatcherMaxTasks = ($dispatcherRows | Measure-Object dispatch_tasks -Maximum).Maximum
$workerDiagnostics = $values.ContainsKey('worker_diagnostics_enabled') -eq $false -or $values['worker_diagnostics_enabled'] -eq '1'
$summary = [ordered]@{
    workerDiagnosticsEnabled = $workerDiagnostics
    waitReturnDiagnosticsEnabled = $values['wait_return_enabled'] -eq '1'
    activeWaitUs = if ($values.ContainsKey('active_wait_us')) { [uint64]$values['active_wait_us'] } else { $null }
    activeWaits = if ($values.ContainsKey('active_waits')) { [uint64]$values['active_waits'] } else { $null }
    activeWaitMs = if ($values.ContainsKey('active_wait_ns')) { [double]$values['active_wait_ns']/1000000.0 } else { $null }
    workerDiagnosticsReported = $values.ContainsKey('worker_diagnostics_enabled')
    recorded = Get-Date -Format o
    executableSha256 = (Get-FileHash $exe).Hash
    actors = $Actors
    areaProfileStride = $AreaProfileStride
    areaAdmissionMode = if ($values.ContainsKey('area_admission_mode')) {$values['area_admission_mode']} else {'fixed'}
    schedulingExtensions = if ($values.ContainsKey('extensions')) {[uint64]$values['extensions']} else {$null}
    maxPartitionServiceWaitMs = if ($values['service_wait_tracking'] -eq '1') {($schedulingRows | Measure-Object max_service_wait_ms -Maximum).Maximum} else {$null}
    actorWork = $ActorWork
    dispatcherProfileStride = $DispatcherProfileStride
    dispatcherProfileAvailable = $dispatcherProfileAvailable
    dispatcherProfileSamples = [uint64]$values['dispatcher_samples']
    dispatcherDispatchTasks = if ($dispatcherProfileAvailable) { [uint64]$values['dispatcher_dispatch_tasks'] } else { $null }
    dispatcherExecuterTasks = if ($dispatcherProfileAvailable) { [uint64]$values['dispatcher_executer_tasks'] } else { $null }
    dispatcherMaxToMeanTasks = if ($dispatcherProfileAvailable -and $dispatcherMeanTasks -gt 0) { $dispatcherMaxTasks / $dispatcherMeanTasks } else { $null }
    dispatcherMaxToMeanBusy = if ($dispatcherProfileAvailable -and $dispatcherMeanBusyMs -gt 0) { $dispatcherMaxBusyMs / $dispatcherMeanBusyMs } else { $null }
    dispatcherMinBusyMs = if ($dispatcherProfileAvailable) { ($dispatcherRows | Measure-Object busy_ms -Minimum).Minimum } else { $null }
    dispatcherMaxBusyMs = if ($dispatcherProfileAvailable) { $dispatcherMaxBusyMs } else { $null }
    dispatcherMinQueueMeanMs = if ($dispatcherProfileAvailable) { ($dispatcherRows | Measure-Object queue_mean_ms -Minimum).Minimum } else { $null }
    dispatcherMaxQueueMeanMs = if ($dispatcherProfileAvailable) { ($dispatcherRows | Measure-Object queue_mean_ms -Maximum).Maximum } else { $null }
    dispatcherMaxWorkerQueueP99Ms = if ($dispatcherProfileAvailable) { ($dispatcherRows | Measure-Object queue_p99_ms -Maximum).Maximum } else { $null }
    objectSampleStride = $ObjectSampleStride
    sampledActors = [uint64]$values['object_sample_actors']
    objectSamples = [uint64]$values['object_samples']
    objectQueueMeanUs = if ([uint64]$values['object_samples'] -gt 0) { [double]$values['object_queue_mean_us'] } else { $null }
    objectQueueMaxUs = if ([uint64]$values['object_samples'] -gt 0) { [double]$values['object_queue_max_us'] } else { $null }
    objectQueueP50Ms = if ([uint64]$values['object_samples'] -gt 0) { [double]$values['object_queue_p50_ms'] } else { $null }
    objectQueueP95Ms = if ([uint64]$values['object_samples'] -gt 0) { [double]$values['object_queue_p95_ms'] } else { $null }
    objectQueueP99Ms = if ([uint64]$values['object_samples'] -gt 0) { [double]$values['object_queue_p99_ms'] } else { $null }
    objectTimerMeanMs = if ([uint64]$values['object_samples'] -gt 0) { [double]$values['object_timer_mean_ms'] } else { $null }
    objectTimerP99Ms = if ([uint64]$values['object_samples'] -gt 0) { [double]$values['object_timer_p99_ms'] } else { $null }
    objectActorMeanUs = if ([uint64]$values['object_samples'] -gt 0) { [double]$values['object_actor_mean_us'] } else { $null }
    objectActorMaxUs = if ([uint64]$values['object_samples'] -gt 0) { [double]$values['object_actor_max_us'] } else { $null }
    taskStatisticsEnabled = $taskStatisticsEnabled
    taskLifetimeVerified = $taskStatisticsEnabled -and $values['tasks_live'] -eq '0'
    tasksCreated = if ($taskStatisticsEnabled) { [uint64]$values['tasks_created'] } else { $null }
    tasksDestroyed = if ($taskStatisticsEnabled) { [uint64]$values['tasks_destroyed'] } else { $null }
    tasksLive = if ($taskStatisticsEnabled) { [uint64]$values['tasks_live'] } else { $null }
    timerSlotMs = if ($values.ContainsKey('slot_ms')) { [int]$values['slot_ms'] } else { $null }
    timerSlots = if ($values.ContainsKey('slots')) { [int]$values['slots'] } else { $null }
    traceThresholdMs = $TimerTraceThresholdMs
    tracedTasks = [uint64]$values['trace_tasks']
    slowTimerTasks = [uint64]$values['trace_slow']
    retainedTraces = [uint64]$values['trace_retained']
    droppedTraces = [uint64]$values['trace_dropped']
    timerHistogramAvailable = $timerHistogramAvailable
    timerP50Ms = if ($timerHistogramAvailable) { [uint64]$values['timer_p50_ms'] } else { $null }
    timerP95Ms = if ($timerHistogramAvailable) { [uint64]$values['timer_p95_ms'] } else { $null }
    timerP99Ms = if ($timerHistogramAvailable) { [uint64]$values['timer_p99_ms'] } else { $null }
    timerP999Ms = if ($timerHistogramAvailable) { [uint64]$values['timer_p999_ms'] } else { $null }
    mode = $Mode
    waitMode = if ($values.ContainsKey('wait_mode')) { $values['wait_mode'] } else { 'poll' }
    signalWaits = [uint64]$values['signal_waits']
    notifiedWaits = [uint64]$values['notified_waits']
    meanWakeLatencyUs = if ($values.ContainsKey('wake_latency_mean_us')) { [double]$values['wake_latency_mean_us'] } else { $null }
    maxWakeLatencyUs = if ($values.ContainsKey('wake_latency_max_us')) { [double]$values['wake_latency_max_us'] } else { $null }
    timerWorkers = [int]$values['timer_workers']
    dispatcherWorkers = [int]$values['dispatcher_workers']
    legacyUpdates = [uint64]$values['legacy_updates']
    legacyNeighborFailures = [uint64]$values['legacy_neighbor_failures']
    radiusUpdates = [uint64]$values['radius_updates']
    radiusAdmissionFailures = [uint64]$values['radius_admission_failures']
    radiusSkippedTicks = [uint64]$values['radius_skipped_ticks']
    objectSchedule = if ($values.ContainsKey('object_schedule')) { $values['object_schedule'] } else { 'relative' }
    areaSchedule = if ($values.ContainsKey('area_schedule')) { $values['area_schedule'] } elseif ($Mode -eq 'area' -or $Mode -eq 'original') { 'relative' } else { 'not_applicable' }
    areaSkippedTicks = if ($Mode -eq 'area' -and $values.ContainsKey('area_skipped_ticks')) { [uint64]$values['area_skipped_ticks'] } else { $null }
    objectSkippedTicks = if ($values.ContainsKey('object_skipped_ticks')) { [uint64]$values['object_skipped_ticks'] } else { $null }
    successfulMoves = [uint64]$values['moves']
    mapSide = [int]$values['map_side']
    movesPerSecond = $MovesPerSecond
    activeAreas = [uint64]$values['active_areas']
    minActivePerPartition = [uint64]$values['min_active_per_partition']
    maxActivePerPartition = [uint64]$values['max_active_per_partition']
    peakSamplePrivateMB = ($samples | Measure-Object privateBytes -Maximum).Maximum / 1MB
    peakSampleWorkingSetMB = ($samples | Measure-Object workingSetBytes -Maximum).Maximum / 1MB
    actorBytes = [int]$values['actor_bytes']
    areaBytes = [int]$values['area_bytes']
    processExitCode = $process.ExitCode
    neverUpdated = [uint64]$values['never_updated']
    logicalProcessors = $logicalProcessors
    elapsedSeconds = $elapsed
    processCpuSeconds = $cpu
    averageCpuPercent = 100 * $cpu / $elapsed / $logicalProcessors
    averageCores = $cpu / $elapsed
    steadySampleSeconds = $steadyWall
    steadyCpuPercent = if ($steadyWall -gt 0) { 100 * $steadyCpu / $steadyWall / $logicalProcessors } else { $null }
    peakSampleCpuPercent = ($samples | Measure-Object cpuPercent -Maximum).Maximum
    actorFps = [double]$values['actor_fps']
    steadyActorFps = if ($values.ContainsKey('steady_actor_fps')) {[double]$values['steady_actor_fps']} else {$null}
    admissionSliceMs = if ($values.ContainsKey('admission_slice_ms')) {[int]$values['admission_slice_ms']} else {$null}
    admissionWaitMs = if ($values.ContainsKey('admission_wait_ms')) {[int]$values['admission_wait_ms']} else {$null}
    admissionMax = if ($values.ContainsKey('admission_max')) {[int]$values['admission_max']} else {$null}
    layout = $Layout
    seed = $Seed
    workerBusyMs = [double]$values['busy_ms']
    workerIdleMs = [double]$values['idle_ms']
    expectedUpdates = [uint64]$values['expected_updates']
    updateDeficit = [uint64]$values['update_deficit']
    belowTargetActors = [uint64]$values['below_target_actors']
    steadyFairnessAvailable = $values['steady_fairness_available'] -eq '1'
    steadyMinActorFps = if ($values.ContainsKey('steady_min_actor_fps')) { [double]$values['steady_min_actor_fps'] } else { $null }
    steadyMaxActorFps = if ($values.ContainsKey('steady_max_actor_fps')) { [double]$values['steady_max_actor_fps'] } else { $null }
    steadyBelowTargetActors = if ($values.ContainsKey('steady_below_target_actors')) { [uint64]$values['steady_below_target_actors'] } else { $null }
    steadyUnder19Actors = if ($values.ContainsKey('steady_under_19_actors')) { [uint64]$values['steady_under_19_actors'] } else { $null }
    steadyUnder10Actors = if ($values.ContainsKey('steady_under_10_actors')) { [uint64]$values['steady_under_10_actors'] } else { $null }
    lateGaps = [uint64]$values['late_gaps']
    sleepCount = [uint64]$values['count']
    sleepReadySamples = [uint64]$values['ready_samples']
    sleepBlockedSamples = [uint64]$values['blocked_samples']
    sleepPastTimerCount = [uint64]$values['past_timer_count']
    sleepPastTimerMs = [uint64]$values['past_timer_ms']
    sleepOvershootMs = [double]$values['overshoot_ms']
    sleepWakeRequests = [uint64]$values['wake_requests']
    maxTimerDelayMs = if ($workerDiagnostics) { [uint64]$values['max_timer_delay_ms'] } else { $null }
    maxQueueDelayMs = if ($workerDiagnostics) { [uint64]$values['max_queue_delay_ms'] } else { $null }
    maxGapMs = [uint64]$values['max_gap_ms']
    p99GapBucketMs = [uint64]$values['p99_gap_bucket_ms']
}
$summary | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $output 'summary.json') -Encoding utf8
$summary | ConvertTo-Json
$process.Dispose()
