# Fetches timed .lrc lyrics for FlatOut 2's 34 soundtrack tracks (24 unique songs) from LRCLIB
# using pinned record IDs (with search fallback) for deterministic, reproducible output.
#
# Usage:
#   powershell -ExecutionPolicy Bypass -File tools\fetch_lyrics.ps1 [-OutDir <scripts\lyrics>] [-PlainLineTimings] [-Force]
param(
    [string]$OutDir,
    [switch]$PlainLineTimings,
    [switch]$Force
)
$ErrorActionPreference = "Stop"
[Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12
$inv = [System.Globalization.CultureInfo]::InvariantCulture

$scriptDir = $PSScriptRoot
$parent = (Resolve-Path (Join-Path $scriptDir "..")).Path
$grandParent = Split-Path $parent
if (-not $OutDir) {
    if ((Test-Path (Join-Path $scriptDir "FO2Lyrics.asi")) -or (Test-Path (Join-Path $scriptDir "FO2Lyrics.ini")) -or
        (Test-Path (Join-Path $scriptDir "FO2VR.asi")) -or (Test-Path (Join-Path $scriptDir "FO2VR.ini"))) {
        $OutDir = Join-Path $scriptDir "lyrics"
    } elseif ((Test-Path (Join-Path $parent "FlatOut2.exe")) -or (Test-Path (Join-Path $parent "scripts\FO2Lyrics.asi")) -or
              (Test-Path (Join-Path $parent "scripts\FO2VR.asi"))) {
        $OutDir = Join-Path $parent "scripts\lyrics"
    } elseif ($grandParent -and (Test-Path (Join-Path $grandParent "scripts"))) {
        $OutDir = Join-Path $grandParent "scripts\lyrics"
    } else {
        $OutDir = Join-Path $parent "build\lyrics"
    }
}
New-Item -ItemType Directory -Force -Path $OutDir | Out-Null

# All 34 soundtrack files across data/songs1/*.ogg and data/songs2/*.ogg (12 and 23 do not exist in the game),
# mapped to their exact LRCLIB record IDs and playlist metadata.
$tracks = @(
    @{ Files = @("01");       Id = 2472115; Artist = "The Chelsea Smiles"; Title = "Nowhere Ride";                 QueryTitle = "Nowhere Ride" }
    @{ Files = @("02");       Id = 56755;   Artist = "Alkaline Trio";      Title = "Fall Victim";                  QueryTitle = "Fall Victim" }
    @{ Files = @("03", "25"); Id = 5617863; Artist = "Papa Roach";         Title = "Blood Brothers";               QueryTitle = "Blood Brothers" }
    @{ Files = @("04");       Id = 2472714; Artist = "Supergrass";         Title = "Road To Rouen";                QueryTitle = "Road To Rouen" }
    @{ Files = @("05", "26"); Id = 3433974; Artist = "Alkaline Trio";      Title = "Mercy Me";                     QueryTitle = "Mercy Me" }
    @{ Files = @("06");       Id = 8007107; Artist = "Nickelback";         Title = "Believe It or Not";            QueryTitle = "Believe It or Not" }
    @{ Files = @("07");       Id = 7183014; Artist = "Megadeth";           Title = "Symphony of Destruction";      QueryTitle = "Symphony of Destruction" }
    @{ Files = @("08");       Id = 3868244; Artist = "Yellowcard";         Title = "Rough Landing Holly";          QueryTitle = "Rough Landing Holly" }
    @{ Files = @("09");       Id = 5817875; Artist = "Yellowcard";         Title = "Breathing";                    QueryTitle = "Breathing" }
    @{ Files = @("10", "27"); Id = 3894681; Artist = "Zebrahead";          Title = "Lobotomy for Dummies";         QueryTitle = "Lobotomy for Dummies" }
    @{ Files = @("11");       Id = 7607404; Artist = "Rise Against";       Title = "Give It All";                  QueryTitle = "Give It All" }
    @{ Files = @("13", "28"); Id = 7739315; Artist = "Papa Roach";         Title = "Not Listening";                QueryTitle = "Not Listening" }
    @{ Files = @("14", "29"); Id = 4751673; Artist = "Underoath";          Title = "Reinventing Your Exit";        QueryTitle = "Reinventing Your Exit" }
    @{ Files = @("15", "30"); Id = 2468210; Artist = "Fall Out Boy";       Title = "7 Minutes In Heaven";          QueryTitle = "7 Minutes In Heaven" }
    @{ Files = @("16", "31"); Id = 5420449; Artist = "Supergrass";         Title = "Richard III";                  QueryTitle = "Richard III" }
    @{ Files = @("17", "32"); Id = 8007382; Artist = "Nickelback";         Title = "Flat On the Floor";            QueryTitle = "Flat On the Floor" }
    @{ Files = @("18", "33"); Id = 7776443; Artist = "Rob Zombie";         Title = "Feel So Numb";                 QueryTitle = "Feel So Numb" }
    @{ Files = @("19");       Id = 3793654; Artist = "Rob Zombie";         Title = "Demon Speeding";               QueryTitle = "Demon Speeding" }
    @{ Files = @("20");       Id = 2520349; Artist = "Wolfmother";         Title = "Pyramid";                      QueryTitle = "Pyramid" }
    @{ Files = @("21", "34"); Id = 5876764; Artist = "Wolfmother";         Title = "Dimension";                    QueryTitle = "Dimension" }
    @{ Files = @("22");       Id = 3846678; Artist = "Fall Out Boy";       Title = "Snitches, and Talkers Get..."; QueryTitle = "Snitches and Talkers Get Stitches and Walkers" }
    @{ Files = @("24");       Id = 2469681; Artist = "The Vines";          Title = "Don't Listen To The Radio";    QueryTitle = "Don't Listen To The Radio" }
    @{ Files = @("35");       Id = 2837104; Artist = "Audioslave";         Title = "Man Or Animal";                QueryTitle = "Man Or Animal" }
    @{ Files = @("36");       Id = 3815807; Artist = "Audioslave";         Title = "Your Time Has Come";           QueryTitle = "Your Time Has Come" }
)

$headers = @{ "User-Agent" = "flatout2-vr-lyrics-fetcher/1.0 (https://github.com/Saikava/flatout2-vr)" }

function Format-LrcTime([double]$sec) {
    if ($sec -lt 0) { $sec = 0.0 }
    $totalCs = [int][Math]::Round($sec * 100.0)
    $mm = [int][Math]::Floor($totalCs / 6000)
    $rem = $totalCs % 6000
    $ss = [int][Math]::Floor($rem / 100)
    $cs = $rem % 100
    return [string]::Format($inv, "{0:D2}:{1:D2}.{2:D2}", $mm, $ss, $cs)
}

function ConvertTo-LrcContent([string]$artist, [string]$title, [string]$synced, [double]$duration, [bool]$plainOnly) {
    $parsed = New-Object System.Collections.Generic.List[object]
    foreach ($rawLine in ($synced -replace "`r`n", "`n" -replace "`r", "`n" -split "`n")) {
        if ($rawLine -match '^\[(\d+):(\d+(?:\.\d+)?)\](.*)$') {
            $mm = [double]::Parse($Matches[1], $inv)
            $ss = [double]::Parse($Matches[2], $inv)
            $parsed.Add([PSCustomObject]@{
                At   = $mm * 60.0 + $ss
                Text = $Matches[3].Trim()
            })
        }
    }

    $out = New-Object System.Collections.Generic.List[string]
    $out.Add("[ti:$title]")
    $out.Add("[ar:$artist]")

    for ($i = 0; $i -lt $parsed.Count; $i++) {
        $at = $parsed[$i].At
        $text = $parsed[$i].Text
        $lineTag = "[" + (Format-LrcTime $at) + "]"

        if ([string]::IsNullOrWhiteSpace($text)) {
            $out.Add("$lineTag ")
            continue
        }
        if ($plainOnly -or $text.Contains("<")) {
            $out.Add("$lineTag$text")
            continue
        }

        $words = $text -split '\s+'
        if ($words.Count -le 1) {
            $tTag = Format-LrcTime $at
            $out.Add("$lineTag<$tTag>$text")
            continue
        }

        $nextAt = if ($i + 1 -lt $parsed.Count) {
            $parsed[$i + 1].At
        } elseif ($duration -gt $at) {
            $duration
        } else {
            $at + 3.5
        }
        $delta = [Math]::Max($nextAt - $at, 0.5)
        $span = [Math]::Min($delta * 0.85, 4.5)

        $totalChars = 0
        foreach ($w in $words) { $totalChars += [Math]::Max($w.Length, 1) }

        $accChars = 0
        $parts = New-Object System.Collections.Generic.List[string]
        for ($wIdx = 0; $wIdx -lt $words.Count; $wIdx++) {
            $wAt = $at + ($span * $accChars / $totalChars)
            $wTag = Format-LrcTime $wAt
            $parts.Add("<$wTag>$($words[$wIdx])")
            $accChars += [Math]::Max($words[$wIdx].Length, 1)
        }
        $out.Add($lineTag + ($parts -join " "))
    }

    return ($out -join "`n") + "`n"
}

function Invoke-LrcLib([string]$uri) {
    $delays = @(2, 5, 10, 20, 35)
    for ($attempt = 0; ; $attempt++) {
        try {
            return Invoke-RestMethod -Uri $uri -Headers $headers -Method Get -TimeoutSec 20
        } catch {
            $status = $null
            $retryAfter = 0
            if ($_.Exception.Response) {
                $status = [int]$_.Exception.Response.StatusCode
                $ra = $_.Exception.Response.Headers["Retry-After"]
                if ($ra) { [void][int]::TryParse($ra, [ref]$retryAfter) }
            }
            $transient = ($status -eq 429) -or ($status -ge 500) -or ($null -eq $status)
            if (-not $transient -or $attempt -ge $delays.Count) { throw }
            $wait = if ($retryAfter -gt 0) { $retryAfter } else { $delays[$attempt] }
            Write-Host "Rate-limited or transient HTTP $status on $uri; retrying in ${wait}s..."
            Start-Sleep -Seconds $wait
        }
    }
}

function Fetch-Record([hashtable]$t) {
    try {
        $rec = Invoke-LrcLib "https://lrclib.net/api/get/$($t.Id)"
        if ($rec -and $rec.syncedLyrics) { return $rec }
    } catch {
        Write-Warning "Pinned ID $($t.Id) lookup failed for '$($t.Artist) - $($t.Title)', trying search..."
    }
    $ar = [uri]::EscapeDataString($t.Artist)
    $ti = [uri]::EscapeDataString($t.QueryTitle)
    $results = Invoke-LrcLib "https://lrclib.net/api/search?artist_name=$ar&track_name=$ti"
    foreach ($r in $results) {
        if ($r.syncedLyrics) { return $r }
    }
    throw "No synced lyrics found on LRCLIB for '$($t.Artist) - $($t.Title)'"
}

$utf8NoBom = New-Object System.Text.UTF8Encoding($false)
$written = 0
$skipped = 0

foreach ($t in $tracks) {
    $needFetch = $Force.IsPresent
    if (-not $needFetch) {
        foreach ($f in $t.Files) {
            if (-not (Test-Path (Join-Path $OutDir "$f.lrc"))) { $needFetch = $true; break }
        }
    }
    if (-not $needFetch) {
        $skipped += $t.Files.Count
        continue
    }

    $rec = Fetch-Record $t
    $dur = if ($null -ne $rec.duration) { [double]$rec.duration } else { 0.0 }
    $content = ConvertTo-LrcContent -artist $t.Artist -title $t.Title -synced ([string]$rec.syncedLyrics) -duration $dur -plainOnly $PlainLineTimings.IsPresent

    foreach ($f in $t.Files) {
        $dest = Join-Path $OutDir "$f.lrc"
        [System.IO.File]::WriteAllText($dest, $content, $utf8NoBom)
        $written++
    }
    Write-Host ("[{0}] {1} - {2} (LRCLIB #{3})" -f ($t.Files -join ","), $t.Artist, $t.Title, $rec.id)
    Start-Sleep -Milliseconds 150
}

Write-Host "Done: $written written, $skipped already up to date in $OutDir"
