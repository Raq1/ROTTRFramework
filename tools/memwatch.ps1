# Memory / VRAM monitor for ROTTR rendering glitches (missing floors, black meshes).
# Read-only: it only samples performance counters. Run it BEFORE starting the game:
#   powershell -ExecutionPolicy Bypass -File G:\ROTTR_Modding\tools\memwatch.ps1
# It waits for the process, logs one CSV line every few seconds and stops when the game exits.
# When a glitch appears in game, note the clock time; every line carries HH:mm:ss.
param(
    [string]$ProcessName = 'ROTTR',
    [int]$IntervalSeconds = 2,
    [int]$MaxSamples = 0,
    [string]$OutDir = (Join-Path $PSScriptRoot 'memwatch_logs')
)

$ErrorActionPreference = 'SilentlyContinue'
New-Item -ItemType Directory -Force $OutDir | Out-Null

# Dedicated VRAM size of the biggest adapter (RTX 2070 = 8 GB)
$vramTotalMb = 0
Get-ChildItem 'HKLM:\SYSTEM\CurrentControlSet\Control\Class\{4d36e968-e325-11ce-bfc1-08002be10318}' |
    ForEach-Object {
        $q = (Get-ItemProperty $_.PSPath).'HardwareInformation.qwMemorySize'
        if ($q -and ($q / 1MB) -gt $vramTotalMb) { $vramTotalMb = [int]($q / 1MB) }
    }

Write-Host "memwatch: waiting for $ProcessName.exe ... (VRAM total $vramTotalMb MB)"
$proc = $null
while (-not $proc) {
    $proc = Get-Process -Name $ProcessName | Select-Object -First 1
    if (-not $proc) { Start-Sleep -Seconds 1 }
}
$gamePid = $proc.Id
$stamp = Get-Date -Format 'yyyy-MM-dd_HHmmss'
$outFile = Join-Path $OutDir "memwatch_$stamp.csv"
'time,availRamMB,commitUsedGB,commitLimitGB,commitPct,gamePrivateGB,gameWorkingSetGB,gameVramMB,gameSharedGpuMB,adapterVramUsedMB,vramTotalMB,othersVramMB,topOther1,topOther2,topOther3,flags' |
    Out-File -FilePath $outFile -Encoding utf8
Write-Host "memwatch: $ProcessName pid $gamePid found, logging to $outFile"

$samples = 0
while ($true) {
    $proc = Get-Process -Id $gamePid
    if (-not $proc) { break }

    $paths = @(
        '\Memory\Available MBytes',
        '\Memory\Committed Bytes',
        '\Memory\Commit Limit',
        '\GPU Adapter Memory(*)\Dedicated Usage',
        '\GPU Process Memory(*)\Dedicated Usage',
        '\GPU Process Memory(*)\Shared Usage'
    )
    $c = (Get-Counter -Counter $paths -SampleInterval 1 -MaxSamples 1).CounterSamples

    $avail = 0; $commit = 0; $limit = 0; $adapter = 0
    $gameVram = 0; $gameShared = 0; $othersVram = 0
    foreach ($s in $c) {
        $p = $s.Path.ToLowerInvariant()
        if ($p -like '*\memory\available mbytes') { $avail = $s.CookedValue }
        elseif ($p -like '*\memory\committed bytes') { $commit = $s.CookedValue }
        elseif ($p -like '*\memory\commit limit') { $limit = $s.CookedValue }
        elseif ($p -like '*gpu adapter memory*dedicated usage') {
            if ($s.CookedValue -gt $adapter) { $adapter = $s.CookedValue }
        }
        elseif ($p -like '*gpu process memory*') {
            $isGame = $p -like "*(pid_$($gamePid)_*"
            if ($p -like '*dedicated usage') {
                if ($isGame) { $gameVram += $s.CookedValue } else { $othersVram += $s.CookedValue }
            } elseif ($isGame) { $gameShared += $s.CookedValue }
        }
    }

    $others = Get-Process | Where-Object { $_.Id -ne $gamePid } |
        Sort-Object PrivateMemorySize64 -Descending | Select-Object -First 3 |
        ForEach-Object { [string]::Format([System.Globalization.CultureInfo]::InvariantCulture, '{0}:{1:F1}GB', $_.Name, ($_.PrivateMemorySize64 / 1GB)) }
    while ($others.Count -lt 3) { $others = @($others) + '' }

    $pct = if ($limit -gt 0) { 100.0 * $commit / $limit } else { 0 }
    $flags = @()
    if ($avail -lt 1024) { $flags += 'LOW_RAM' }
    if ($pct -ge 90) { $flags += 'COMMIT_90' }
    if ($vramTotalMb -gt 0 -and ($adapter / 1MB) -ge ($vramTotalMb * 0.92)) { $flags += 'VRAM_FULL' }
    if (($gameShared / 1MB) -ge 512) { $flags += 'VRAM_SPILL' }

    $inv = [System.Globalization.CultureInfo]::InvariantCulture
    $line = [string]::Format($inv,
        '{0},{1:F0},{2:F1},{3:F1},{4:F0},{5:F2},{6:F2},{7:F0},{8:F0},{9:F0},{10},{11:F0},{12},{13},{14},{15}',
        (Get-Date -Format 'HH:mm:ss'), $avail, ($commit / 1GB), ($limit / 1GB), $pct,
        ($proc.PrivateMemorySize64 / 1GB), ($proc.WorkingSet64 / 1GB),
        ($gameVram / 1MB), ($gameShared / 1MB), ($adapter / 1MB), $vramTotalMb, ($othersVram / 1MB),
        $others[0], $others[1], $others[2], ($flags -join '|'))
    Add-Content -Path $outFile -Value $line -Encoding utf8
    if ($flags.Count -gt 0) { Write-Host $line -ForegroundColor Yellow } else { Write-Host $line }

    $samples++
    if ($MaxSamples -gt 0 -and $samples -ge $MaxSamples) { break }
    if ($IntervalSeconds -gt 1) { Start-Sleep -Seconds ($IntervalSeconds - 1) }
}
Write-Host "memwatch: finished, $samples samples in $outFile"
