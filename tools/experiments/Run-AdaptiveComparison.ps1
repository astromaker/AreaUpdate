param([string]$OutputDirectory = 'artifacts/area-adaptive/measurements')
$repositoryRoot = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)


$ErrorActionPreference = 'Stop'
$root = Join-Path $repositoryRoot $OutputDirectory
if (Test-Path $root) { throw 'Choose a new output directory' }
New-Item -ItemType Directory -Path $root | Out-Null
Copy-Item (Join-Path $repositoryRoot 'Source/x64/Release/AreaUpdateExample.exe') (Join-Path $root 'AreaUpdateExample.exe')
$results = @()
foreach ($actors in @(140000,280000)) {
    $side = if ($actors -eq 140000) {453} else {640}
    $moves = if ($actors -eq 140000) {4000} else {8000}
    $run = 0
    foreach ($policy in @('fixed','adaptive','adaptive','fixed')) {
        ++$run
        $output = "$OutputDirectory/$actors-$run-$policy"
        & (Join-Path $repositoryRoot 'tools/benchmark/Measure-RuntimeCpu.ps1') -Seconds 20 -Actors $actors -MovesPerSecond $moves -MapSide $side -Mode area -AreaAdmissionMode $policy -AdmissionMax 64 -AdmissionWaitMs 25 -ExecutablePath "$OutputDirectory/AreaUpdateExample.exe" -OutputDirectory $output | Out-Null
        $value = Get-Content (Join-Path $repositoryRoot "$output/summary.json") -Raw | ConvertFrom-Json
        if ($value.areaAdmissionMode -ne $policy -or $value.areaProfileStride -ne 0) { throw 'Admission comparison configuration mismatch' }
        $value | Add-Member -NotePropertyName run -NotePropertyValue $run
        $results += $value
        $results | Export-Csv (Join-Path $root 'results.csv') -NoTypeInformation
        Write-Output ("actors={0} policy={1} FPS={2:F3} CPU={3:F2}% p99gap={4}ms max_service_wait={5}ms" -f $actors,$policy,$value.actorFps,$value.steadyCpuPercent,$value.p99GapBucketMs,$value.maxPartitionServiceWaitMs)
    }
}
foreach ($policy in @('adaptive','fixed')) {
    $output = "$OutputDirectory/280000-60s-$policy"
    & (Join-Path $repositoryRoot 'tools/benchmark/Measure-RuntimeCpu.ps1') -Seconds 60 -Actors 280000 -MovesPerSecond 8000 -MapSide 640 -Mode area -AreaAdmissionMode $policy -AdmissionMax 64 -AdmissionWaitMs 25 -ExecutablePath "$OutputDirectory/AreaUpdateExample.exe" -OutputDirectory $output | Out-Null
    $value = Get-Content (Join-Path $repositoryRoot "$output/summary.json") -Raw | ConvertFrom-Json
    $results += $value
    $results | Export-Csv (Join-Path $root 'results.csv') -NoTypeInformation
    Write-Output ("60s policy={0} FPS={1:F3} CPU={2:F2}% p99gap={3}ms max_service_wait={4}ms" -f $policy,$value.actorFps,$value.steadyCpuPercent,$value.p99GapBucketMs,$value.maxPartitionServiceWaitMs)
}
