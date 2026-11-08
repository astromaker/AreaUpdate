param(
    [string]$OutputDirectory = 'artifacts/area-profile',
    [ValidateSet('standard','queue')][string]$Suite = 'standard',
    [ValidateSet('fixed','adaptive')][string]$AreaAdmissionMode = 'fixed'
)
$repositoryRoot = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)


$ErrorActionPreference = 'Stop'
$root = Join-Path $repositoryRoot $OutputDirectory
if (Test-Path $root) { throw 'Choose a new output directory' }
New-Item -ItemType Directory -Path $root | Out-Null
Copy-Item (Join-Path $repositoryRoot 'Source/x64/Release/AreaUpdateExample.exe') (Join-Path $root 'AreaUpdateExample.exe')
$cases = @(
    @{name='280k-off20'; seconds=20; actors=280000; side=640; moves=8000; stride=0},
    @{name='280k-on20'; seconds=20; actors=280000; side=640; moves=8000; stride=64},
    @{name='280k-on60'; seconds=60; actors=280000; side=640; moves=8000; stride=64},
    @{name='280k-off60'; seconds=60; actors=280000; side=640; moves=8000; stride=0},
    @{name='280k-on20-repeat'; seconds=20; actors=280000; side=640; moves=8000; stride=64},
    @{name='140k-on20'; seconds=20; actors=140000; side=453; moves=4000; stride=64},
    @{name='280k-on60-quiet'; seconds=60; actors=280000; side=640; moves=8000; stride=64},
    @{name='280k-off60-quiet'; seconds=60; actors=280000; side=640; moves=8000; stride=0}
)
if ($Suite -eq 'queue') {
    $cases = @(
        @{name='280k-off60'; seconds=60; actors=280000; side=640; moves=8000; stride=0},
        @{name='280k-on60'; seconds=60; actors=280000; side=640; moves=8000; stride=64},
        @{name='280k-on20'; seconds=20; actors=280000; side=640; moves=8000; stride=64},
        @{name='280k-on20-repeat'; seconds=20; actors=280000; side=640; moves=8000; stride=64},
        @{name='140k-on20'; seconds=20; actors=140000; side=453; moves=4000; stride=64}
    )
}
$cases | ForEach-Object {[pscustomobject]$_} | Export-Csv (Join-Path $root 'plan.csv') -NoTypeInformation
$results = @()
foreach ($case in $cases) {
    $output = "$OutputDirectory/$($case.name)"
    & (Join-Path $repositoryRoot 'tools/benchmark/Measure-RuntimeCpu.ps1') -Seconds $case.seconds -Actors $case.actors -MovesPerSecond $case.moves -MapSide $case.side -Mode area -AreaProfileStride $case.stride -AreaAdmissionMode $AreaAdmissionMode -ExecutablePath "$OutputDirectory/AreaUpdateExample.exe" -OutputDirectory $output | Out-Null
    $summary = Get-Content (Join-Path $repositoryRoot "$output/summary.json") -Raw | ConvertFrom-Json
    if ($case.stride -ne 0) {
        $profile = Import-Csv (Join-Path $repositoryRoot "$output/area-profile.csv")
        $timer = ($profile | Where-Object metric -eq timer_late_ns | Measure-Object count -Sum).Sum
        $ready = ($profile | Where-Object metric -eq ready_to_run_ns | Measure-Object count -Sum).Sum
        $total = ($profile | Where-Object metric -eq deadline_to_run_ns | Measure-Object count -Sum).Sum
        if ($timer -le 0 -or $timer -ne $ready -or $timer -ne $total) { throw 'Invalid Area delay sample counts' }
        $split = @('enqueue_ns','queue_residence_ns','extracted_to_run_ns')
        $readySum = ($profile | Where-Object metric -eq ready_to_run_ns | Measure-Object sum -Sum).Sum
        $splitSum = ($profile | Where-Object metric -in $split | Measure-Object sum -Sum).Sum
        if ($readySum -ne $splitSum) { throw 'Area queue delay decomposition mismatch' }
        $partitionPath = Join-Path $repositoryRoot "$output/area-partitions.csv"
        $partitions = Import-Csv $partitionPath
        $epochCount = ($profile | Where-Object metric -eq epoch_areas | Measure-Object count -Sum).Sum
        $areaCount = ($profile | Where-Object metric -eq epoch_areas | Measure-Object sum -Sum).Sum
        if ($epochCount -ne ($partitions | Measure-Object epochs -Sum).Sum -or $areaCount -ne ($partitions | Measure-Object areas -Sum).Sum) { throw 'Epoch/partition totals mismatch' }
    }
    $summary | Add-Member -NotePropertyName caseName -NotePropertyValue $case.name
    $results += $summary
    $results | Export-Csv (Join-Path $root 'results.csv') -NoTypeInformation
    Write-Output ("{0}: FPS={1:F3} CPU={2:F2}%" -f $case.name,$summary.actorFps,$summary.steadyCpuPercent)
}
