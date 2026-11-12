#pragma once
// The songs' lyrics, karaoke style, on the HUD panel in VR or on the screen without VR (see lyrics.cpp).
#include <cstdint>
#include <string>
#include <vector>

struct IDirect3DDevice9;
struct IDirect3DTexture9;

namespace lyrics {

// Hooks the game's music play (its playlist advance). False when the game's code differs or when
// standalone FO2Lyrics.asi is already installed beside FO2VR.asi.
bool Install();
bool Active();

// Advances the song's clock and picks the lines to show; called once per frame with the seconds since
// the last call.
void Tick(double dt);

// Toggles lyrics display on/off and writes the setting to the active .ini file.
void Toggle();

// The lines of the playing song, in order, with the words not yet sung and those already sung (when the
// .lrc file tells each word's time): up to `lyricsLines` lines around the song's position, each faded in
// and out over its first and last kFade seconds. An empty Show when nothing is to show.
struct Word
{
    std::wstring text;  // with the trailing space when the line keeps it
    double at = 0;      // when it is sung, seconds into the song
};
struct Line
{
    std::wstring plain;       // the line's text, measured and drawn
    double at = 0;            // when it is sung, seconds into the song
    double until = 0;         // the next line's time (its own fade starts there)
    std::vector<Word> words;  // the words with their own times (empty: line-timed only)
};
struct Show
{
    Line lines[6];
    int count = 0;
};
const Show& CurrentShow();

// Stereo projection onto the virtual HUD panel plane in front of the recenter origin, used in VR when
// the HUD panel swapchain itself is not shown (Hud=0, or Hud=2 while racing).
struct PanelView
{
    float eye[3];                         // eye position relative to recenter origin, metres
    float right[3], up[3], forward[3];    // eye axes in head space
    float left, rightTan, down, upTan;    // eye frustum tangents
    float panelDist;                      // metres in front of recenter origin
    float panelWidth, panelHeight;        // metres
    float panelY;                         // vertical centre offset, metres
};

// Releases the font atlas texture and vertex declaration (safe on device reset and small thread stacks).
void ReleaseFont();

// Draws the lyrics into the bound render target (w x h pixels): flat in 2D when `panel` is null, or
// projected onto the 3D panel plane in stereo when `panel` is given. Set `inScene` when called inside
// an already-open IDirect3DDevice9::BeginScene..EndScene block.
void Draw(IDirect3DDevice9* dev, float w, float h, const PanelView* panel = nullptr, bool inScene = false);

}  // namespace lyrics
