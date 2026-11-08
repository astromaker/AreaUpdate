param(
    [Parameter(Mandatory=$true)][string]$BaselineDirectory,
    [Parameter(Mandatory=$true)][string]$CandidateDirectory,
    [Parameter(Mandatory=$true)][string]$OutputDirectory,
    [string[]]$Modes=@('job'),
    [int[]]$ActorCounts=@(70000,280000),
    [ValidateSet('0','1')][string]$BaselineDiagnostics='1',
    [ValidateSet('0','1')][string]$CandidateDiagnostics='1',
    [ValidateRange(0,1000000)][int]$ObjectSampleStride=0
)
$repositoryRoot = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)


$ErrorActionPreference='Stop'
$root=Join-Path $repositoryRoot $OutputDirectory
if(Test-Path $root){throw 'Preserve previous measurements'}
New-Item -ItemType Directory $root|Out-Null
$rows=@()
$directories=@{baseline=$BaselineDirectory;candidate=$CandidateDirectory}
$savedDiagnostics=[Environment]::GetEnvironmentVariable('AREAUPDATE_WORKER_DIAGNOSTICS','Process')
try {
foreach($actors in $ActorCounts){
    $side=[int][math]::Ceiling(320*[math]::Sqrt($actors/70000.0))
    $moves=[int][math]::Ceiling($actors*2000.0/70000)
    foreach($seed in @(42,43)){
        $order=if($seed -eq 42){@('baseline','candidate')}else{@('candidate','baseline')}
        foreach($mode in $Modes){
            foreach($version in $order){
                $env:AREAUPDATE_WORKER_DIAGNOSTICS=if($version -eq 'baseline'){$BaselineDiagnostics}else{$CandidateDiagnostics}
                $name="$actors-$seed-$mode-$version"
                & "$repositoryRoot/tools/benchmark/Measure-RuntimeCpu.ps1" -Seconds 20 -Actors $actors -MovesPerSecond $moves `
                    -MapSide $side -Seed $seed -Mode $mode -JoinDurationMs 1000 -JoinIntervalMs 1 -WaitMode signal `
                    -ObjectSampleStride $ObjectSampleStride -DispatcherProfileStride 0 `
                    -AllowStarvation -MaxPrivateMemoryMB 1500 -ExecutablePath "$($directories[$version])/AreaUpdateExample.exe" `
                    -OutputDirectory "$OutputDirectory/$name"|Out-Null
                $row=Get-Content "$root/$name/summary.json" -Raw|ConvertFrom-Json
                if($row.processExitCode -ne 0 -or $row.neverUpdated -ne 0){throw "Runtime failed: $name"}
                if($row.taskStatisticsEnabled -and $row.tasksLive -ne 0){throw "Task leak: $name"}
                $row|Add-Member version $version
                $rows+=$row
                $rows|Export-Csv "$root/results.csv" -NoTypeInformation
                "$name FPS=$($row.steadyActorFps) CPU=$($row.steadyCpuPercent) gap=$($row.maxGapMs)"
            }
        }
    }
}
} finally {
    [Environment]::SetEnvironmentVariable('AREAUPDATE_WORKER_DIAGNOSTICS',$savedDiagnostics,'Process')
}
