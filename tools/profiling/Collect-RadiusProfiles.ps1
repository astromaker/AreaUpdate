param([string]$OutputDirectory='artifacts/radius-profiles-admin')
$repositoryRoot = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)


$ErrorActionPreference='Stop'
$principal=[Security.Principal.WindowsPrincipal]::new([Security.Principal.WindowsIdentity]::GetCurrent())
if(-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)){
    throw 'Run this collector in Administrator PowerShell. No recording was started.'
}
$root=Join-Path $repositoryRoot $OutputDirectory
if(Test-Path $root){throw 'Choose a new profile output directory'}
$keys=@('AREAUPDATE_RADIUS_PROFILE','AREAUPDATE_RADIUS_BATCH','AREAUPDATE_RADIUS_SORT_TIMERS',
    'AREAUPDATE_RADIUS_TIMER_BATCH','AREAUPDATE_RADIUS_PUBLISH_BATCH','AREAUPDATE_RADIUS_FAST_BUCKET',
    'AREAUPDATE_RADIUS_MERGE_NOTIFY','AREAUPDATE_RADIUS_QUEUE_DIAGNOSTICS','AREAUPDATE_RADIUS_LATENCY_STRIDE')
$saved=@{}
foreach($key in $keys){$saved[$key]=[Environment]::GetEnvironmentVariable($key,'Process')}
try{
    $env:AREAUPDATE_RADIUS_PROFILE='0'
    $env:AREAUPDATE_RADIUS_BATCH='64'
    $env:AREAUPDATE_RADIUS_SORT_TIMERS='1'
    $env:AREAUPDATE_RADIUS_TIMER_BATCH='1'
    $env:AREAUPDATE_RADIUS_PUBLISH_BATCH='1'
    $env:AREAUPDATE_RADIUS_FAST_BUCKET='1'
    $env:AREAUPDATE_RADIUS_MERGE_NOTIFY='1'
    foreach($case in @('capacity','hol','latency')){
        $env:AREAUPDATE_RADIUS_QUEUE_DIAGNOSTICS=if($case -eq 'hol'){'1'}else{'0'}
        $env:AREAUPDATE_RADIUS_LATENCY_STRIDE=if($case -eq 'hol'){'4096'}else{'0'}
        $actors=if($case -eq 'latency'){5000}else{280000}
        $side=if($case -eq 'latency'){64}else{640}
        $moves=if($case -eq 'latency'){2000}else{8000}
        $seconds=if($case -eq 'latency'){30}else{10}
        $timerTrace=if($case -eq 'capacity'){0}else{10}
        & "$repositoryRoot/tools/profiling/Collect-AreaProfiles.ps1" -Profile CPU -Mode legacy -CpuActors $actors -CpuMapSide $side `
            -CpuMovesPerSecond $moves -CpuSeconds $seconds -TimerTraceThresholdMs $timerTrace `
            -CpuProfile "$repositoryRoot/tools/profiling/RadiusCpu.wprp!RadiusCpu" -OutputDirectory "$OutputDirectory/$case"
        & "$repositoryRoot/tools/profiling/Export-RadiusProfile.ps1" -InputDirectory "$OutputDirectory/$case" -OutputDirectory "$OutputDirectory/$case/export"
    }
}finally{
    foreach($key in $keys){[Environment]::SetEnvironmentVariable($key,$saved[$key],'Process')}
}
