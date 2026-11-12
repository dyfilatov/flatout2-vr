#include "lyrics.h"

#include "game.h"
#include "util.h"

#include <windows.h>
#include <d3d9.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <cwctype>
#include <string>
#include <utility>
#include <vector>

// The game's music plays one .ogg per song (data/music/playlist_*.bed), and the mod shows its lyrics,
// karaoke style, on the HUD panel in VR or on the screen without VR. The song's start position and its
// path are taken from the game's playlist table as MusicPlay starts it (kMusicPlayCall: one call site
// per playlist), and the song's position is kept by a clock that runs while the game's music is in a
// playing state (see game.h), so pauses hold it. The lyrics themselves come from the player:
// scripts\lyrics\*.lrc next to the mod (the standard [mm:ss.xx] format, and extended <mm:ss.xx> word
// timings when the file has them), matched to a song by [file:], by [ti:] title and [ar:] artist, or by
// the file's name (01.ogg -> 01.lrc, or any .lrc whose name begins with "01" followed by a separator).
// The mod ships no lyrics: the songs are the artists' own work (see tools\fetch_lyrics.ps1).

namespace lyrics {

namespace {

// ---- the hook into the game's music ---------------------------------------------------------------

bool g_active = false;
uintptr_t oMusicPlay = fo2::kMusicPlay;
std::string g_path;           // the playing .ogg's path, as the game named it (forward slashes, lowercase)
double g_startPos = 0.0;      // the song's start, seconds into the file (stunts' StartPos)
bool g_playing = false;       // a song was started (its position is kept below)
double g_position = 0.0;      // the song's position, seconds into the file
bool g_racing = false;        // the playing song's playlist is one that races/stunts play (2 races, 3 stunts)

// The .lrc files: read once at the first song, each with the lines the file tells.
struct Song
{
    std::wstring name;          // the .lrc's own file name (01.lrc), lowercase
    std::string path;           // the game's .ogg path it wants (from [file:], forward slashes, lowercase)
    std::string title, artist;  // [ti:] and [ar:], lowercase
    std::vector<Line> lines;    // by their time, each line's until filled from the next
    bool hasWords = false;      // some line tells its words' times
};
std::vector<Song> g_songs;
bool g_songsRead = false;
int g_song = -1;               // the playing song's lyrics (into g_songs), -1 none
bool g_bannerPending = false;  // check kMusicBanner after MusicPlay returns

constexpr double kFade = 0.35;  // seconds a line takes to fade in and out

std::string Lower(std::string s)
{
    for (char& c : s) c = (char)tolower((unsigned char)c);
    return s;
}

std::wstring Lower(std::wstring s)
{
    for (wchar_t& c : s) c = (wchar_t)towlower(c);
    return s;
}

std::string NormalizePath(std::string s)
{
    for (char& c : s)
    {
        if (c == '\\') c = '/';
        else c = (char)tolower((unsigned char)c);
    }
    return s;
}

// "data/songs1/01.ogg" -> L"01"
std::wstring SongStem(const std::string& path)
{
    std::wstring p;
    p.reserve(path.size());
    for (char c : path)
    {
        unsigned char u = (unsigned char)c;
        p.push_back(u == '\\' ? L'/' : u < 0x80 ? (wchar_t)towlower(u) : L'?');
    }
    size_t slash = p.find_last_of(L'/');
    if (slash != std::wstring::npos) p.erase(0, slash + 1);
    size_t dot = p.find_last_of(L'.');
    if (dot != std::wstring::npos) p.resize(dot);
    return p;
}

// ---- .lrc files -----------------------------------------------------------------------------------

// Reads a UTF-8 (with or without BOM), Windows-1252, or UTF-16 text file into UTF-16 without large stack buffers.
std::wstring ReadText(const std::wstring& path)
{
    HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return L"";
    DWORD fileSize = GetFileSize(f, nullptr);
    if (fileSize == INVALID_FILE_SIZE || fileSize == 0)
    {
        CloseHandle(f);
        return L"";
    }
    DWORD toRead = std::min<DWORD>(fileSize, 1024u * 1024u);
    std::vector<char> raw(toRead);
    DWORD n = 0;
    BOOL ok = ReadFile(f, raw.data(), toRead, &n, nullptr);
    CloseHandle(f);
    if (!ok || n == 0) return L"";
    if (n < 2) return std::wstring(raw.data(), raw.data() + n);

    std::wstring out;
    const BYTE* bytes = (const BYTE*)raw.data();
    if (bytes[0] == 0xFF && bytes[1] == 0xFE)  // UTF-16 LE
    {
        out.assign((const wchar_t*)(raw.data() + 2), (const wchar_t*)(raw.data() + (n & ~1u)));
    }
    else if (bytes[0] == 0xFE && bytes[1] == 0xFF)  // UTF-16 BE
    {
        out.reserve((n - 2) / 2);
        for (DWORD i = 2; i + 1 < n; i += 2)
        {
            wchar_t c = bytes[i + 1];
            out.push_back((wchar_t)(c | ((wchar_t)bytes[i] << 8)));
        }
    }
    else
    {
        bool bom = n >= 3 && bytes[0] == 0xEF && bytes[1] == 0xBB && bytes[2] == 0xBF;
        const char* s = raw.data() + (bom ? 3 : 0);
        int len = (int)(n - (bom ? 3 : 0));
        if (len <= 0) return L"";
        UINT cp = CP_UTF8;
        int need = MultiByteToWideChar(cp, MB_ERR_INVALID_CHARS, s, len, nullptr, 0);
        if (need <= 0)
        {
            cp = 1252;
            need = MultiByteToWideChar(cp, 0, s, len, nullptr, 0);
        }
        if (need > 0)
        {
            out.resize((size_t)need);
            MultiByteToWideChar(cp, 0, s, len, out.data(), need);
        }
    }
    return out;
}

// "mm:ss.xx" or "[mm:ss.xx]" (the .xx may be .x, .xxx or nothing); false when it is not one.
bool TimeTag(const wchar_t* s, double* out)
{
    if (!s) return false;
    if (*s == L'[') s++;
    wchar_t* end = nullptr;
    double whole = wcstod(s, &end);
    if (end == s || *end != L':') return false;
    s = end + 1;
    double part = wcstod(s, &end);
    if (end == s) return false;
    if (*end == L']') end++;
    if (*end != L'\0') return false;
    if (whole < 0 || part < 0 || part >= 60) return false;
    *out = whole * 60.0 + part;
    return true;
}

void AddLine(Song& song, double at, const std::wstring& text)
{
    Line line;
    line.at = at;
    // Extended .lrc word timings ("<mm:ss.xx>word "): extracted so words brighten one by one as they are sung.
    std::wstring plain;
    const wchar_t* q = text.c_str();
    double wordAt = at;
    while (*q)
    {
        if (q[0] == L'<' && iswdigit(q[1]))
        {
            const wchar_t* t = q + 1;
            wchar_t* end = nullptr;
            double whole = wcstod(t, &end);
            if (end != t && *end == L':')
            {
                double part = wcstod(end + 1, &end);
                if (*end == L'>')
                {
                    wordAt = whole * 60.0 + part;
                    q = end + 1;
                    const wchar_t* stop = q;
                    while (*stop && *stop != L'<') stop++;
                    std::wstring word(q, stop);
                    if (!word.empty())
                    {
                        line.words.push_back({ word, wordAt });
                        plain += word;
                    }
                    q = stop;
                    continue;
                }
            }
        }
        plain.push_back(*q++);
    }
    if (!line.words.empty()) song.hasWords = true;
    while (!plain.empty() && iswspace(plain.back())) plain.pop_back();
    if (plain.empty() && line.words.empty()) return;
    line.plain = std::move(plain);
    song.lines.push_back(std::move(line));
}

std::string WideToUtf8Lower(const wchar_t* val)
{
    while (*val && iswspace(*val)) val++;
    int wlen = (int)wcslen(val);
    while (wlen > 0 && iswspace(val[wlen - 1])) wlen--;
    if (wlen <= 0) return "";
    int need = WideCharToMultiByte(CP_UTF8, 0, val, wlen, nullptr, 0, nullptr, nullptr);
    if (need <= 0) return "";
    std::string out((size_t)need, '\0');
    WideCharToMultiByte(CP_UTF8, 0, val, wlen, out.data(), need, nullptr, nullptr);
    return Lower(std::move(out));
}

void ReadLrc(const std::wstring& path)
{
    std::wstring text = ReadText(path);
    if (text.empty()) return;
    Song song;
    song.name = Lower(path.substr(path.find_last_of(L"/\\") + 1));
    std::vector<std::pair<double, std::wstring>> timed;
    double offset = 0;
    bool any = false;
    wchar_t* s = text.data();
    while (s && *s)
    {
        while (*s && *s != L'[') s++;
        if (!*s) break;
        wchar_t* tag = s + 1;
        wchar_t* close = wcschr(tag, L']');
        if (!close) break;
        *close = L'\0';
        wchar_t* rest = close + 1;
        // Support multiple timestamps on one line, e.g. [01:10.00][02:10.00]Chorus text
        std::vector<double> lineTimes;
        double at = 0;
        if (TimeTag(tag, &at))
        {
            lineTimes.push_back(at);
            while (*rest == L'[')
            {
                wchar_t* nextClose = wcschr(rest + 1, L']');
                if (!nextClose) break;
                std::wstring extraTag(rest + 1, nextClose - (rest + 1));
                double extraAt = 0;
                if (!TimeTag(extraTag.c_str(), &extraAt)) break;
                lineTimes.push_back(extraAt);
                rest = nextClose + 1;
            }
        }

        wchar_t* end = wcspbrk(rest, L"\r\n");
        std::wstring value;
        if (end)
        {
            value.assign(rest, end - rest);
            *end = L'\0';
            s = end + 1;
        }
        else
        {
            value.assign(rest);
            s = rest + wcslen(rest);
        }

        if (!lineTimes.empty())
        {
            for (double t : lineTimes) timed.emplace_back(t, value);
            any = true;
            continue;
        }

        wchar_t* colon = wcschr(tag, L':');
        if (!colon) continue;
        *colon = L'\0';
        std::wstring key = Lower(tag);
        const wchar_t* val = colon + 1;
        if (key == L"ti") song.title = WideToUtf8Lower(val);
        else if (key == L"ar") song.artist = WideToUtf8Lower(val);
        else if (key == L"file") song.path = NormalizePath(WideToUtf8Lower(val));
        else if (key == L"offset")
        {
            while (*val && iswspace(*val)) val++;
            wchar_t* e = nullptr;
            double off = wcstod(val, &e);
            if (e != val) offset = off / 1000.0;  // [offset:] is in milliseconds
        }
    }
    if (!any) return;
    std::stable_sort(timed.begin(), timed.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
    for (auto& [t, value] : timed)
    {
        size_t firstNonSpace = 0;
        while (firstNonSpace < value.size() && iswspace(value[firstNonSpace])) firstNonSpace++;
        std::wstring trimmed = value.substr(firstNonSpace);
        AddLine(song, std::max(0.0, t + offset), trimmed);
    }
    for (size_t i = 0; i + 1 < song.lines.size(); i++) song.lines[i].until = song.lines[i + 1].at;
    if (!song.lines.empty()) song.lines.back().until = song.lines.back().at + 8.0;
    if (!song.lines.empty()) g_songs.push_back(std::move(song));
}

void ReadSongs()
{
    g_songs.clear();
    g_songsRead = true;
    std::wstring dir = ModuleDir() + L"lyrics";
    std::wstring pattern = dir + L"\\*.lrc";
    WIN32_FIND_DATAW fd;
    HANDLE find = FindFirstFileW(pattern.c_str(), &fd);
    if (find != INVALID_HANDLE_VALUE)
    {
        std::vector<std::wstring> files;
        do
        {
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
            files.push_back(dir + L"\\" + fd.cFileName);
        } while (FindNextFileW(find, &fd));
        FindClose(find);
        std::sort(files.begin(), files.end());
        for (const std::wstring& file : files) ReadLrc(file);
    }
    Log("lyrics: %d song%s in lyrics\\", (int)g_songs.size(), g_songs.size() == 1 ? "" : "s");
}

int FindSongByFileTag(const std::string& path)
{
    if (path.empty()) return -1;
    std::string p = NormalizePath(path);
    for (size_t i = 0; i < g_songs.size(); i++)
        if (!g_songs[i].path.empty() && g_songs[i].path == p) return (int)i;
    return -1;
}

// Matches "data/songs1/01.ogg" to "01.lrc" (or a file whose stem starts with "01" + non-alphanumeric separator).
int FindSongByPath(const std::string& path)
{
    int byTag = FindSongByFileTag(path);
    if (byTag >= 0) return byTag;
    std::wstring key = SongStem(path);
    if (key.empty()) return -1;
    for (size_t i = 0; i < g_songs.size(); i++)
    {
        std::wstring base = g_songs[i].name;
        size_t dot = base.find_last_of(L'.');
        if (dot != std::wstring::npos) base.resize(dot);
        if (base == key) return (int)i;
    }
    for (size_t i = 0; i < g_songs.size(); i++)
    {
        std::wstring base = g_songs[i].name;
        size_t dot = base.find_last_of(L'.');
        if (dot != std::wstring::npos) base.resize(dot);
        if (base.size() > key.size() && base.compare(0, key.size(), key) == 0 && !iswalnum(base[key.size()]))
            return (int)i;
    }
    return -1;
}

std::string ReadGameString(uintptr_t strAddr)
{
    if (!strAddr) return "";
    uint32_t size = *(const uint32_t*)(strAddr + fo2::kStrSize);
    uint32_t cap = *(const uint32_t*)(strAddr + fo2::kStrCap);
    if (size == 0 || size > 1024) return "";
    const char* s = cap >= 16 ? *(const char* const*)(strAddr + fo2::kStrBuf) : (const char*)(strAddr + fo2::kStrBuf);
    return s ? std::string(s, size) : "";
}

struct PlaylistMatch
{
    std::string path;
    std::string artist;
    std::string title;
    int startPosMs = 0;
};

// Finds the playlist entry whose preloaded .ogg buffer is `data`: checks `playlistId` (1..3) first, then the others.
bool GetSongFromPlaylist(int playlistId, int lenBytes, const void* data, PlaylistMatch& out)
{
    if (!data) return false;
    auto scan = [&](int idx) -> bool {
        uintptr_t vec = fo2::kPlaylistBegin[idx];
        uintptr_t begin = *(const uintptr_t*)vec;
        uintptr_t end = *(const uintptr_t*)(vec + 4);
        if (!begin || end <= begin) return false;
        for (uintptr_t entry = begin; entry + fo2::kPlaylistEntrySize <= end; entry += fo2::kPlaylistEntrySize)
        {
            if (*(const void* const*)(entry + fo2::kPlaylistData) != data) continue;
            int entryLen = *(const int*)(entry + fo2::kPlaylistDataLen);
            if (lenBytes > 0 && entryLen > 0 && entryLen != lenBytes) continue;
            out.path = NormalizePath(ReadGameString(entry + fo2::kPlaylistFile));
            out.artist = ReadGameString(entry + fo2::kPlaylistArtist);
            out.title = ReadGameString(entry + fo2::kPlaylistSong);
            out.startPosMs = *(const int*)(entry + fo2::kPlaylistStartPos);
            return !out.path.empty() || !out.title.empty();
        }
        return false;
    };

    if (playlistId >= 1 && playlistId <= 3 && scan(playlistId - 1)) return true;
    for (int idx = 0; idx < 3; idx++)
        if (idx != playlistId - 1 && scan(idx)) return true;
    return false;
}

int FindSong(const std::string& artist, const std::string& song)
{
    if (song.empty()) return -1;
    std::string a = Lower(artist), t = Lower(song);
    for (size_t i = 0; i < g_songs.size(); i++)
        if (!g_songs[i].title.empty() && g_songs[i].title == t &&
            (g_songs[i].artist.empty() || a.empty() || g_songs[i].artist == a))
            return (int)i;
    for (size_t i = 0; i < g_songs.size(); i++)
        if (!g_songs[i].title.empty() && g_songs[i].title == t) return (int)i;
    if (!a.empty())
    {
        for (size_t i = 0; i < g_songs.size(); i++)
            if (!g_songs[i].title.empty() && g_songs[i].artist == a &&
                (g_songs[i].title.find(t) != std::string::npos || t.find(g_songs[i].title) != std::string::npos))
                return (int)i;
    }
    for (size_t i = 0; i < g_songs.size(); i++)
        if (!g_songs[i].title.empty() &&
            (g_songs[i].title.find(t) != std::string::npos || t.find(g_songs[i].title) != std::string::npos))
            return (int)i;
    return -1;
}

int FindSongByBanner()
{
    const char* const* banner = (const char* const*)fo2::kMusicBanner;
    if (banner && banner[1] && *banner[1] && banner[2] && *banner[2])
        return FindSong(banner[1], banner[2]);
    return -1;
}

void SelectSong(int song, const char* via)
{
    g_song = song;
    g_playing = true;
    g_position = g_startPos;
    const Song& s = g_songs[song];
    const char* label = !s.title.empty() ? s.title.c_str() : g_path.c_str();
    Log("lyrics: \"%s%s%s\" (%ls, %d lines, start %.2f s)%s%s", s.artist.c_str(), s.artist.empty() ? "" : " - ", label,
        s.name.c_str(), (int)s.lines.size(), g_startPos, via, g_racing ? "" : " - menus");
}

// ---- the playing song ----------------------------------------------------------------------------

void OnPlay(int playlist, int lenBytes, const void* data)
{
    g_playing = false;
    g_bannerPending = false;
    g_path.clear();
    int seekMs = *(const int*)fo2::kMusicSeekTime != 0 ? *(const int*)fo2::kMusicSeekMs : 0;
    g_startPos = seekMs > 0 ? seekMs * 0.001 : 0.0;
    g_position = g_startPos;
    g_song = -1;
    g_racing = playlist == 2 || playlist == 3;
    if (!g_songsRead) ReadSongs();
    if (g_songs.empty()) return;

    PlaylistMatch pm;
    if (GetSongFromPlaylist(playlist, lenBytes, data, pm))
    {
        g_path = pm.path;
        if (g_startPos <= 0.0 && playlist == 3 && *(const int*)fo2::kMusicSeekTime != 0 && pm.startPosMs > 0)
        {
            g_startPos = pm.startPosMs * 0.001;
            g_position = g_startPos;
        }
        int song = FindSongByFileTag(g_path);
        if (song < 0 && !pm.title.empty()) song = FindSong(pm.artist, pm.title);
        if (song < 0 && !g_path.empty()) song = FindSongByPath(g_path);
        if (song >= 0)
        {
            SelectSong(song, "");
            return;
        }
        Log("lyrics: no .lrc match for \"%s - %s\" (%s) in lyrics\\", pm.artist.c_str(), pm.title.c_str(), g_path.c_str());
    }
    g_bannerPending = true;
}

void __stdcall SongStart(int playlist, int lenBytes, const void* data)
{
    OnPlay(playlist, lenBytes, data);
}

void __stdcall SongStarted()
{
    if (!g_playing && g_bannerPending)
    {
        int song = FindSongByBanner();
        if (song >= 0)
        {
            g_bannerPending = false;
            SelectSong(song, " [banner]");
        }
    }
}

// Before the game's own call: the preloaded .ogg buffer pointer is at [esp+4], the playlist id in eax,
// and the buffer's byte length in ecx.
static __declspec(naked) void MusicPlayHook()
{
    __asm {
        pushad
        push dword ptr [esp + 32 + 4]  ; buffer pointer
        push ecx                       ; buffer length in bytes
        push eax                       ; playlist id (1 menus, 2 races, 3 stunts)
        call SongStart                 ; __stdcall: cleans 12 bytes
        popad
        push dword ptr [esp + 4]       ; re-push buffer pointer for oMusicPlay (cdecl)
        call dword ptr [oMusicPlay]
        add esp, 4
        pushad
        call SongStarted
        popad
        ret
    }
}

}  // namespace

bool Install()
{
#ifndef FO2_LYRICS_STANDALONE
    if (GetModuleHandleW(L"FO2Lyrics.asi") != nullptr ||
        GetFileAttributesW((ModuleDir() + L"FO2Lyrics.asi").c_str()) != INVALID_FILE_ATTRIBUTES)
    {
        Log("lyrics: FO2Lyrics.asi is present - leaving lyrics to it");
        g_active = false;
        return true;
    }
#endif
    bool ok = true;
    for (uintptr_t site : fo2::kMusicPlayCall)
        ok &= PatchCall(site, fo2::kMusicPlay, (const void*)&MusicPlayHook);
    if (!ok)
    {
        Log("lyrics: the game's music play calls differ - no lyrics");
        g_active = false;
        return false;
    }
    g_active = true;
    Log("lyrics: the game's music play calls hooked (lyrics from scripts\\lyrics\\*.lrc, [Lyrics] Lyrics=%d)",
        g_cfg.lyricsEnabled ? 1 : 0);
    return true;
}

bool Active() { return g_active; }

void Toggle()
{
    if (!g_active) return;
    g_cfg.lyricsEnabled = !g_cfg.lyricsEnabled;
    SaveIniValue("Lyrics", "Lyrics", g_cfg.lyricsEnabled ? "1" : "0");
    if (g_cfg.lyricsEnabled && !g_songsRead) ReadSongs();
    Log("Lyrics key: Lyrics=%d (%s)", g_cfg.lyricsEnabled ? 1 : 0, g_cfg.lyricsEnabled ? "on" : "off");
}

void Tick(double dt)
{
    if (!g_active) return;
    if (g_bannerPending)
    {
        int song = FindSongByBanner();
        if (song >= 0)
        {
            g_bannerPending = false;
            SelectSong(song, " [banner]");
        }
    }
    if (!g_playing) return;
    int state = *(const int*)fo2::kMusicState;
    if (state >= 1 && state <= 4) g_position += dt;
    else if (state == 0) g_playing = false;
    if (g_position < 0.0) g_position = 0.0;
}

const Show& CurrentShow()
{
    static Show none;
    if (!g_active || !g_playing || g_song < 0 || g_song >= (int)g_songs.size()) return none;
    int state = *(const int*)fo2::kMusicState;
    if (state == 0) return none;
    const std::vector<Line>& lines = g_songs[g_song].lines;
    if (lines.empty()) return none;
    double at = g_position;
    if (at > lines.back().until + kFade) return none;

    int i = (int)(std::upper_bound(lines.begin(), lines.end(), at, [](double t, const Line& l) { return t < l.at; }) - lines.begin()) - 1;
    static Show show;
    show.count = 0;
    int maxLines = std::clamp(g_cfg.lyricsLines, 1, 6);

    if (i < 0)
    {
        if (lines[0].at - at <= 5.0) show.lines[show.count++] = lines[0];
        return show;
    }

    if (at > lines[i].until + kFade)
    {
        if (i + 1 < (int)lines.size() && lines[i + 1].at - at <= 5.0)
        {
            show.lines[show.count++] = lines[i + 1];
            return show;
        }
        return none;
    }

    int last = (maxLines >= 2 && i + 1 < (int)lines.size() && lines[i + 1].at - at <= 8.0) ? i + 1 : i;
    int first = std::max(0, last - (maxLines - 1));
    for (int k = first; k <= last && show.count < maxLines; k++)
        show.lines[show.count++] = lines[k];
    return show;
}

// ---- the font -------------------------------------------------------------------------------------

namespace {

constexpr int kFontPx = 48;
constexpr int kAtlas = 1024;

struct Glyph
{
    RECT uv = {};
    float advance = 0.0f;
    float ox = 0.0f;
};

// Compact heap-backed glyph table (never allocates a 65536-entry array on the stack or in .bss).
struct Font
{
    IDirect3DDevice9* dev = nullptr;
    IDirect3DTexture9* tex = nullptr;
    IDirect3DVertexDeclaration9* decl = nullptr;
    int lineHeight = 0;
    int16_t ascii[128] = {};
    std::vector<std::pair<wchar_t, int>> extra;
    std::vector<Glyph> glyphs;
} g_font;

const Glyph kEmptyGlyph = {};

const Glyph& FindGlyph(wchar_t c)
{
    if (c < 128)
    {
        int idx = g_font.ascii[(unsigned)c];
        return idx >= 0 && idx < (int)g_font.glyphs.size() ? g_font.glyphs[(size_t)idx] : kEmptyGlyph;
    }
    auto it = std::lower_bound(g_font.extra.begin(), g_font.extra.end(), c,
                               [](const std::pair<wchar_t, int>& a, wchar_t b) { return a.first < b; });
    if (it != g_font.extra.end() && it->first == c && it->second >= 0 && it->second < (int)g_font.glyphs.size())
        return g_font.glyphs[(size_t)it->second];
    return kEmptyGlyph;
}

// Non-FVF element order (TEXCOORD0 before COLOR0) so IDirect3DDevice9::GetFVF returns 0: when standalone
// FO2Lyrics.asi draws during FO2VR's HUD capture, FO2VR's HudCardOf leaves custom declarations on the HUD panel.
bool EnsureVertexDecl(IDirect3DDevice9* dev)
{
    if (g_font.decl) return true;
    static const D3DVERTEXELEMENT9 kElems[] = {
        { 0, 0, D3DDECLTYPE_FLOAT4, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_POSITIONT, 0 },
        { 0, 16, D3DDECLTYPE_FLOAT2, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD, 0 },
        { 0, 24, D3DDECLTYPE_D3DCOLOR, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_COLOR, 0 },
        D3DDECL_END()
    };
    return SUCCEEDED(dev->CreateVertexDeclaration(kElems, &g_font.decl)) && g_font.decl != nullptr;
}

bool BuildFont(IDirect3DDevice9* dev)
{
    if (g_font.dev && g_font.dev != dev) ReleaseFont();
    if (g_font.tex) return EnsureVertexDecl(dev);

    std::vector<wchar_t> chars;
    chars.reserve(256);
    for (const Song& s : g_songs)
        for (const Line& l : s.lines)
        {
            for (wchar_t c : l.plain)
                if (c != 0) chars.push_back(c);
            for (const Word& w : l.words)
                for (wchar_t c : w.text)
                    if (c != 0) chars.push_back(c);
        }
    if (chars.empty()) return false;
    chars.push_back(L' ');
    std::sort(chars.begin(), chars.end());
    chars.erase(std::unique(chars.begin(), chars.end()), chars.end());

    HDC dc = GetDC(nullptr);
    HDC mem = CreateCompatibleDC(dc);
    const wchar_t* candidateFonts[] = {
        L"Franklin Gothic Heavy",
        L"Franklin Gothic Demi Cond",
        L"Franklin Gothic Medium Cond",
        L"Franklin Gothic Medium",
        L"Impact",
        L"Arial"
    };
    HFONT font = nullptr;
    for (const wchar_t* face : candidateFonts)
    {
        font = CreateFontW(-kFontPx, 0, 0, 0, FW_HEAVY, TRUE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                           CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY, DEFAULT_PITCH | FF_DONTCARE, face);
        if (font) break;
    }
    if (!font)
    {
        DeleteDC(mem);
        ReleaseDC(nullptr, dc);
        return false;
    }
    HGDIOBJ old = SelectObject(mem, font);
    TEXTMETRICW tm = {};
    GetTextMetricsW(mem, &tm);
    wchar_t actualFace[64] = {};
    GetTextFaceW(mem, 64, actualFace);
    int cellH = std::max((int)tm.tmHeight, kFontPx);

    D3DLOCKED_RECT lock = {};
    IDirect3DTexture9* tex = nullptr;
    HRESULT hr = dev->CreateTexture(kAtlas, kAtlas, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &tex, nullptr);
    if (FAILED(hr))
        hr = dev->CreateTexture(kAtlas, kAtlas, 1, D3DUSAGE_DYNAMIC, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &tex, nullptr);
    if (FAILED(hr) || FAILED(tex->LockRect(0, &lock, nullptr, 0)))
    {
        Log("lyrics: BuildFont failed to create/lock texture (hr 0x%08X)", (unsigned)hr);
        if (tex) tex->Release();
        SelectObject(mem, old);
        DeleteObject(font);
        DeleteDC(mem);
        ReleaseDC(nullptr, dc);
        return false;
    }

    int pitchDwords = (int)lock.Pitch / 4;
    DWORD* px = (DWORD*)lock.pBits;
    memset(px, 0, (size_t)kAtlas * (size_t)lock.Pitch);
    // Solid white 4x4 texel block at (0,0)..(3,3) for translucent backing plates.
    for (int py = 0; py < 4; py++)
        for (int pxIdx = 0; pxIdx < 4; pxIdx++)
            px[py * pitchDwords + pxIdx] = 0xFFFFFFFF;

    for (int16_t& a : g_font.ascii) a = -1;
    g_font.extra.clear();
    g_font.glyphs.clear();
    g_font.glyphs.reserve(chars.size());

    int x = 6, y = 1, row = 0;
    for (wchar_t ch : chars)
    {
        ABC abc = {};
        if (!GetCharABCWidthsW(mem, ch, ch, &abc)) continue;
        int width = abc.abcA + (int)abc.abcB + abc.abcC;
        if (width <= 0) width = abc.abcB > 0 ? (int)abc.abcB : kFontPx / 4;

        Glyph g;
        g.advance = (float)width;

        GLYPHMETRICS gm = {};
        static const MAT2 kUnit = { { 0, 1 }, { 0, 0 }, { 0, 0 }, { 0, 1 } };
        DWORD need = GetGlyphOutlineW(mem, ch, GGO_GRAY8_BITMAP, &gm, 0, nullptr, &kUnit);
        if (need != GDI_ERROR && need > 0 && gm.gmBlackBoxX > 0)
        {
            std::vector<BYTE> bits(need);
            if (GetGlyphOutlineW(mem, ch, GGO_GRAY8_BITMAP, &gm, need, bits.data(), &kUnit) != GDI_ERROR)
            {
                int cellW = (int)gm.gmBlackBoxX;
                if (x + cellW + 1 > kAtlas)
                {
                    x = 1;
                    y += row + 1;
                    row = 0;
                }
                if (y + cellH + 1 <= kAtlas)
                {
                    int gw = std::min((int)gm.gmBlackBoxX, kAtlas), gh = std::min((int)gm.gmBlackBoxY, kAtlas);
                    int stride = ((int)gm.gmBlackBoxX + 3) & ~3;
                    int goy = y + tm.tmAscent - gm.gmptGlyphOrigin.y;
                    for (int r = 0; r < gh; r++)
                        for (int col = 0; col < gw; col++)
                        {
                            int px2 = x + col, py2 = goy + r;
                            if (px2 < x || px2 >= x + cellW || px2 >= kAtlas || py2 < y || py2 >= y + cellH || py2 >= kAtlas)
                                continue;
                            DWORD a = r * stride + col < (int)bits.size() ? bits[(size_t)(r * stride + col)] : 0;
                            a = std::min<DWORD>(a, 64u) * 255u / 64u;
                            px[py2 * pitchDwords + px2] = 0x00FFFFFFu | (a << 24);
                        }
                    g.uv = { x, y, x + cellW, y + cellH };
                    g.ox = (float)gm.gmptGlyphOrigin.x;
                    x += cellW + 2;
                    row = std::max(row, cellH);
                }
            }
        }

        int idx = (int)g_font.glyphs.size();
        g_font.glyphs.push_back(g);
        if (ch < 128)
            g_font.ascii[(unsigned)ch] = (int16_t)idx;
        else
            g_font.extra.emplace_back(ch, idx);
    }

    tex->UnlockRect(0);
    SelectObject(mem, old);
    DeleteObject(font);
    DeleteDC(mem);
    ReleaseDC(nullptr, dc);

    g_font.dev = dev;
    g_font.tex = tex;
    g_font.lineHeight = cellH;
    if (!EnsureVertexDecl(dev))
    {
        ReleaseFont();
        return false;
    }
    Log("lyrics: font built (\"%ls\", %d px tall, %d glyphs)", actualFace, cellH, (int)g_font.glyphs.size());
    return true;
}

float TextWidth(const std::wstring& s)
{
    float w = 0.0f;
    for (wchar_t c : s) w += FindGlyph(c).advance;
    return w;
}

void GlyphUv(const Glyph& g, float* u0, float* v0, float* u1, float* v1)
{
    *u0 = (float)g.uv.left / (float)kAtlas;
    *v0 = (float)g.uv.top / (float)kAtlas;
    *u1 = (float)g.uv.right / (float)kAtlas;
    *v1 = (float)g.uv.bottom / (float)kAtlas;
}

bool InRace()
{
    const BYTE* gf = *(const BYTE* const*)fo2::kGameFlow;
    return gf && *(const int*)(gf + fo2::kGameState) == 2;
}

float Dot3(const float a[3], const float b[3]) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }

}  // namespace

void ReleaseFont()
{
    if (g_font.tex)
    {
        g_font.tex->Release();
        g_font.tex = nullptr;
    }
    if (g_font.decl)
    {
        g_font.decl->Release();
        g_font.decl = nullptr;
    }
    g_font.dev = nullptr;
    g_font.lineHeight = 0;
    for (int16_t& a : g_font.ascii) a = -1;
    g_font.extra.clear();
    g_font.glyphs.clear();
}

void Draw(IDirect3DDevice9* dev, float targetW, float targetH, const PanelView* panel, bool inScene)
{
    const Show& show = CurrentShow();
    if (!show.count || !dev || targetW <= 0.0f || targetH <= 0.0f) return;
    if (!g_songsRead && g_song >= 0) ReadSongs();
    if (g_song < 0 || !BuildFont(dev)) return;

    struct V
    {
        float x, y, z, rhw;
        float u, v;
        DWORD colour;
    };
    std::vector<V> verts;
    verts.reserve(512);
    double now = g_position;

    // Virtual canvas size: the bound target's pixels in 2D, or a virtual HUD canvas when projecting onto the 3D panel.
    float canvasW = targetW, canvasH = targetH;
    if (panel)
    {
        if (panel->panelWidth <= 0.0f || panel->panelHeight <= 0.0f || panel->rightTan <= panel->left || panel->upTan <= panel->down)
            return;
        canvasW = 1920.0f;
        canvasH = canvasW * (panel->panelHeight / panel->panelWidth);
    }

    auto toRgb = [](uint32_t bgr) -> DWORD {
        return ((bgr & 0xFFu) << 16) | (bgr & 0xFF00u) | ((bgr >> 16) & 0xFFu);
    };
    DWORD dim = toRgb(g_cfg.lyricsColour);
    DWORD bright = toRgb(g_cfg.lyricsSungColour);

    float lineH = 0.046f * canvasH * g_cfg.lyricsScale;
    float cellH = g_font.lineHeight > 0 ? (float)g_font.lineHeight : (float)kFontPx;
    float scale = lineH / cellH;

    float lineWidths[6] = {};
    float lineAlphas[6] = {};
    float maxW = 0.0f;
    float maxAlpha = 0.0f;

    for (int i = 0; i < show.count; i++)
    {
        const Line& line = show.lines[i];
        float alpha = 1.0f;
        if (now < line.at)
        {
            double appearAt = i > 0 ? show.lines[i - 1].at : line.at - 4.0;
            if (now < appearAt) alpha = 0.0f;
            else if (now - appearAt < kFade) alpha = (float)((now - appearAt) / kFade);
        }
        else if (now - line.at < kFade && i == show.count - 1)
        {
            alpha = (float)((now - line.at) / kFade);
        }
        if (now > line.until)
        {
            double past = now - line.until;
            alpha = past >= kFade ? 0.0f : std::min(alpha, (float)(1.0 - past / kFade));
        }
        lineAlphas[i] = alpha;
        if (alpha > maxAlpha) maxAlpha = alpha;
        if (alpha > 0.0f)
        {
            lineWidths[i] = TextWidth(line.plain) * scale;
            if (lineWidths[i] > maxW) maxW = lineWidths[i];
        }
    }
    if (maxAlpha <= 0.0f) return;

    // Fit horizontally in the lower-centre gap between the rival portrait on the left and the gauges on the right.
    float maxAllowedW = 0.38f * canvasW;
    if (maxW > maxAllowedW && maxW > 0.0f)
    {
        float shrink = maxAllowedW / maxW;
        scale *= shrink;
        lineH *= shrink;
        for (int i = 0; i < show.count; i++) lineWidths[i] *= shrink;
        maxW = maxAllowedW;
    }

    float yFrac = InRace() ? g_cfg.lyricsY : 0.82f;
    float cy = canvasH * yFrac;
    float totalH = (float)show.count * lineH;
    float topY = cy - 0.5f * totalH;

    float padX = 14.0f * scale;
    float padY = 6.0f * scale;
    float px0 = 0.5f * (canvasW - maxW) - padX;
    float px1 = 0.5f * (canvasW + maxW) + padX;
    float py0 = topY - padY;
    float py1 = topY + totalH + padY;

    auto mapVertex = [&](float cx, float cyPos, float u, float v, DWORD col, V* out) -> bool {
        if (!panel)
        {
            *out = { cx - 0.5f, cyPos - 0.5f, 0.0f, 1.0f, u, v, col };
            return true;
        }
        float pu = cx / canvasW;
        float pv = cyPos / canvasH;
        float p[3] = {
            (pu - 0.5f) * panel->panelWidth - panel->eye[0],
            panel->panelY + (0.5f - pv) * panel->panelHeight - panel->eye[1],
            panel->panelDist - panel->eye[2]
        };
        float ex = Dot3(p, panel->right);
        float ey = Dot3(p, panel->up);
        float ez = Dot3(p, panel->forward);
        if (ez <= 0.05f) return false;
        float sx = ((ex / ez - panel->left) / (panel->rightTan - panel->left)) * targetW - 0.5f;
        float sy = ((panel->upTan - ey / ez) / (panel->upTan - panel->down)) * targetH - 0.5f;
        *out = { sx, sy, 0.0f, 1.0f / ez, u, v, col };
        return true;
    };

    auto emitBox = [&](float x0, float y0, float x1, float y1, float u0, float v0, float u1, float v1, DWORD col) {
        V q[4];
        if (!mapVertex(x0, y0, u0, v0, col, &q[0]) || !mapVertex(x1, y0, u1, v0, col, &q[1]) ||
            !mapVertex(x1, y1, u1, v1, col, &q[2]) || !mapVertex(x0, y1, u0, v1, col, &q[3]))
            return;
        verts.push_back(q[0]);
        verts.push_back(q[1]);
        verts.push_back(q[3]);
        verts.push_back(q[1]);
        verts.push_back(q[2]);
        verts.push_back(q[3]);
    };

    float uw = 1.5f / (float)kAtlas, vw = 1.5f / (float)kAtlas;
    DWORD plateA = (DWORD)std::clamp(lroundf(maxAlpha * 180.0f), 0l, 255l) << 24;
    DWORD plateCol = plateA | 0x000E1216u;
    DWORD borderCol = plateA | 0x00020406u;
    emitBox(px0 - 1.5f, py0 - 1.5f, px1 + 1.5f, py1 + 1.5f, uw, vw, uw, vw, borderCol);
    emitBox(px0, py0, px1, py1, uw, vw, uw, vw, plateCol);

    auto emitChar = [&](wchar_t ch, float cx, float y0, DWORD col) {
        const Glyph& g = FindGlyph(ch);
        float gw = (float)(g.uv.right - g.uv.left) * scale;
        if (gw > 0.0f)
        {
            float u0, v0, u1, v1;
            GlyphUv(g, &u0, &v0, &u1, &v1);
            float x0 = cx + g.ox * scale;
            emitBox(x0, y0, x0 + gw, y0 + lineH, u0, v0, u1, v1, col);
        }
    };

    for (int i = 0; i < show.count; i++)
    {
        float alpha = lineAlphas[i];
        if (alpha <= 0.0f) continue;
        const Line& line = show.lines[i];
        float lineY0 = topY + (float)i * lineH;
        float startX = 0.5f * (canvasW - lineWidths[i]);
        DWORD aBits = (DWORD)std::clamp(lroundf(alpha * 255.0f), 0l, 255l) << 24;

        float o = 1.5f * scale;
        DWORD shadowColor = aBits;  // black RGB with the line's alpha
        const float offsets[4][2] = { { -o, 0.0f }, { o, 0.0f }, { 0.0f, -o }, { 0.0f, o } };
        for (const auto& off : offsets)
        {
            float scx = startX + off[0];
            float sy0 = lineY0 + off[1];
            if (!line.words.empty())
            {
                for (const Word& word : line.words)
                    for (wchar_t ch : word.text)
                    {
                        emitChar(ch, scx, sy0, shadowColor);
                        scx += FindGlyph(ch).advance * scale;
                    }
            }
            else
            {
                for (wchar_t ch : line.plain)
                {
                    emitChar(ch, scx, sy0, shadowColor);
                    scx += FindGlyph(ch).advance * scale;
                }
            }
        }

        float scx = startX;
        if (!line.words.empty())
        {
            for (const Word& word : line.words)
            {
                DWORD c = (now >= word.at ? bright : dim) | aBits;
                for (wchar_t ch : word.text)
                {
                    emitChar(ch, scx, lineY0, c);
                    scx += FindGlyph(ch).advance * scale;
                }
            }
        }
        else
        {
            DWORD c = (now >= line.at ? bright : dim) | aBits;
            for (wchar_t ch : line.plain)
            {
                emitChar(ch, scx, lineY0, c);
                scx += FindGlyph(ch).advance * scale;
            }
        }
    }

    if (verts.empty()) return;
    IDirect3DStateBlock9* saved = nullptr;
    if (FAILED(dev->CreateStateBlock(D3DSBT_ALL, &saved))) return;

    D3DVIEWPORT9 oldVp = {};
    dev->GetViewport(&oldVp);
    D3DVIEWPORT9 vp = { 0, 0, (DWORD)targetW, (DWORD)targetH, 0.0f, 1.0f };
    dev->SetViewport(&vp);

    dev->SetVertexShader(nullptr);
    dev->SetPixelShader(nullptr);
    dev->SetVertexDeclaration(g_font.decl);
    dev->SetTexture(0, g_font.tex);
    dev->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG2);
    dev->SetTextureStageState(0, D3DTSS_COLORARG2, D3DTA_DIFFUSE);
    dev->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_MODULATE);
    dev->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_TEXTURE);
    dev->SetTextureStageState(0, D3DTSS_ALPHAARG2, D3DTA_DIFFUSE);
    dev->SetTextureStageState(0, D3DTSS_TEXCOORDINDEX, 0);
    dev->SetTextureStageState(0, D3DTSS_TEXTURETRANSFORMFLAGS, D3DTTFF_DISABLE);
    dev->SetTextureStageState(1, D3DTSS_COLOROP, D3DTOP_DISABLE);
    dev->SetTextureStageState(1, D3DTSS_ALPHAOP, D3DTOP_DISABLE);
    dev->SetSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_LINEAR);
    dev->SetSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
    dev->SetSamplerState(0, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
    dev->SetSamplerState(0, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
    dev->SetSamplerState(0, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
    dev->SetSamplerState(0, D3DSAMP_SRGBTEXTURE, FALSE);
    dev->SetRenderState(D3DRS_ZENABLE, D3DZB_FALSE);
    dev->SetRenderState(D3DRS_ZWRITEENABLE, FALSE);
    dev->SetRenderState(D3DRS_STENCILENABLE, FALSE);
    dev->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
    dev->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
    dev->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
    dev->SetRenderState(D3DRS_BLENDOP, D3DBLENDOP_ADD);
    dev->SetRenderState(D3DRS_SEPARATEALPHABLENDENABLE, TRUE);
    dev->SetRenderState(D3DRS_SRCBLENDALPHA, D3DBLEND_ONE);
    dev->SetRenderState(D3DRS_DESTBLENDALPHA, D3DBLEND_INVSRCALPHA);
    dev->SetRenderState(D3DRS_BLENDOPALPHA, D3DBLENDOP_ADD);
    dev->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
    dev->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
    dev->SetRenderState(D3DRS_FILLMODE, D3DFILL_SOLID);
    dev->SetRenderState(D3DRS_LIGHTING, FALSE);
    dev->SetRenderState(D3DRS_FOGENABLE, FALSE);
    dev->SetRenderState(D3DRS_SCISSORTESTENABLE, FALSE);
    dev->SetRenderState(D3DRS_COLORWRITEENABLE, 0xF);
    dev->SetRenderState(D3DRS_SRGBWRITEENABLE, FALSE);

    bool beganScene = !inScene && SUCCEEDED(dev->BeginScene());
    dev->DrawPrimitiveUP(D3DPT_TRIANGLELIST, (UINT)(verts.size() / 3), verts.data(), sizeof(V));
    if (beganScene) dev->EndScene();

    dev->SetViewport(&oldVp);
    saved->Apply();
    saved->Release();
}

}  // namespace lyrics
