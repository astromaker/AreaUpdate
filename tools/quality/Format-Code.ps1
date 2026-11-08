param(
    [string[]]$Paths = @('Source/AreaUpdate/Area.cpp'),
    [switch]$All,
    [switch]$Check
)
$repositoryRoot = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)


$ErrorActionPreference = 'Stop'
$root = $repositoryRoot
$formatter = Get-Command clang-format -ErrorAction Stop
$version = & $formatter.Source --version
if ($LASTEXITCODE -ne 0 -or $version -notmatch 'version\s+(\d+)' -or [int]$Matches[1] -lt 18) {
    throw 'clang-format 18 or newer is required; LLVM 22 is verified for this repository.'
}

if ($All) {
    $candidates = @(Get-ChildItem -LiteralPath (Join-Path $root 'Source') -Recurse -File |
        Where-Object { $_.Extension -in @('.h', '.hpp', '.cpp', '.cxx') })
}
else {
    $candidates = @($Paths | ForEach-Object {
        $path = if ([IO.Path]::IsPathRooted($_)) { $_ } else { Join-Path $root $_ }
        Get-Item -LiteralPath $path -ErrorAction Stop
    })
}

$files = @($candidates | Where-Object {
    $relative = [IO.Path]::GetRelativePath($root, $_.FullName).Replace('\', '/')
    if ($relative.StartsWith('../') -or [IO.Path]::IsPathRooted($relative)) {
        throw "Path is outside the repository: $($_.FullName)"
    }
    $_.Extension -in @('.h', '.hpp', '.cpp', '.cxx') -and
        $relative -like 'Source/*' -and
        $relative -notlike 'Source/AreaUpdateLegacy/Baseline/*' -and
        $relative -notlike 'Source/AreaUpdateLegacy/OriginalAreaAdapter.*' -and
        $relative -notmatch '/(x64|x86|Debug|Release)/'
} | Sort-Object FullName -Unique)

if (!$files.Count) { throw 'No project-owned C++ files selected' }
foreach ($file in $files) {
    $arguments = @('--style=file', '--fallback-style=none')
    if ($Check) { $arguments += @('--dry-run', '--Werror') }
    else { $arguments += '-i' }
    & $formatter.Source @arguments $file.FullName
    if ($LASTEXITCODE -ne 0) { throw "Formatting failed: $($file.FullName)" }
}
Write-Output "Formatting $(if ($Check) { 'check passed' } else { 'applied' }): $($files.Count) file(s)"
