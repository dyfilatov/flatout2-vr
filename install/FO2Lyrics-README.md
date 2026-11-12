# FlatOut 2 Karaoke Lyrics (`FO2Lyrics.asi`)

Shows timed lyrics for FlatOut 2's soundtrack in the menus, in races, and in stunt events.

Karaoke also works together with the [FlatOut 2 VR](https://github.com/Saikava/flatout2-vr) mod.

![Karaoke lyrics during a race](https://github.com/dyfilatov/flatout2-lyrics/raw/main/docs/ingame.jpg)

## Quick start

1. Extract `FlatOut2-Lyrics-<version>.zip` into the FlatOut 2 folder (the one that contains `FlatOut2.exe`). If `winmm.dll` is already there from another ASI mod, keep it.
2. Fetch timed `.lrc` lyrics for the 24 full soundtrack tracks into `scripts\lyrics\` (stunt copies `25`–`34` are skipped):
   - Double-click `scripts\fetch_lyrics.cmd`, or run in PowerShell:

```powershell
powershell -ExecutionPolicy Bypass -File scripts\fetch_lyrics.ps1
```

3. Optional: check the lyrics against the game's music without launching FlatOut 2. Double-click `scripts\preview_lyrics.cmd` (needs [Python 3](https://www.python.org/downloads/)). It plays each `.ogg` straight out of `data.bfs` and highlights the `.lrc` in time. Mark a song as wrong; the note is saved to `scripts\lyrics\lyrics-review.json`.
4. Start the game (v1.2, GOG or Steam). Press **F10** at any time to toggle karaoke lyrics on or off.

> Note: No copyrighted `.lrc` files are bundled. `fetch_lyrics.ps1` downloads timed lyrics from [LRCLIB](https://lrclib.net) onto your machine (pass `-PlainLineTimings` for line-level timestamps without word-by-word interpolation, or `-Force` to re-download).

## Controls & Settings

Configured in `scripts\FO2Lyrics.ini`:

| Setting | Default | Description |
| --- | --- | --- |
| `Lyrics` | `1` | `1` = show lyrics, `0` = off (**F10** toggles in-game and saves to `FO2Lyrics.ini`) |
| `Lines` | `3` | Number of lines shown at once (`1`..`6`) |
| `Scale` | `1.0` | Font size multiplier (`0.5`..`3.0`) |
| `Y` | `0.82` | Vertical position in races (`0.1`..`0.95`; `0.82` sits between the rival card and speedometer) |
| `Colour` | `2828FF` | Unsung words colour in `0xBBGGRR` hex (FlatOut 2 red) |
| `SungColour` | `00D7FF` | Sung / active words colour in `0xBBGGRR` hex (FlatOut 2 yellow) |
| `[Keys] Lyrics` | `121` | Virtual-key code to toggle lyrics (`121` = **F10**) |

## Custom `.lrc` files

Place custom `.lrc` files in `scripts\lyrics\`. Songs are matched by:
1. `[file:data/songs1/01.ogg]` tag,
2. `[ti:...]` title and `[ar:...]` artist tags, or
3. File name (`01.lrc` for `data/songs1/01.ogg`, `35.lrc` for `data/songs2/35.ogg`).

Both standard `[mm:ss.xx]` line timings and extended `<mm:ss.xx>` word timings are supported, along with `[offset:+200]` (milliseconds) and UTF-8 / UTF-16 encodings. Playlist `StartPos` offsets (stunt events) and race pauses are tracked automatically.

Diagnostic log: `scripts\FO2Lyrics.log`. To uninstall, remove `scripts\FO2Lyrics.asi`, `scripts\FO2Lyrics.ini`, `scripts\FO2Lyrics-README.md`, `scripts\FO2Lyrics-licenses\`, `scripts\fetch_lyrics.*`, `scripts\preview_lyrics.*`, `scripts\lyrics_preview.html`, `scripts\lyrics\`, and `winmm.dll` (keep `winmm.dll` if another ASI mod uses it).

## Disclaimer

This mod is vibe-coded and provided "as is", without warranty of any kind. The author shall not be liable for any claim, damages or other liability.

## License

- `FO2Lyrics.asi`: MIT (`scripts\FO2Lyrics-licenses\FO2Lyrics.txt`).
- `winmm.dll`: [Ultimate ASI Loader](https://github.com/ThirteenAG/Ultimate-ASI-Loader) by ThirteenAG, MIT (`scripts\FO2Lyrics-licenses\Ultimate-ASI-Loader.txt`).
