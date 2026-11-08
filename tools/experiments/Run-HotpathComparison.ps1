param(
    [Parameter(Mandatory=$true)][string]$BaselineDirectory,
    [Parameter(Mandatory=$true)][string]$CandidateDirectory,
    [Parameter(Mandatory=$true)][string]$OutputDirectory,
    [ValidateRange(1,3)][int]$Repeats=2
)
$repositoryRoot = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)


$ErrorActionPreference='Stop'
$root=Join-Path $repositoryRoot $OutputDirectory
if(Test-Path $root){throw 'Choose a new output directory'}
New-Item -ItemType Directory $root -Force|Out-Null
$keys=@('AREAUPDATE_RADIUS_PROFILE','AREAUPDATE_RADIUS_BATCH','AREAUPDATE_RADIUS_SORT_TIMERS',
    'AREAUPDATE_RADIUS_TIMER_BATCH','AREAUPDATE_RADIUS_PUBLISH_BATCH','AREAUPDATE_RADIUS_FAST_BUCKET',
    'AREAUPDATE_RADIUS_MERGE_NOTIFY','AREAUPDATE_RADIUS_QUEUE_DIAGNOSTICS','AREAUPDATE_RADIUS_LATENCY_STRIDE')
$saved=@{}
foreach($key in $keys){$saved[$key]=[Environment]::GetEnvironmentVariable($key,'Process')}
$rows=@()
try{
    $env:AREAUPDATE_RADIUS_PROFILE='0'
    $env:AREAUPDATE_RADIUS_BATCH='64'
    $env:AREAUPDATE_RADIUS_SORT_TIMERS='1'
    $env:AREAUPDATE_RADIUS_TIMER_BATCH='1'
    $env:AREAUPDATE_RADIUS_PUBLISH_BATCH='1'
    $env:AREAUPDATE_RADIUS_FAST_BUCKET='1'
    $env:AREAUPDATE_RADIUS_MERGE_NOTIFY='1'
    $env:AREAUPDATE_RADIUS_QUEUE_DIAGNOSTICS='0'
    $env:AREAUPDATE_RADIUS_LATENCY_STRIDE='0'
    $directories=@{baseline=$BaselineDirectory;candidate=$CandidateDirectory}
    $hashes=@()
    foreach($version in @('baseline','candidate')){
        foreach($name in @('AreaUpdateExample','UpdateComparison')){
            $hashes+=Get-FileHash (Join-Path $repositoryRoot "$($directories[$version])/$name.exe")
        }
        foreach($layout in @('uniform','boundary')){
            & (Join-Path $repositoryRoot "$($directories[$version])/UpdateComparison.exe") --motion-probe legacy 3 5000 42 verify $layout none 64 0 0 0 *> "$root/verify-$version-$layout.log"
            if($LASTEXITCODE -ne 0){throw "Motion regression: $version $layout"}
        }
    }
    $hashes|Export-Csv "$root/binaries.csv" -NoTypeInformation
    $cases=@()
    for($repeat=0;$repeat -lt $Repeats;++$repeat){
        foreach($seed in @(42,43)){
            $order=if(($repeat+($seed-42))%2 -eq 0){@('baseline','candidate')}else{@('candidate','baseline')}
            foreach($version in $order){$cases+=@{repeat=$repeat;seed=$seed;actors=280000;version=$version}}
        }
    }
    foreach($version in @('baseline','candidate')){$cases+=@{repeat=0;seed=42;actors=70000;version=$version}}
    foreach($case in $cases){
        $name="$($case.actors)-$($case.seed)-$($case.repeat)-$($case.version)"
        $exe="$($directories[$case.version])/AreaUpdateExample.exe"
        & "$repositoryRoot/tools/benchmark/Measure-RuntimeCpu.ps1" -Seconds 20 -Actors $case.actors -MapSide 640 -MovesPerSecond 8000 `
            -Seed $case.seed -JoinDurationMs 1000 -JoinIntervalMs 1 -Mode legacy -TimerTraceThresholdMs 20 -AllowStarvation `
            -ExecutablePath $exe -OutputDirectory "$OutputDirectory/$name"|Out-Null
        $row=Get-Content "$root/$name/summary.json" -Raw|ConvertFrom-Json
        if($row.processExitCode -ne 0 -or $row.neverUpdated -ne 0 -or $row.tasksLive -ne 0 -or
            $row.taskStatisticsEnabled -ne $true){throw "Runtime invariant: $name"}
        $row|Add-Member caseName $name
        $row|Add-Member version $case.version
        $row|Add-Member repeat $case.repeat
        $timer=Get-Content "$root/$name/runtime.log"|Where-Object{$_ -like 'RADIUS_TIMERS *'}|Select-Object -Last 1
        foreach($match in [regex]::Matches($timer,'(\w+)=([^ ]+)')){$row|Add-Member "timer_$($match.Groups[1].Value)" $match.Groups[2].Value}
        $rows+=$row
        $rows|Export-Csv "$root/results.csv" -NoTypeInformation
        "$name FPS=$($row.steadyActorFps) CPU=$($row.steadyCpuPercent) maxGap=$($row.maxGapMs) minFPS=$($row.steadyMinActorFps)"
    }
}finally{
    foreach($key in $saved.Keys){[Environment]::SetEnvironmentVariable($key,$saved[$key],'Process')}
}
