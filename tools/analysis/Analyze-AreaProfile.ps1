param([string]$InputDirectory = 'artifacts/area-profile')
$repositoryRoot = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)


$ErrorActionPreference = 'Stop'
$root = Join-Path $repositoryRoot $InputDirectory
$windows = @(@{from=6; to=20}, @{from=20; to=40}, @{from=40; to=60})
$aggregates = @()
$trends = @()
$pressure = @()
foreach ($directory in Get-ChildItem $root -Directory) {
    $profilePath = Join-Path $directory.FullName 'area-profile.csv'
    if (-not (Test-Path $profilePath)) { continue }
    $rows = Import-Csv $profilePath
    $summary = Get-Content (Join-Path $directory.FullName 'summary.json') -Raw | ConvertFrom-Json
    $fullSeconds = [int][math]::Floor($summary.elapsedSeconds)
    $epochRows = Import-Csv (Join-Path $directory.FullName 'area-profile-seconds.csv')
    foreach ($window in $windows) {
        if ($window.from -ge $fullSeconds) { continue }
        $selected = @($rows | Where-Object { [int]$_.second -ge $window.from -and [int]$_.second -lt $window.to })
        foreach ($group in $selected | Group-Object metric) {
            $count = ($group.Group | Measure-Object count -Sum).Sum
            $sum = ($group.Group | Measure-Object sum -Sum).Sum
            $aggregates += [pscustomobject]@{
                caseName=$directory.Name; fromSecond=$window.from; toSecond=$window.to
                metric=$group.Name; count=$count; sum=$sum
                mean=if ($count -gt 0) {$sum/$count} else {$null}
                maximum=($group.Group | Measure-Object max -Maximum).Maximum
                # Per-second bounds cannot reconstruct a combined percentile.
                maxSecondP99Upper=($group.Group | Measure-Object p99_upper -Maximum).Maximum
            }
        }
        $epochs = @($selected | Where-Object metric -eq epoch_areas)
        if ($epochs.Count -gt 0) {
            $count = ($epochs | Measure-Object count -Sum).Sum
            $selectedEpochs = @($epochRows | Where-Object { [int]$_.second -ge $window.from -and [int]$_.second -lt $window.to })
            $exhausted = ($selectedEpochs | Measure-Object budget_exhausted_epochs -Sum).Sum
            $backlog = ($selectedEpochs | Measure-Object budget_backlog_epochs -Sum).Sum
            $areas = ($epochs | Measure-Object sum -Sum).Sum
            $pressure += [pscustomobject]@{
                caseName=$directory.Name; fromSecond=$window.from; toSecond=$window.to
                epochs=$count; areas=$areas
                budgetExhaustedEpochs=$exhausted; budgetBacklogEpochs=$backlog
                backlogPercent=if ($count -gt 0) {100*$backlog/$count} else {$null}
                areasPerEpoch=if ($count -gt 0) {$areas/$count} else {$null}
                completedEpochAreasPerSecond=$areas/($window.to-$window.from)
            }
        }
    }
    foreach ($row in Import-Csv (Join-Path $directory.FullName 'area-seconds.csv')) {
        $row | Add-Member -NotePropertyName caseName -NotePropertyValue $directory.Name
        $trends += $row
    }
}
$aggregates | Export-Csv (Join-Path $root 'profile-windows.csv') -NoTypeInformation
$trends | Export-Csv (Join-Path $root 'fps-trend.csv') -NoTypeInformation
$pressure | Export-Csv (Join-Path $root 'epoch-pressure-windows.csv') -NoTypeInformation
$aggregates | Where-Object metric -in @('batch_ns','timer_late_ns','enqueue_ns','queue_residence_ns','extracted_to_run_ns','epoch_tail_ns','blocked_return_ns') | Format-Table caseName,fromSecond,toSecond,metric,count,mean,maximum
