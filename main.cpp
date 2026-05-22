/*
    an product writed by Huyalex in 2026
*/
#define _WIN32_WINNT 0x0601
#include <windows.h>
#include <gdiplus.h>
#include <windowsx.h>
#include <string>
#include <vector>
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

static const float POLL_SEC    = 1.0f;
static const int   FPS         = 120;

static const float SLOT_STIFF  = 20.0f;
static const float SLOT_DAMP   = 1.00f;

static const float ENTRY_STIFF = 32.0f;
static const float ENTRY_DAMP  = 0.82f;

static const float NOTIF_W     = 360.f;
static const float NOTIF_H     = 90.f;
static const float GAP         = 10.f;
static const float MARGIN_TOP  = 20.f;
static const float MARGIN_RIGHT = 20.f;
static const float DISPLAY_SEC = 5.0f;

static const float SLIDE_RIGHT = NOTIF_W + MARGIN_RIGHT + 30.f;

struct Spring {
    float v=0, x=0, t=0;
    float stiff, damp;
    Spring(float st, float da) : stiff(st), damp(da) {}
    void tick(float dt) {
        float f = (t-x)*stiff - v*damp*stiff;
        v += f*dt; x += v*dt;
    }
    void snap(float val) { x=v=t=val; }
    bool settled(float eps=0.5f) const { return fabsf(x-t)<eps && fabsf(v)<eps; }
};

enum class CardState { SlidingIn, Displaying, SlidingOut, Dead };
enum class Source    { Spotify, Discord };

struct NotifCard {
    Source       source;
    std::wstring line1, line2;
    CardState    state = CardState::SlidingIn;
    float        life  = DISPLAY_SEC;

    Spring ySlot  { SLOT_STIFF,  SLOT_DAMP  };
    Spring xEntry { ENTRY_STIFF, ENTRY_DAMP };
    Spring alpha  { ENTRY_STIFF, ENTRY_DAMP };

    NotifCard(Source src, std::wstring l1, std::wstring l2)
        : source(src), line1(std::move(l1)), line2(std::move(l2))
    {
        xEntry.snap(SLIDE_RIGHT);
        xEntry.t = 0.f;

        alpha.snap(0.f);
        alpha.t = 1.f;
    }

    float screenY() const { return ySlot.x; }

    bool dead() const { return state == CardState::Dead; }

    void tick(float dt) {
        switch (state) {
            case CardState::SlidingIn:
                if (xEntry.settled(0.5f))
                    state = CardState::Displaying;
                break;
            case CardState::Displaying:
                life -= dt;
                if (life <= 0.f) {
                    state    = CardState::SlidingOut;
                    xEntry.t = SLIDE_RIGHT;
                    alpha.t  = 0.f;
                }
                break;
            case CardState::SlidingOut:
                if (alpha.x <= 0.005f && xEntry.settled(1.f))
                    state = CardState::Dead;
                break;
            case CardState::Dead: break;
        }
        ySlot .tick(dt);
        xEntry.tick(dt);
        alpha .tick(dt);
    }
};

static std::vector<NotifCard> g_cards;
static HWND  g_hwnd      = NULL;
static float g_pollClock = 0.f;

static std::wstring g_lastSpotTitle;
static std::wstring g_lastDcServer, g_lastDcChannel;
static int          g_lastDcUnread = 0;
static std::wstring g_spArtist;

static Gdiplus::Image* g_imgSpot    = nullptr;
static Gdiplus::Image* g_imgEgg     = nullptr;
static Gdiplus::Image* g_imgDiscord = nullptr;

static Gdiplus::Image* PickArt(const NotifCard& c) {
    if (c.source == Source::Discord) {
        if (!g_imgDiscord) g_imgDiscord = Gdiplus::Image::FromFile(L"Discord.png");
        return g_imgDiscord;
    }
    if (g_spArtist == L"Thắng" || g_spArtist == L"Ngọt") {
        if (!g_imgEgg) g_imgEgg = Gdiplus::Image::FromFile(L"Keo.png");
        return g_imgEgg;
    }
    if (!g_imgSpot) g_imgSpot = Gdiplus::Image::FromFile(L"Spotify.png");
    return g_imgSpot;
}

static void RoundedRect(GraphicsPath& p, float x, float y, float w, float h, float r) {
    r = min(r, min(w/2.f, h/2.f));
    p.Reset();
    p.AddArc(x,       y,       r*2,r*2, 180, 90);
    p.AddArc(x+w-r*2, y,       r*2,r*2, 270, 90);
    p.AddArc(x+w-r*2, y+h-r*2, r*2,r*2,   0, 90);
    p.AddArc(x,       y+h-r*2, r*2,r*2,  90, 90);
    p.CloseFigure();
}

static void Spawn(Source src, std::wstring l1, std::wstring l2) {
    int live = 0;
    for (auto& c : g_cards) if (!c.dead()) live++;

    float slotY = MARGIN_TOP + live * (NOTIF_H + GAP);

    NotifCard card(src, std::move(l1), std::move(l2));
    card.ySlot.snap(slotY);
    card.ySlot.t = slotY;
    g_cards.push_back(std::move(card));
}

static bool IsExe(DWORD pid, const wchar_t* name) {
    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!h) return false;
    wchar_t path[MAX_PATH]={}; DWORD sz=MAX_PATH;
    bool ok = QueryFullProcessImageNameW(h, 0, path, &sz) != 0;
    CloseHandle(h);
    if (!ok) return false;
    std::wstring p(path);
    auto sl = p.find_last_of(L"\\/");
    std::wstring nm = sl!=std::wstring::npos ? p.substr(sl+1) : p;
    std::wstring want(name);
    for (auto& c:nm)   c=towlower(c);
    for (auto& c:want) c=towlower(c);
    return nm==want;
}

struct SpCtx { bool found=false; std::wstring title,artist; };
static SpCtx g_sp;
BOOL CALLBACK EnumSpotify(HWND hw, LPARAM) {
    if (!IsWindowVisible(hw)) return TRUE;
    DWORD pid=0; GetWindowThreadProcessId(hw,&pid);
    if (!IsExe(pid,L"Spotify.exe")) return TRUE;
    wchar_t buf[512]={}; GetWindowTextW(hw,buf,512);
    std::wstring wt(buf);
    if (wt.empty()||wt==L"Spotify"||wt==L"Spotify Premium"||wt==L"Spotify Free") return TRUE;
    auto sep=wt.find(L" - ");
    if (sep!=std::wstring::npos){g_sp.artist=wt.substr(0,sep);g_sp.title=wt.substr(sep+3);}
    else{g_sp.title=wt;g_sp.artist=L"Spotify";}
    g_sp.found=true; return FALSE;
}
static void PollSpotify() {
    g_sp={}; EnumWindows(EnumSpotify,0);
    if (!g_sp.found) return;
    g_spArtist=g_sp.artist;
    if (g_sp.title!=g_lastSpotTitle) {
        g_lastSpotTitle=g_sp.title;
        Spawn(Source::Spotify, L"Track: "+g_sp.title, L"Artist: "+g_sp.artist);
    }
}

struct DcCtx { bool found=false; std::wstring server,channel; int unread=0; };
static DcCtx g_dc;
BOOL CALLBACK EnumDiscord(HWND hw, LPARAM) {
    if (!IsWindowVisible(hw)) return TRUE;
    DWORD pid=0; GetWindowThreadProcessId(hw,&pid);
    if (!IsExe(pid,L"Discord.exe")) return TRUE;
    wchar_t buf[512]={}; GetWindowTextW(hw,buf,512);
    std::wstring wt(buf);
    if (wt.empty()||wt==L"Discord") return TRUE;
    int unread=0;
    if (!wt.empty()&&wt.back()==L')') {
        auto lp=wt.rfind(L'(');
        if (lp!=std::wstring::npos&&lp>1&&wt[lp-1]==L' ') {
            std::wstring num=wt.substr(lp+1,wt.size()-lp-2);
            bool ok=!num.empty();
            for (wchar_t c:num) if(!iswdigit(c)){ok=false;break;}
            if (ok){unread=_wtoi(num.c_str());wt=wt.substr(0,lp-1);}
        }
    }
    auto sep=wt.find(L" \u2014 #");
    if (sep==std::wstring::npos) return TRUE;
    g_dc.server=wt.substr(0,sep); g_dc.channel=wt.substr(sep+4);
    g_dc.unread=unread; g_dc.found=true; return FALSE;
}
static void PollDiscord() {
    g_dc={}; EnumWindows(EnumDiscord,0);
    if (!g_dc.found||g_dc.unread<=0){g_lastDcUnread=0;return;}
    bool isNew=(g_dc.server!=g_lastDcServer||g_dc.channel!=g_lastDcChannel||g_dc.unread>g_lastDcUnread);
    g_lastDcServer=g_dc.server; g_lastDcChannel=g_dc.channel; g_lastDcUnread=g_dc.unread;
    if (!isNew) return;
    Spawn(Source::Discord,
          g_dc.server+L" \u2014 #"+g_dc.channel,
          std::to_wstring(g_dc.unread)+L" unread message"+(g_dc.unread==1?L"":L"s"));
}

static void DrawCard(Graphics& g, const NotifCard& card, int scrW) {
    int a = (int)(max(0.f,min(1.f,card.alpha.x))*255.f);
    if (a<=0) return;

    float restX = (float)scrW - NOTIF_W - MARGIN_RIGHT;
    float xPos  = restX + card.xEntry.x;
    float yPos  = card.screenY();

    for (int i=5;i>=1;i--) {
        int sa=(int)(a*0.04f*(6-i));
        GraphicsPath sp; RoundedRect(sp,xPos-i,yPos-i*0.5f,NOTIF_W+i*2,NOTIF_H+i,16.f);
        SolidBrush sb(Color(sa,0,0,0)); g.FillPath(&sb,&sp);
    }
    { GraphicsPath body; RoundedRect(body,xPos,yPos,NOTIF_W,NOTIF_H,16.f);
      SolidBrush bb(Color(a,14,14,16)); g.FillPath(&bb,&body); }

    float pad=15.f, artSz=NOTIF_H-pad*2.f;
    { Gdiplus::Image* img=PickArt(card);
      GraphicsPath clip; RoundedRect(clip,xPos+pad,yPos+pad,artSz,artSz,12.f);
      if (img&&img->GetLastStatus()==Ok) {
          GraphicsState st=g.Save(); g.SetClip(&clip);
          g.DrawImage(img,xPos+pad,yPos+pad,artSz,artSz); g.Restore(st);
      } else {
          BYTE r=34,gb=34,b=38;
          if (card.source==Source::Discord){r=88;gb=101;b=242;}
          SolidBrush fb(Color(a,r,gb,b)); g.FillPath(&fb,&clip);
      }
    }

    float tx=xPos+pad+artSz+15.f, tw=NOTIF_W-pad*2.f-artSz-15.f;
    FontFamily ff(L"Segoe UI");
    Gdiplus::Font fLabel(&ff,10,FontStyleRegular,UnitPixel);
    Gdiplus::Font fLine1(&ff,13,FontStyleBold,   UnitPixel);
    Gdiplus::Font fLine2(&ff,11,FontStyleRegular,UnitPixel);
    SolidBrush bWhite (Color(a,255,255,255));
    SolidBrush bGray  (Color(a,140,140,150));
    SolidBrush bAccent(card.source==Source::Spotify
        ? Color(a,30,215,96) : Color(a,88,101,242));
    StringFormat sf;
    sf.SetTrimming(StringTrimmingEllipsisCharacter);
    sf.SetFormatFlags(StringFormatFlagsNoWrap);

    const wchar_t* lbl=card.source==Source::Spotify
        ? L"SPOTIFY \u00b7 NOW PLAYING" : L"DISCORD \u00b7 MESSAGE";
    g.DrawString(lbl,              -1,&fLabel,RectF(tx,yPos+12.f,tw,14.f),&sf,&bGray);
    g.DrawString(card.line1.c_str(),-1,&fLine1,RectF(tx,yPos+30.f,tw,18.f),&sf,&bWhite);
    g.DrawString(card.line2.c_str(),-1,&fLine2,RectF(tx,yPos+52.f,tw,16.f),&sf,&bAccent);
}
static void Render(HWND hwnd) {
    bool any=false;
    for (auto& c:g_cards) if (!c.dead()&&c.alpha.x>0.001f){any=true;break;}
    if (!any) return;

    RECT rc; GetClientRect(hwnd,&rc);
    int SW=rc.right, SH=rc.bottom;
    HDC hdc=GetDC(hwnd);
    HDC memDC=CreateCompatibleDC(hdc);
    HBITMAP bmp=CreateCompatibleBitmap(hdc,SW,SH);
    SelectObject(memDC,bmp);

    Graphics g(memDC);
    g.SetSmoothingMode(SmoothingModeAntiAlias);
    g.SetTextRenderingHint(TextRenderingHintAntiAliasGridFit);
    g.Clear(Color(0,0,0,0));

    for (auto& card:g_cards) DrawCard(g,card,SW);

    int maxA=0;
    for (auto& c:g_cards) maxA=max(maxA,(int)(max(0.f,min(1.f,c.alpha.x))*255.f));
    if (maxA>0) {
        BLENDFUNCTION bf={AC_SRC_OVER,0,(BYTE)maxA,AC_SRC_ALPHA};
        POINT pt={0,0}; SIZE sz={SW,SH};
        UpdateLayeredWindow(hwnd,hdc,NULL,&sz,memDC,&pt,0,&bf,ULW_ALPHA);
    }
    ReleaseDC(hwnd,hdc); DeleteObject(bmp); DeleteDC(memDC);
}

static void Update(float dt) {
    for (auto& c:g_cards) c.tick(dt);

    int slot=0;
    for (auto& c:g_cards) {
        if (c.dead() || c.state==CardState::SlidingOut) continue;
        c.ySlot.t = MARGIN_TOP + slot*(NOTIF_H+GAP);
        slot++;
    }

    g_cards.erase(
        std::remove_if(g_cards.begin(),g_cards.end(),
                       [](const NotifCard& c){return c.dead();}),
        g_cards.end());

    bool vis=false;
    for (auto& c:g_cards) if (c.alpha.x>0.001f){vis=true;break;}
    if (vis) { if (!IsWindowVisible(g_hwnd)) ShowWindow(g_hwnd,SW_SHOWNOACTIVATE); }
    else     { ShowWindow(g_hwnd,SW_HIDE); }
}

LRESULT CALLBACK WndProc(HWND hw,UINT msg,WPARAM wp,LPARAM lp) {
    if (msg==WM_NCHITTEST) return HTTRANSPARENT;
    if (msg==WM_KEYDOWN&&wp==VK_ESCAPE){PostQuitMessage(0);return 0;}
    if (msg==WM_DESTROY){PostQuitMessage(0);return 0;}
    return DefWindowProc(hw,msg,wp,lp);
}

int WINAPI WinMain(HINSTANCE hInst,HINSTANCE,LPSTR,int) {
    ULONG_PTR tok; GdiplusStartupInput gsi;
    GdiplusStartup(&tok,&gsi,NULL);

    WNDCLASSW wc={};
    wc.lpfnWndProc=WndProc; wc.hInstance=hInst;
    wc.lpszClassName=L"NotifyOverlayWin";
    wc.hCursor=LoadCursor(NULL,IDC_ARROW);
    RegisterClassW(&wc);

    int scrW=GetSystemMetrics(SM_CXSCREEN);
    int winH=(int)(MARGIN_TOP+6*(NOTIF_H+GAP)+40);

    g_hwnd=CreateWindowExW(
        WS_EX_TOPMOST|WS_EX_LAYERED|WS_EX_TOOLWINDOW|WS_EX_TRANSPARENT,
        L"NotifyOverlayWin",L"Notify Overlay",WS_POPUP,
        0,0,scrW,winH,NULL,NULL,hInst,NULL);
    ShowWindow(g_hwnd,SW_HIDE);

    { g_sp={}; EnumWindows(EnumSpotify,0);
      if (g_sp.found){g_lastSpotTitle=g_sp.title;g_spArtist=g_sp.artist;}
      g_dc={}; EnumWindows(EnumDiscord,0);
      if (g_dc.found){g_lastDcServer=g_dc.server;g_lastDcChannel=g_dc.channel;g_lastDcUnread=g_dc.unread;} }

    auto lastTime=std::chrono::high_resolution_clock::now();
    MSG msg={}; const int tgtMs=1000/FPS;
    while (msg.message!=WM_QUIT) {
        auto frameStart=std::chrono::high_resolution_clock::now();
        while (PeekMessage(&msg,NULL,0,0,PM_REMOVE)){
            if (msg.message==WM_QUIT) break;
            TranslateMessage(&msg); DispatchMessage(&msg);
        }
        auto now=std::chrono::high_resolution_clock::now();
        float dt=std::chrono::duration<float>(now-lastTime).count();
        lastTime=now; if (dt>0.05f) dt=0.05f;

        g_pollClock+=dt;
        if (g_pollClock>=POLL_SEC){g_pollClock=0.f;PollSpotify();PollDiscord();}

        Update(dt);
        Render(g_hwnd);

        long long ex=std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::high_resolution_clock::now()-frameStart).count();
        int sl=tgtMs-(int)ex; if (sl>0) std::this_thread::sleep_for(std::chrono::milliseconds(sl));
    }
    GdiplusShutdown(tok); return 0;
}