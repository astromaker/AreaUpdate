param([string]$InputDirectory='artifacts/area-validation')
$repositoryRoot = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)

$ErrorActionPreference='Stop'
$root=Join-Path $repositoryRoot $InputDirectory
$followup=Join-Path $root 'followup'
$phases=@();$locks=@();$memory=@();$interactions=@()
foreach($dir in Get-ChildItem $followup -Directory){
    $summaryPath=Join-Path $dir.FullName 'summary.json'
    if(-not(Test-Path $summaryPath)){continue}
    $s=Get-Content $summaryPath -Raw|ConvertFrom-Json
    if($s.processExitCode -ne 0 -or $s.taskLifetimeVerified -ne $true -or $s.neverUpdated -ne 0){throw "Invalid run $($dir.Name)"}
    $raw=Get-Content (Join-Path $dir.FullName 'runtime.log')
    foreach($line in $raw|Where-Object{$_ -match '^WORKER_PHASE '}){
        $row=[ordered]@{caseName=$dir.Name}
        foreach($match in [regex]::Matches($line,'(\w+)=([^ ]+)')){$row[$match.Groups[1].Value]=[double]$match.Groups[2].Value}
        if($row.timer_ns+$row.request_ns+$row.ticker_ns -gt $row.busy_ns -or $row.blocked_idle_ns -gt $row.idle_ns){throw 'Worker phase bounds'}
        $phases+=[pscustomobject]$row
    }
    $profile=Join-Path $dir.FullName 'area-profile.csv'
    if(Test-Path $profile){
        foreach($group in (Import-Csv $profile|Where-Object{[int]$_.second -ge 6 -and [int]$_.second -lt 20}|Group-Object metric)){
            $n=($group.Group|Measure-Object count -Sum).Sum
            $sum=($group.Group|Measure-Object sum -Sum).Sum
            $locks+=[pscustomobject]@{caseName=$dir.Name;metric=$group.Name;count=$n;mean=if($n){$sum/$n}else{$null};maximum=($group.Group|Measure-Object max -Maximum).Maximum}
        }
    }
    if($dir.Name -like 'stress-*' -or $dir.Name -eq 'memory-no-moves'){
        $samples=Import-Csv (Join-Path $dir.FullName 'samples.csv')
        $first=@($samples|Where-Object{[double]$_.elapsedSeconds -ge 10 -and [double]$_.elapsedSeconds -lt 20})
        $durationLine=$raw|Where-Object{$_ -match '^SUMMARY seconds='}|Select-Object -First 1
        $duration=[double]([regex]::Match($durationLine,'seconds=([\d.]+)').Groups[1].Value)
        # Exclude shutdown/destruction samples from the final live-process window.
        $last=@($samples|Where-Object{[double]$_.elapsedSeconds -ge $duration-20 -and [double]$_.elapsedSeconds -lt $duration-10})
        $a=($first|Measure-Object privateBytes -Average).Average/1MB
        $b=($last|Measure-Object privateBytes -Average).Average/1MB
        $memory+=[pscustomobject]@{caseName=$dir.Name;firstPrivateMB=$a;lastPrivateMB=$b;deltaPrivateMB=$b-$a;steadyFps=$s.steadyActorFps;p99GapBucketMs=$s.p99GapBucketMs;maxGapMs=$s.maxGapMs;cpu=$s.steadyCpuPercent;taskLifetimeVerified=$s.taskLifetimeVerified}
    }
}
foreach($file in Get-ChildItem $followup -Filter '*interaction-*.log'){
    foreach($line in Get-Content $file.FullName|Where-Object{$_ -match '^mode='}){
        $row=[ordered]@{caseName=$file.BaseName}
        foreach($match in [regex]::Matches($line,'(\w+)=([^ ]+)')){$row[$match.Groups[1].Value]=$match.Groups[2].Value}
        if($row.violations -ne '0'){throw 'Interaction overlap detected'}
        $interactions+=[pscustomobject]$row
    }
}
$phases|Export-Csv "$root/worker-phases.csv" -NoTypeInformation
$locks|Export-Csv "$root/profile-means.csv" -NoTypeInformation
$memory|Export-Csv "$root/stress-memory.csv" -NoTypeInformation
$interactions|Export-Csv "$root/interactions.csv" -NoTypeInformation
$memory|Format-Table
