/*
    an product writed by Huyalex in 2026
*/
#define _WIN32_WINNT 0x0601
#include <windows.h>
#include <gdiplus.h>
#include <windowsx.h>
#include <string>
#include <chrono>
#include <thread>
#include <algorithm>
#include <cmath>

using std::min;
using std::max;
using namespace Gdiplus;

#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "gdiplus.lib")
#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "user32.lib")

static const float POLL_SEC      = 1.0f;
static const int   FPS           = 120;
static const float STIFF         = 14.0f;
static const float DAMP          = 0.85f;

static const float NOTIF_WIDTH   = 360.f;
static const float NOTIF_HEIGHT  = 90.f;
static const float DISPLAY_TIME  = 5.0f;

struct Spring {
    float v = 0, x = 0, t = 0;
    void tick(float dt) {
        float f = (t - x) * STIFF - v * DAMP * STIFF;
        v += f * dt; x += v * dt;
    }
    void snap(float val) { x = v = t = val; }
};

enum class AnimState { Hidden, SlidingIn, Displaying, SlidingOut };

struct Track {
    std::wstring title, artist;
    bool playing = false;
};

inline AnimState g_animState = AnimState::Hidden;
inline Spring    g_XOffset;
inline Spring    g_Alpha;
inline Track     g_tr;
inline float     g_pollClock = 0.f;
inline float     g_lifeClock = 0.f; 
inline HWND      g_hwnd      = NULL;

struct SpotifyContext { DWORD pid = 0; bool found = false; std::wstring title, artist; };
static SpotifyContext sCtx;

static bool IsSpotifyProcess(DWORD pid) {
    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!h) return false;
    wchar_t path[MAX_PATH] = {}; DWORD sz = MAX_PATH;
    bool ok = QueryFullProcessImageNameW(h, 0, path, &sz) != 0;
    CloseHandle(h);
    if (!ok) return false;
    std::wstring p(path);
    auto sl = p.find_last_of(L"\\/");
    std::wstring nm = sl != std::wstring::npos ? p.substr(sl + 1) : p;
    for (auto& c : nm) c = towlower(c);
    return nm == L"spotify.exe";
}

BOOL CALLBACK EnumTopWindows(HWND hw, LPARAM) {
    if (!IsWindowVisible(hw)) return TRUE;
    DWORD pid = 0; GetWindowThreadProcessId(hw, &pid);
    if (!IsSpotifyProcess(pid)) return TRUE;
    sCtx.pid = pid;
    wchar_t buf[512] = {}; GetWindowTextW(hw, buf, 512);
    std::wstring wt(buf);
    if (wt.empty() || wt == L"Spotify" || wt == L"Spotify Premium" || wt == L"Spotify Free") return TRUE;
    auto sep = wt.find(L" - ");
    if (sep != std::wstring::npos) {
        sCtx.artist = wt.substr(0, sep);
        sCtx.title  = wt.substr(sep + 3);
    } else { sCtx.title = wt; sCtx.artist = L"Spotify"; }
    sCtx.found = true;
    return FALSE;
}

void TriggerNotification() {
    g_animState = AnimState::SlidingIn;
    g_XOffset.t = 0.f;
    g_Alpha.t   = 1.0f;
    g_lifeClock = DISPLAY_TIME;
}

void PollSpotify() {
    std::wstring prevTitle = g_tr.title;
    sCtx = {};
    EnumWindows(EnumTopWindows, 0);

    if (sCtx.found) {
        g_tr.playing = true; 
        if (sCtx.title != prevTitle) {
            g_tr.title  = sCtx.title;
            g_tr.artist = sCtx.artist;
            TriggerNotification();
        }
    } else {
        g_tr.playing = false;
    }
}

void SmoothRoundedRect(GraphicsPath& p, float x, float y, float w, float h, float r) {
    r = min(r, min(w / 2.f, h / 2.f));
    p.Reset();
    p.AddArc(x, y, r * 2.f, r * 2.f, 180, 90);
    p.AddArc(x + w - r * 2.f, y, r * 2.f, r * 2.f, 270, 90);
    p.AddArc(x + w - r * 2.f, y + h - r * 2.f, r * 2.f, r * 2.f, 0, 90);
    p.AddArc(x, y + h - r * 2.f, r * 2.f, r * 2.f, 90, 90);
    p.CloseFigure();
}

void DrawAlbumArt(Graphics& g, float x, float y, float sz, int alpha) {
    static Gdiplus::Image* defaultArt = nullptr;
    static Gdiplus::Image* easterEggArt = nullptr;
    
    if (!defaultArt) defaultArt = Gdiplus::Image::FromFile(L"Spotify.png");
    if (!easterEggArt) easterEggArt = Gdiplus::Image::FromFile(L"Keo.png");
    
    Gdiplus::Image* currentArt = defaultArt;
    if (g_tr.artist == L"Thắng" || g_tr.artist == L"Ngọt") {
        currentArt = easterEggArt;
    }
    
    if (currentArt && currentArt->GetLastStatus() == Ok) {
        GraphicsPath path;
        SmoothRoundedRect(path, x, y, sz, sz, 12.f);
        
        GraphicsState state = g.Save();
        g.SetClip(&path);
        g.DrawImage(currentArt, x, y, sz, sz);
        g.Restore(state);
    } else {
        SolidBrush disc(Color(alpha, 34, 34, 38));
        GraphicsPath dp; SmoothRoundedRect(dp, x, y, sz, sz, 12.f);
        g.FillPath(&disc, &dp);
    }
}

void RenderNotification(HWND hwnd) {
    if (g_animState == AnimState::Hidden) return;

    RECT rc; GetClientRect(hwnd, &rc);
    int SW = rc.right, SH = rc.bottom;

    HDC hdc = GetDC(hwnd);
    HDC memDC = CreateCompatibleDC(hdc);
    HBITMAP bmp = CreateCompatibleBitmap(hdc, SW, SH);
    SelectObject(memDC, bmp);

    Graphics g(memDC);
    g.SetSmoothingMode(SmoothingModeAntiAlias);
    g.SetTextRenderingHint(TextRenderingHintAntiAliasGridFit);
    g.Clear(Color(0, 0, 0, 0)); 

    int alphaChannel = (int)(max(0.f, min(1.f, g_Alpha.x)) * 255.f);
    
    if (alphaChannel > 0) {
        float xPos = SW - NOTIF_WIDTH - 20.f + g_XOffset.x;
        float yPos = 25.f; 

        for (int i = 5; i >= 1; i--) {
            int shadowAlpha = (int)(alphaChannel * 0.04f * (6 - i));
            GraphicsPath sp; SmoothRoundedRect(sp, xPos - i, yPos - i * 0.5f, NOTIF_WIDTH + (i * 2), NOTIF_HEIGHT + i, 16.f);
            SolidBrush sb(Color(shadowAlpha, 0, 0, 0));
            g.FillPath(&sb, &sp);
        }

        {
            GraphicsPath body; SmoothRoundedRect(body, xPos, yPos, NOTIF_WIDTH, NOTIF_HEIGHT, 16.f);
            SolidBrush bodyBrush(Color(alphaChannel, 14, 14, 16)); 
            g.FillPath(&bodyBrush, &body);
        }

        {
            float pad = 15.f;
            float artSize = NOTIF_HEIGHT - (pad * 2.f);
            DrawAlbumArt(g, xPos + pad, yPos + pad, artSize, alphaChannel);

            float textX = xPos + pad + artSize + 15.f;
            float textW = NOTIF_WIDTH - (pad * 2.f) - artSize - 15.f;

            FontFamily fontFamily(L"Segoe UI");
            Gdiplus::Font songFont(&fontFamily, 13, FontStyleBold, UnitPixel);
            Gdiplus::Font artistFont(&fontFamily, 11, FontStyleRegular, UnitPixel);

            SolidBrush whiteBrush(Color(alphaChannel, 255, 255, 255));
            SolidBrush greenBrush(Color(alphaChannel, 30, 215, 96)); 

            StringFormat sf; 
            sf.SetTrimming(StringTrimmingEllipsisCharacter);
            sf.SetFormatFlags(StringFormatFlagsNoWrap);

            std::wstring nameStr   = L"Name: " + g_tr.title;
            std::wstring artistStr = L"Artist: " + g_tr.artist;

            g.DrawString(nameStr.c_str(), -1, &songFont, RectF(textX, yPos + 24.f, textW, 18.f), &sf, &whiteBrush);
            g.DrawString(artistStr.c_str(), -1, &artistFont, RectF(textX, yPos + 46.f, textW, 16.f), &sf, &greenBrush);
        }

        BLENDFUNCTION bf = { AC_SRC_OVER, 0, (BYTE)alphaChannel, AC_SRC_ALPHA };
        POINT ptSrc = { 0, 0 }; SIZE sizeWnd = { SW, SH };
        UpdateLayeredWindow(hwnd, hdc, NULL, &sizeWnd, memDC, &ptSrc, 0, &bf, ULW_ALPHA);
    }
    
    ReleaseDC(hwnd, hdc);
    DeleteObject(bmp);
    DeleteDC(memDC);
}

void UpdateStatePipeline(float dt) {
    switch (g_animState) {
        case AnimState::SlidingIn:
            if (g_Alpha.x > 0.01f && !IsWindowVisible(g_hwnd)) {
                ShowWindow(g_hwnd, SW_SHOWNOACTIVATE);
            }
            if (fabsf(g_XOffset.x - g_XOffset.t) < 0.5f) {
                g_animState = AnimState::Displaying;
            }
            break;

        case AnimState::Displaying:
            g_lifeClock -= dt;
            if (g_lifeClock <= 0.f) {
                g_animState = AnimState::SlidingOut;
                g_XOffset.t = 150.f;
                g_Alpha.t   = 0.0f;
            }
            break;

        case AnimState::SlidingOut:
            if (g_Alpha.x <= 0.005f && fabsf(g_XOffset.x - g_XOffset.t) < 1.0f) {
                g_animState = AnimState::Hidden;
                ShowWindow(g_hwnd, SW_HIDE);
            }
            break;

        case AnimState::Hidden:
            break;
    }

    g_XOffset.tick(dt);
    g_Alpha.tick(dt);
}

LRESULT CALLBACK WndProc(HWND hw, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_NCHITTEST:
            return HTTRANSPARENT; 
        case WM_KEYDOWN:
            if (wp == VK_ESCAPE) { PostQuitMessage(0); return 0; }
            break;
        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProc(hw, msg, wp, lp);
}

int WINAPI WinMain(HINSTANCE hInst, HINSTANCE, LPSTR, int nShow) {
    ULONG_PTR tok; GdiplusStartupInput gsi;
    GdiplusStartup(&tok, &gsi, NULL);

    WNDCLASSW wc = {};
    wc.lpfnWndProc = WndProc; wc.hInstance = hInst;
    wc.lpszClassName = L"NotifyOverlayWin";
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    RegisterClassW(&wc);

    int scrW = GetSystemMetrics(SM_CXSCREEN);

    g_hwnd = CreateWindowExW(
        WS_EX_TOPMOST | WS_EX_LAYERED | WS_EX_TOOLWINDOW | WS_EX_TRANSPARENT,
        L"NotifyOverlayWin", L"Spotify Notify", WS_POPUP,
        0, 0, scrW, 250, NULL, NULL, hInst, NULL);

    ShowWindow(g_hwnd, SW_HIDE);

    g_XOffset.snap(150.f);
    g_Alpha.snap(0.f);

    PollSpotify();

    auto lastTime = std::chrono::high_resolution_clock::now();
    MSG msg = {};
    const int targetFrameMs = 1000 / FPS;

    while (msg.message != WM_QUIT) {
        auto frameStart = std::chrono::high_resolution_clock::now();
        
        while (PeekMessage(&msg, NULL, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) break;
            TranslateMessage(&msg); DispatchMessage(&msg);
        }

        auto now = std::chrono::high_resolution_clock::now();
        float dt = std::chrono::duration<float>(now - lastTime).count();
        lastTime = now;
        if (dt > 0.05f) dt = 0.05f;

        g_pollClock += dt;
        if (g_pollClock >= POLL_SEC) {
            g_pollClock = 0.f;
            PollSpotify();
        }

        UpdateStatePipeline(dt);
        RenderNotification(g_hwnd);

        long long executionTime = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::high_resolution_clock::now() - frameStart).count();
        int sleepDuration = targetFrameMs - (int)executionTime;
        if (sleepDuration > 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(sleepDuration));
        }
    }

    GdiplusShutdown(tok);
    return 0;
}