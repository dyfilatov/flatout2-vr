// Standalone ASI entry point (FO2Lyrics.asi): works in flat-screen FlatOut 2 on its own and alongside
// FO2VR.asi in VR (draws in the game's 2D pass before EndScene, which FO2VR captures onto the HUD panel
// in races and onto the virtual screen in menus).
#include "game.h"
#include "lyrics.h"
#include "util.h"

#include <windows.h>
#include <d3d9.h>

namespace {

bool SettingsDialog()
{
    const wchar_t* p = GetCommandLineW();
    if (*p == L'"')
    {
        p++;
        while (*p && *p != L'"') p++;
        if (*p) p++;
    }
    else
        while (*p > L' ') p++;
    while (*p && *p <= L' ') p++;
    return _wcsicmp(p, L"setup") == 0 || _wcsicmp(p, L"-setup") == 0;
}

using EndFrameFn = void(__thiscall*)(void* self);
using FlushSpritesFn = void(__thiscall*)(void* self, int mode);

EndFrameFn oEndFrame = (EndFrameFn)fo2::kEndFrame;
uintptr_t oPreReset = fo2::kPreReset;
bool g_keyWasDown = false;
double g_lastTick = 0.0;

bool GameWindowFocused()
{
    HWND fg = GetForegroundWindow();
    if (!fg) return false;
    DWORD pid = 0;
    GetWindowThreadProcessId(fg, &pid);
    return pid == GetCurrentProcessId();
}

bool VrUnfocusedHotkeysAllowed()
{
    if (!GetModuleHandleW(L"FO2VR.asi")) return false;
    std::wstring vrIni = ModuleDir() + L"FO2VR.ini";
    return GetPrivateProfileIntW(L"VR", L"Enabled", 1, vrIni.c_str()) != 0 &&
           GetPrivateProfileIntW(L"Test", L"Background", 0, vrIni.c_str()) == 0;
}

bool KeyPressed(int vk, bool& wasDown)
{
    bool allowed = GameWindowFocused() || VrUnfocusedHotkeysAllowed();
    bool down = vk > 0 && allowed && (GetAsyncKeyState(vk) & 0x8000) != 0;
    bool pressed = down && !wasDown;
    wasDown = down;
    return pressed;
}

void __fastcall EndFrameHook(void* self, void*)
{
    double now = NowSeconds();
    lyrics::Tick(g_lastTick > 0.0 ? now - g_lastTick : 0.0);
    g_lastTick = now;

    if (KeyPressed(g_cfg.keyLyrics, g_keyWasDown)) lyrics::Toggle();

    IDirect3DDevice9* dev = *(IDirect3DDevice9**)fo2::kD3DDevice;
    if (dev && g_cfg.lyricsEnabled && lyrics::CurrentShow().count > 0 && dev->TestCooperativeLevel() == D3D_OK)
    {
        // Flush the game's queued 2D sprites first so lyrics draw over the HUD and menus, while still
        // inside BeginScene..EndScene (captured onto g_hud.rt when FO2VR's HUD panel is active).
        void** vtbl = *(void***)self;
        auto flushSprites = (FlushSpritesFn)vtbl[fo2::kSlotFlushSprites / sizeof(void*)];
        flushSprites(self, 3);

        IDirect3DSurface9* rt = nullptr;
        if (SUCCEEDED(dev->GetRenderTarget(0, &rt)) && rt)
        {
            D3DSURFACE_DESC d = {};
            if (SUCCEEDED(rt->GetDesc(&d)) && d.Width > 0 && d.Height > 0)
                lyrics::Draw(dev, (float)d.Width, (float)d.Height, nullptr, true);
            rt->Release();
        }
    }

    oEndFrame(self);
}

static __declspec(naked) void PreResetHook()
{
    __asm {
        pushad
        call lyrics::ReleaseFont
        popad
        jmp dword ptr [oPreReset]
    }
}

void Init()
{
    if (SettingsDialog()) return;
    LogInit(ModuleDir() + L"FO2Lyrics.log");
    LoadConfig(L"FO2Lyrics.ini");

    char exe[MAX_PATH];
    GetModuleFileNameA(nullptr, exe, MAX_PATH);
    const BYTE* img = (const BYTE*)GetModuleHandleA(nullptr);
    const IMAGE_NT_HEADERS* nt = (const IMAGE_NT_HEADERS*)(img + ((const IMAGE_DOS_HEADER*)img)->e_lfanew);
    Log("FO2Lyrics build %s (standalone)", BuildStamp());
    Log("game: %s, timestamp %08X, image size %08X, entry %08X", exe, (unsigned)nt->FileHeader.TimeDateStamp,
        (unsigned)nt->OptionalHeader.SizeOfImage, (unsigned)nt->OptionalHeader.AddressOfEntryPoint);

    if (!PatchPointer(fo2::kSlotEndFrame, fo2::kEndFrame, (const void*)&EndFrameHook) || !lyrics::Install())
    {
        Log("unsupported FlatOut2.exe (needs v1.2 DRM-free) - mod inactive");
        return;
    }
    if (!PatchCall(fo2::kPreResetCall, fo2::kPreReset, (const void*)&PreResetHook))
        Log("lyrics: device reset hook differs - managed font texture used");

    Log("config: Lyrics %d (%d lines, scale %.2f, y %.2f, colours %06X/%06X, key %d)", g_cfg.lyricsEnabled ? 1 : 0,
        g_cfg.lyricsLines, g_cfg.lyricsScale, g_cfg.lyricsY, g_cfg.lyricsColour, g_cfg.lyricsSungColour, g_cfg.keyLyrics);
}

}  // namespace

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH)
    {
        DisableThreadLibraryCalls(inst);
        Init();
    }
    return TRUE;
}
