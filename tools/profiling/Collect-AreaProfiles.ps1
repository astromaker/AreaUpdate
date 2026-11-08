param(
    [string]$OutputDirectory='artifacts/motion-profiles',
    [ValidateSet('CPU','Heap','all')][string]$Profile='all',
    [ValidateSet('area','legacy')][string]$Mode='area',
    [ValidateRange(1,1000000)][int]$CpuActors=280000,
    [ValidateRange(16,2048)][int]$CpuMapSide=640,
    [ValidateRange(0,1000000)][int]$CpuMovesPerSecond=8000,
    [ValidateRange(3,60)][int]$CpuSeconds=10,
    [ValidateRange(0,1000)][int]$TimerTraceThresholdMs=0,
    [string]$CpuProfile='CPU'
)
$repositoryRoot = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)

$ErrorActionPreference='Stop'
$principal=[Security.Principal.WindowsPrincipal]::new([Security.Principal.WindowsIdentity]::GetCurrent())
if(-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)){
    throw 'Administrator PowerShell is required for WPR collection. No recording was started.'
}
$root=Join-Path $repositoryRoot $OutputDirectory
if(Test-Path $root){throw 'Choose a new profile output directory'}
New-Item -ItemType Directory "$root/bin","$root/temp" -Force|Out-Null
$sourceExe=Join-Path $repositoryRoot 'Source/x64/Release/AreaUpdateExample.exe'
Copy-Item $sourceExe "$root/bin/AreaCpuProbe.exe"
Copy-Item $sourceExe "$root/bin/AreaHeapProbe.exe"
Copy-Item (Join-Path $repositoryRoot 'Source/x64/Release/AreaUpdateExample.pdb') "$root/bin/AreaUpdateExample.pdb"
if($Profile -ne 'Heap' -and $CpuProfile.Contains('!')){
    $profileFile=$CpuProfile.Substring(0,$CpuProfile.IndexOf('!'))
    Copy-Item -LiteralPath $profileFile -Destination "$root/profile-definition.wprp"
}
Get-ChildItem "$root/bin" -File | Get-FileHash | Export-Csv "$root/binaries.csv" -NoTypeInformation
$wpr=Join-Path $env:windir 'system32/wpr.exe'
function Invoke-Wpr([string[]]$Options){
    $output=& $wpr @Options 2>&1
    $output|Add-Content "$root/wpr.log"
    if($LASTEXITCODE -ne 0){throw "WPR failed: $Options`n$output"}
    return $output
}
$kinds=if($Profile -eq 'all'){@('CPU','Heap')}else{@($Profile)}
foreach($kind in $kinds){
    $instance='AreaProbe'+[guid]::NewGuid().ToString('N')
    $recording=$false;$enabled=$false;$process=$null
    try{
        $image=if($kind -eq 'CPU'){'AreaCpuProbe.exe'}else{'AreaHeapProbe.exe'}
        if($kind -eq 'Heap'){
            $state=Invoke-Wpr -Options @('-HeapTracingConfig',$image)
            if(($state -join ' ') -notmatch 'disabled'){throw 'Existing heap settings must not be overwritten'}
            $enabled=$true
            Invoke-Wpr -Options @('-HeapTracingConfig',$image,'enable')|Out-Null
            Invoke-Wpr -Options @('-start','Heap','-filemode','-recordtempto',"$root/temp",'-instancename',$instance)|Out-Null
            $recording=$true
        }
        $actors=if($kind -eq 'CPU'){$CpuActors}else{5000}
        $side=if($kind -eq 'CPU'){$CpuMapSide}else{320}
        $moves=if($kind -eq 'CPU'){$CpuMovesPerSecond}else{2000}
        $targetSeconds=if($kind -eq 'CPU'){[Math]::Max(25,$CpuSeconds+15)}else{25}
        $process=Start-Process -FilePath "$root/bin/$image" -ArgumentList @($targetSeconds,$actors,$moves,42,1000,1,0,$side,$Mode,$TimerTraceThresholdMs,'signal',0,0,0,'adaptive',1,10,16) -WindowStyle Hidden -PassThru -RedirectStandardOutput "$root/$kind-runtime.log" -RedirectStandardError "$root/$kind-runtime.stderr"
        # Keep a handle while WPR merges the trace, even if the target exits meanwhile.
        $null=$process.Handle
        if($kind -eq 'CPU'){
            Start-Sleep -Seconds 6
            if($process.HasExited){throw 'Target exited before CPU recording'}
            Invoke-Wpr -Options @('-start',$CpuProfile,'-filemode','-recordtempto',"$root/temp",'-instancename',$instance)|Out-Null
            $recording=$true
        }
        $duration=if($kind -eq 'CPU'){$CpuSeconds}else{3}
        Start-Sleep -Seconds $duration
        Invoke-Wpr -Options @('-stop',"$root/$kind.etl",'-instancename',$instance)|Out-Null
        $recording=$false
        if(-not $process.WaitForExit(45000)){$process.Kill();$process.WaitForExit();throw 'Profile target watchdog'}
        $process.WaitForExit()
        $exitCode=$process.ExitCode
        if($null -eq $exitCode){throw "Profile target exit code unavailable: $kind; see $root/$kind-runtime.log"}
        if($exitCode -ne 0){throw "Profile target validation failed: $kind exit_code=$exitCode; see $root/$kind-runtime.log and $root/$kind-runtime.stderr"}
        $log=Get-Content "$root/$kind-runtime.log" -Raw
        if($log -notmatch 'task_statistics=1' -or $log -notmatch 'never_updated=0' -or $log -notmatch 'tasks_live=0'){
            throw "Profile target invariants unavailable or failed: $kind"
        }
        $settings=[ordered]@{}
        foreach($name in @('AREAUPDATE_RADIUS_PROFILE','AREAUPDATE_RADIUS_BATCH','AREAUPDATE_RADIUS_SORT_TIMERS',
            'AREAUPDATE_RADIUS_TIMER_BATCH','AREAUPDATE_RADIUS_PUBLISH_BATCH','AREAUPDATE_RADIUS_FAST_BUCKET',
            'AREAUPDATE_RADIUS_MERGE_NOTIFY','AREAUPDATE_RADIUS_QUEUE_DIAGNOSTICS','AREAUPDATE_RADIUS_LATENCY_STRIDE')){
            $settings[$name]=[Environment]::GetEnvironmentVariable($name,'Process')
        }
        [pscustomobject]@{kind=$kind;pid=$process.Id;image=$image;mode=$Mode;actors=$actors;mapSide=$side;
            movesPerSecond=$moves;durationSeconds=$duration;targetSeconds=$targetSeconds;timerTraceMs=$TimerTraceThresholdMs;
            cpuProfile=$CpuProfile;settings=$settings;etlBytes=(Get-Item "$root/$kind.etl").Length}|ConvertTo-Json -Depth 5|Set-Content "$root/$kind.json"
        "$kind trace completed"
    }
    finally{
        try{
            if($recording){Invoke-Wpr -Options @('-cancel','-instancename',$instance)|Out-Null}
        }
        finally{
            try{
                if($null -ne $process -and -not $process.HasExited){$process.Kill();$process.WaitForExit()}
            }
            finally{
                if($enabled){Invoke-Wpr -Options @('-HeapTracingConfig','AreaHeapProbe.exe','disable')|Out-Null}
            }
        }
    }
}
