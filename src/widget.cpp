#include "widget.h"

#include <commctrl.h>
#include <algorithm>
#include <cmath>
#include <windowsx.h>

namespace {

HWND g_wnd, g_taskbar, g_tip, g_notify, g_rebar;
UINT g_clickMsg;
bool g_light;
int g_hoveredItem = -1;
std::vector<WidgetItem> g_items;
std::wstring g_tipText;
RECT g_lastTray{}, g_lastBar{};
int g_lastW = -1, g_lastDpi = 0;
SIZE g_size{};

constexpr int kCellDip = 42;
constexpr int kRingDip = 34;
constexpr int kRingPaddingDip = 6;
const wchar_t* kClass = L"WirelessBatteryWidget";

int Dpi() {
    UINT d = g_taskbar ? GetDpiForWindow(g_taskbar) : 0;
    return d ? (int)d : 96;
}
int Px(int v) { return MulDiv(v, Dpi(), USER_DEFAULT_SCREEN_DPI); }
int CellSize(int barHeight) { return std::min(Px(kCellDip), barHeight - Px(4)); }
int RingDiameter(int cell) { return std::min(Px(kRingDip), cell - Px(kRingPaddingDip)); }

wchar_t Glyph(DeviceKind k, bool bluetooth) {
    switch (k) {
        case DeviceKind::Headset:  return 0xE7F6;  // Headphone
        case DeviceKind::Mouse:    return 0xE962;  // Mouse
        case DeviceKind::Keyboard: return 0xE765;  // KeyboardClassic
        case DeviceKind::Gamepad:  return 0xE7FC;  // Game
        default:                   return bluetooth ? 0xE702 : 0xE772;  // Bluetooth / Devices
    }
}
constexpr wchar_t kBatteryUnknown = 0xE996;  // shown when there is nothing to display

// Creates a font, falling back when the preferred face is not installed (GDI would silently substitute).
HFONT MakeFont(int px, int weight, const wchar_t* face, const wchar_t* fallback) {
    for (const wchar_t* f : { face, fallback }) {
        HFONT h = CreateFontW(-px, 0, 0, 0, weight, 0, 0, 0, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                              ANTIALIASED_QUALITY, DEFAULT_PITCH, f);
        HDC dc = CreateCompatibleDC(nullptr);
        HGDIOBJ o = SelectObject(dc, h);
        wchar_t got[LF_FACESIZE] = {};
        GetTextFaceW(dc, LF_FACESIZE, got);
        SelectObject(dc, o);
        DeleteDC(dc);
        if (!_wcsicmp(got, f) || f == fallback) return h;
        DeleteObject(h);
    }
    return nullptr;
}

std::vector<BYTE> RenderGlyph(wchar_t glyph, HFONT font, int cell) {
    std::vector<BYTE> mask((size_t)cell * cell);
    HDC dc = CreateCompatibleDC(nullptr);
    if (!dc) return mask;
    BITMAPINFO info{};
    info.bmiHeader = { sizeof(BITMAPINFOHEADER), cell, -cell, 1, 32, BI_RGB };
    void* bits = nullptr;
    HBITMAP bitmap = CreateDIBSection(dc, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (bitmap) {
        HGDIOBJ prevBitmap = SelectObject(dc, bitmap);
        HGDIOBJ prevFont = font ? SelectObject(dc, font) : nullptr;
        PatBlt(dc, 0, 0, cell, cell, BLACKNESS);
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, RGB(255, 255, 255));
        RECT bounds{0, 0, cell, cell};
        DrawTextW(dc, &glyph, 1, &bounds, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
        GdiFlush();
        const auto* pixels = static_cast<const DWORD*>(bits);
        std::transform(pixels, pixels + mask.size(), mask.begin(), [](DWORD pixel) { return (BYTE)(pixel & 0xff); });
        if (prevFont) SelectObject(dc, prevFont);
        SelectObject(dc, prevBitmap);
        DeleteObject(bitmap);
    }
    DeleteDC(dc);
    return mask;
}

struct Rgb { int r, g, b; };

struct Canvas {
    int w, h;
    std::vector<DWORD> px;  // premultiplied BGRA
    Canvas(int w_, int h_) : w(w_), h(h_), px((size_t)w_ * h_, 0) {}
    void Blend(int x, int y, Rgb c, int a) {  // source-over, a = 0..255
        if (x < 0 || y < 0 || x >= w || y >= h || a <= 0) return;
        DWORD d = px[y * w + x];
        int inv = 255 - a;
        int da = d >> 24, dr = (d >> 16) & 255, dg = (d >> 8) & 255, db = d & 255;
        int oa = a + da * inv / 255;
        int orr = c.r * a / 255 + dr * inv / 255;
        int og = c.g * a / 255 + dg * inv / 255;
        int ob = c.b * a / 255 + db * inv / 255;
        px[y * w + x] = ((DWORD)oa << 24) | ((DWORD)orr << 16) | ((DWORD)og << 8) | (DWORD)ob;
    }
};

void DrawBadge(Canvas& cv, int x, const std::vector<BYTE>& glyph, const WidgetItem& item, bool hovered) {
    const auto fg = g_light ? Rgb{0, 0, 0} : Rgb{255, 255, 255};
    const auto red = g_light ? Rgb{196, 30, 40} : Rgb{255, 90, 90};
    const auto charge = g_light ? Rgb{0, 120, 212} : Rgb{96, 205, 255};
    const auto accent = item.charging ? charge : item.low ? red : fg;
    const int opacity = item.dim ? 120 : 255;
    const int cell = cv.h;
    const int diameter = RingDiameter(cell);
    const double halfStroke = (double)Dpi() / USER_DEFAULT_SCREEN_DPI;
    const double radius = diameter / 2.0 - halfStroke;
    const double hoverRadius = (diameter + Px(kRingPaddingDip)) / 2.0;
    if (radius <= 0) return;
    constexpr double turn = 6.283185307179586;
    const int level = std::clamp(item.percent, 0, 100);
    const double arc = turn * level / 100.0;
    const auto coverage = [](double distance) { return std::clamp(0.5 - distance, 0.0, 1.0); };
    for (int row = 0; row < cell; ++row) {
        const double dy = row + 0.5 - cell / 2.0;
        for (int col = 0; col < cell; ++col) {
            const double dx = col + 0.5 - cell / 2.0;
            const double distance = std::hypot(dx, dy);
            const int hover = hovered ? (int)std::lround(21 * coverage(distance - hoverRadius)) : 0;
            cv.Blend(x + col, row, fg, 1 + hover);

            const double ring = coverage(std::abs(distance - radius) - halfStroke);
            if (ring > 0) {
                double angle = std::atan2(dx, -dy);
                if (angle < 0) angle += turn;
                const double edge = angle <= arc ? std::min(angle, arc - angle)
                    : -std::min(angle - arc, turn - angle);
                const double fill = level == 100 ? 1.0 : level == 0 ? 0.0 : coverage(-edge * radius);
                const double alpha = 60 + 195 * fill;
                const auto tint = [&](int base, int filled) {
                    return (int)std::lround(base + (filled - base) * 255 * fill / alpha);
                };
                const Rgb color{tint(fg.r, accent.r), tint(fg.g, accent.g), tint(fg.b, accent.b)};
                cv.Blend(x + col, row, color, (int)(ring * alpha * opacity / 255));
            }
            cv.Blend(x + col, row, item.charging && !level ? charge : fg, glyph[row * cell + col] * opacity / 255);
        }
    }
}

void Paint() {
    if (!g_wnd) return;
    RECT bar{};
    if (!GetClientRect(g_taskbar, &bar)) return;
    const int H = CellSize(bar.bottom - bar.top);
    if (RingDiameter(H) <= 0) return;
    const int W = H * (int)std::max(size_t{1}, g_items.size());
    Canvas cv(W, H);
    const int glyphSize = std::min(Px(19), RingDiameter(H) * 5 / 9);
    HFONT icons = MakeFont(glyphSize, FW_NORMAL, L"Segoe Fluent Icons", L"Segoe MDL2 Assets");
    if (g_items.empty()) {
        WidgetItem empty;
        empty.dim = true;
        DrawBadge(cv, 0, RenderGlyph(kBatteryUnknown, icons, H), empty, g_hoveredItem == 0);
    }
    for (size_t i = 0; i < g_items.size(); ++i) {
        const auto& item = g_items[i];
        DrawBadge(cv, (int)i * H, RenderGlyph(Glyph(item.kind, item.bluetooth), icons, H), item, g_hoveredItem == (int)i);
    }
    if (icons) DeleteObject(icons);

    HDC screen = GetDC(nullptr);
    HDC mem = CreateCompatibleDC(screen);
    BITMAPINFO bi{};
    bi.bmiHeader = { sizeof(BITMAPINFOHEADER), W, -H, 1, 32, BI_RGB };
    void* bits = nullptr;
    HBITMAP bmp = CreateDIBSection(mem, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    memcpy(bits, cv.px.data(), (size_t)W * H * 4);
    HGDIOBJ ob = SelectObject(mem, bmp);
    SIZE sz{ W, H };
    POINT src{ 0, 0 };
    RECT tray{}, taskbar{};
    HWND trayWindow = FindWindowExW(g_taskbar, nullptr, L"TrayNotifyWnd", nullptr);
    const bool havePosition = trayWindow && GetWindowRect(trayWindow, &tray) && GetWindowRect(g_taskbar, &taskbar);
    POINT position{tray.left - W - Px(4), taskbar.top + (taskbar.bottom - taskbar.top - H) / 2};
    BLENDFUNCTION bf{ AC_SRC_OVER, 0, 255, AC_SRC_ALPHA };
    // Publish the right-anchored bounds and the new strip together.
    if (UpdateLayeredWindow(g_wnd, screen, havePosition ? &position : nullptr, &sz, mem, &src, 0, &bf, ULW_ALPHA))
        g_size = sz;
    SelectObject(mem, ob);
    DeleteObject(bmp);
    DeleteDC(mem);
    ReleaseDC(nullptr, screen);
    g_lastW = -1;  // force a re-position with the new size
}

LRESULT CALLBACK WidgetProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_MOUSEMOVE: {
            const int index = std::max(0, GET_X_LPARAM(lp)) / std::max(1L, g_size.cy);
            if (index != g_hoveredItem) {
                if (g_hoveredItem < 0) {
                    TRACKMOUSEEVENT t{ sizeof(t), TME_LEAVE, h, 0 };
                    TrackMouseEvent(&t);
                }
                g_hoveredItem = index;
                Paint();
                widget::Reposition();
            }
            return 0;
        }
        case WM_MOUSELEAVE:
            g_hoveredItem = -1;
            Paint();
            widget::Reposition();
            return 0;
        case WM_LBUTTONUP:
        case WM_RBUTTONUP:
            PostMessageW(g_notify, g_clickMsg, 0, 0);
            return 0;
        case WM_MOUSEACTIVATE:
            return MA_NOACTIVATE;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

}  // namespace

namespace widget {

bool Create(HINSTANCE inst, HWND notifyWnd, UINT clickMsg, bool lightTheme) {
    Destroy();
    g_taskbar = FindWindowW(L"Shell_TrayWnd", nullptr);
    if (!g_taskbar || !FindWindowExW(g_taskbar, nullptr, L"TrayNotifyWnd", nullptr)) { g_taskbar = nullptr; return false; }
    RECT tb;
    GetWindowRect(g_taskbar, &tb);
    if (tb.bottom - tb.top > tb.right - tb.left) { g_taskbar = nullptr; return false; }  // vertical taskbar: use tray icons
    // Windows 11's taskbar is a XAML island; on Windows 10 the task buttons live in ReBarWindow32
    // and have to be narrowed so they never slide under the widget.
    g_rebar = FindWindowExW(g_taskbar, nullptr, L"Windows.UI.Composition.DesktopWindowContentBridge", nullptr)
                  ? nullptr
                  : FindWindowExW(g_taskbar, nullptr, L"ReBarWindow32", nullptr);
    g_notify = notifyWnd;
    g_clickMsg = clickMsg;
    g_light = lightTheme;

    static bool registered = false;
    if (!registered) {
        WNDCLASSEXW wc{ sizeof(wc) };
        wc.lpfnWndProc = WidgetProc;
        wc.hInstance = inst;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.lpszClassName = kClass;
        registered = RegisterClassExW(&wc) != 0;
    }
    g_wnd = CreateWindowExW(WS_EX_LAYERED | WS_EX_NOACTIVATE, kClass, L"", WS_CHILD | WS_CLIPSIBLINGS, 0, 0, 1, 1,
                            g_taskbar, nullptr, inst, nullptr);
    if (!g_wnd) { g_taskbar = nullptr; return false; }

    INITCOMMONCONTROLSEX icc{ sizeof(icc), ICC_WIN95_CLASSES };
    InitCommonControlsEx(&icc);
    g_tip = CreateWindowExW(WS_EX_TOPMOST, TOOLTIPS_CLASSW, nullptr, WS_POPUP | TTS_ALWAYSTIP | TTS_NOPREFIX, 0, 0, 0, 0,
                            notifyWnd, nullptr, inst, nullptr);
    if (g_tip) {
        TTTOOLINFOW ti{ sizeof(ti) };
        ti.uFlags = TTF_IDISHWND | TTF_SUBCLASS;
        ti.hwnd = notifyWnd;
        ti.uId = (UINT_PTR)g_wnd;
        ti.lpszText = const_cast<wchar_t*>(L"Wireless Battery");
        SendMessageW(g_tip, TTM_ADDTOOLW, 0, (LPARAM)&ti);
        SendMessageW(g_tip, TTM_SETMAXTIPWIDTH, 0, 600);
    }
    g_lastDpi = Dpi();
    Paint();
    g_lastW = -1;
    Reposition();
    ShowWindow(g_wnd, SW_SHOWNOACTIVATE);
    return true;
}

void Destroy() {
    if (g_rebar && IsWindow(g_rebar) && g_taskbar && IsWindow(g_taskbar)) {
        // Let Explorer lay the task buttons out again over the full width.
        RECT r;
        GetClientRect(g_taskbar, &r);
        PostMessageW(g_taskbar, WM_SIZE, SIZE_RESTORED, MAKELPARAM(r.right, r.bottom));
    }
    g_rebar = nullptr;
    if (g_tip) { DestroyWindow(g_tip); g_tip = nullptr; }
    if (g_wnd) { DestroyWindow(g_wnd); g_wnd = nullptr; }
    g_taskbar = nullptr;
    g_hoveredItem = -1;
}

bool Alive() { return g_wnd && IsWindow(g_wnd) && g_taskbar && IsWindow(g_taskbar); }

void SetItems(const std::vector<WidgetItem>& items, const std::wstring& tooltip) {
    g_items = items;
    if (tooltip != g_tipText && g_tip) {
        g_tipText = tooltip;
        TTTOOLINFOW ti{ sizeof(ti) };
        ti.hwnd = g_notify;
        ti.uId = (UINT_PTR)g_wnd;
        ti.lpszText = const_cast<wchar_t*>(g_tipText.c_str());
        SendMessageW(g_tip, TTM_UPDATETIPTEXTW, 0, (LPARAM)&ti);
    }
    Paint();
    Reposition();
}

void SetLightTheme(bool light) {
    g_light = light;
    Paint();
}

bool Reposition() {
    if (!Alive()) return false;
    HWND tray = FindWindowExW(g_taskbar, nullptr, L"TrayNotifyWnd", nullptr);
    if (!tray) return true;
    RECT tr{}, bar{};
    if (!GetWindowRect(tray, &tr) || !GetWindowRect(g_taskbar, &bar)) return true;
    const int dpi = Dpi();
    if (dpi != g_lastDpi || CellSize(bar.bottom - bar.top) != g_size.cy) {
        g_lastDpi = dpi;
        Paint();
    }
    POINT p{ tr.left, bar.top };
    ScreenToClient(g_taskbar, &p);
    // Reserve the tray-side slot for the battery strip. Volume Mixer treats this child as occupied.
    int left = p.x - g_size.cx - Px(4);
    int top = p.y + (bar.bottom - bar.top - g_size.cy) / 2;

    // Windows 10: Explorer re-expands the task buttons on its own layout passes; narrow them again.
    if (g_rebar && IsWindow(g_rebar)) {
        RECT rr;
        GetWindowRect(g_rebar, &rr);
        POINT tl{ rr.left, rr.top };
        ScreenToClient(g_taskbar, &tl);
        if (tl.x + (rr.right - rr.left) > left && left > tl.x)
            MoveWindow(g_rebar, tl.x, tl.y, left - tl.x, rr.bottom - rr.top, TRUE);
    }

    // Keep us above the taskbar's own content (our sibling windows).
    bool covered = GetWindow(g_wnd, GW_HWNDPREV) != nullptr;
    if (!covered && EqualRect(&tr, &g_lastTray) && EqualRect(&bar, &g_lastBar) && g_lastW == g_size.cx) return true;
    if (SetWindowPos(g_wnd, HWND_TOP, left, top, g_size.cx, g_size.cy, SWP_NOACTIVATE | SWP_SHOWWINDOW)) {
        g_lastTray = tr;
        g_lastBar = bar;
        g_lastW = g_size.cx;
    }
    return true;
}

}  // namespace widget
