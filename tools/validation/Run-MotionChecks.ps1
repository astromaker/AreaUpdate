param([ValidateSet('verify','synthetic')][string]$Stage='verify', [string]$OutputDirectory='artifacts/motion-checks')
$repositoryRoot = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)

$ErrorActionPreference='Stop'
$root=Join-Path $repositoryRoot "$OutputDirectory/$Stage"
if(Test-Path $root){throw 'Choose a new output directory'}
New-Item -ItemType Directory $root -Force|Out-Null
Copy-Item "$repositoryRoot/Source/x64/Release/UpdateComparison.exe" "$root/UpdateComparison.exe"
$rows=@()
foreach($actors in $(if($Stage -eq 'verify'){@(5000)}else{@(5000,50000)})){
 foreach($layout in @('uniform','hotspot')){
  foreach($seed in @(42,43)){
   $modes=if($seed -eq 42){@('area','job')}else{@('job','area')}
   foreach($mode in $modes){
    $seconds=if($Stage -eq 'verify'){10}else{15}
    $name="$actors-$layout-$seed-$mode"
    $log="$root/$name.log"
    $p=Start-Process -FilePath "$root/UpdateComparison.exe" -ArgumentList @('--motion-probe',$mode,$seconds,$actors,$seed,$Stage,$layout) -WindowStyle Hidden -PassThru -RedirectStandardOutput $log -RedirectStandardError "$log.stderr"
    if(-not $p.WaitForExit(($seconds+45)*1000)){$p.Kill();$p.WaitForExit();throw 'Motion watchdog'}
    if($p.ExitCode -ne 0){throw "Motion test failed $name"}
    $line=Get-Content $log|Where-Object{$_ -match '^MOTION '}|Select-Object -Last 1
    if(-not $line){throw 'Missing motion summary'}
    $row=[ordered]@{caseName=$name}
    foreach($match in [regex]::Matches($line,'(\w+)=([^ ]+)')){$row[$match.Groups[1].Value]=$match.Groups[2].Value}
    if($row.sent -ne $row.applied -or $row.overlaps -ne '0' -or $row.tasks_live -ne '0'){throw 'Motion invariant failed'}
    $row.cpu_percent=100*[double]$row.cpu_seconds/[double]$row.seconds/[Environment]::ProcessorCount
    $rows+=[pscustomobject]$row
    $rows|Export-Csv "$root/results.csv" -NoTypeInformation
    '{0}: FPS={1} CPU={2:F2}% moves={3} events={4} ownership_cycle={5}' -f $name,$row.steady_fps,$row.cpu_percent,$row.moves,$row.sent,$row.ownership_cycle
   }
  }
 }
}
if($Stage -eq 'verify'){
 $log="$root/fault-event-id.log"
 $p=Start-Process -FilePath "$root/UpdateComparison.exe" -ArgumentList @('--motion-probe','area',2,64,42,'verify','uniform','event-id') -WindowStyle Hidden -PassThru -RedirectStandardOutput $log -RedirectStandardError "$log.stderr"
 if(-not $p.WaitForExit(30000)){$p.Kill();$p.WaitForExit();throw 'Fault test watchdog'}
 if($p.ExitCode -eq 0 -or (Get-Content "$log.stderr" -Raw) -notmatch 'Event missing, duplicate or misdirected'){throw 'Verifier failed to detect injected event corruption'}
 'Injected event corruption was correctly rejected'
}
