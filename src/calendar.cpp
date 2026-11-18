// Calendar — a small native Windows calendar widget.
// Win32 + Direct2D + DirectWrite + DWM acrylic backdrop. No runtimes required.

#ifndef UNICODE
#define UNICODE
#endif
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <windowsx.h>
#include <dwmapi.h>
#include <shellscalingapi.h>
#include <d2d1.h>
#include <dwrite.h>
#include <string>
#include <vector>
#include <cmath>
#include <cwctype>
#include <climits>
#include <algorithm>

#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "shcore.lib")
#pragma comment(lib, "d2d1.lib")
#pragma comment(lib, "dwrite.lib")

template <class T> static void SafeRelease(T*& p) { if (p) { p->Release(); p = nullptr; } }

// ---------------------------------------------------------------------------
// Date math (serial day numbers, 0 = 1970-01-01)
// ---------------------------------------------------------------------------
static int Mod(int a, int b) { int r = a % b; return r < 0 ? r + b : r; }

static int DaysFromCivil(int y, int m, int d) {
    y -= m <= 2;
    const int era = (y >= 0 ? y : y - 399) / 400;
    const int yoe = y - era * 400;
    const int doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const int doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + doe - 719468;
}

static void CivilFromDays(int z, int& y, int& m, int& d) {
    z += 719468;
    const int era = (z >= 0 ? z : z - 146096) / 146097;
    const int doe = z - era * 146097;
    const int yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    y = yoe + era * 400;
    const int doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const int mp = (5 * doy + 2) / 153;
    d = doy - (153 * mp + 2) / 5 + 1;
    m = mp < 10 ? mp + 3 : mp - 9;
    y += (m <= 2);
}

static int DayOfWeek(int z) { return Mod(z + 4, 7); } // 0 = Sunday
static bool IsLeap(int y) { return (y % 4 == 0 && y % 100 != 0) || y % 400 == 0; }
static int DaysInMonth(int y, int m) {
    static const int dim[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    return m == 2 && IsLeap(y) ? 29 : dim[m - 1];
}
static int TodaySerial() {
    SYSTEMTIME st; GetLocalTime(&st);
    return DaysFromCivil(st.wYear, st.wMonth, st.wDay);
}

// ---------------------------------------------------------------------------
// Retail 52/53-week fiscal calendar
//   Fiscal year Y is a 52/53-week year ending on the Sunday nearest January 31 of Y+1.
//   It therefore starts on the Monday after the Sunday nearest Jan 31 of Y.
//   Fiscal weeks run Monday–Sunday. Periods follow 4-4-5; a 53rd week joins P12.
// ---------------------------------------------------------------------------
static int SundayNearest(int z) {
    int dow = DayOfWeek(z);
    return dow <= 3 ? z - dow : z + (7 - dow);
}
static int FiscalYearStart(int fy) { return SundayNearest(DaysFromCivil(fy, 1, 31)) + 1; }

struct Fiscal { int year, week, period, quarter; };

static Fiscal FiscalOf(int z) {
    int y, m, d; CivilFromDays(z, y, m, d);
    int fy = z >= FiscalYearStart(y) ? y : y - 1;
    if (z >= FiscalYearStart(fy + 1)) fy++;
    int week = (z - FiscalYearStart(fy)) / 7 + 1;
    static const int periodEnd[12] = {4, 8, 13, 17, 21, 26, 30, 34, 39, 43, 47, 52};
    int period = 12;
    for (int i = 0; i < 12; i++) if (week <= periodEnd[i]) { period = i + 1; break; }
    return {fy, week, period, (period - 1) / 3 + 1};
}

// ---------------------------------------------------------------------------
// Layout (DIPs). Side columns are optional, so the width is computed at runtime.
// ---------------------------------------------------------------------------
namespace L {
    constexpr float H = 420;
    constexpr float Left = 17;
    constexpr float ColW = 34, CellW = 48, CellH = 44;
    constexpr float HeaderY = 14, HeaderH = 52;
    constexpr float DowY = 70, DowH = 28;
    constexpr float GridY = 100;
    constexpr float GridBottom = GridY + 6 * CellH;    // 364
    constexpr float FooterY = 374, FooterH = 34;
    constexpr float Btn = 36;
    constexpr float BtnY = HeaderY + (HeaderH - Btn) / 2;
    constexpr float SetY = 76, SetRowH = 54;

    inline float WeekW = ColW, FiscalW = ColW;
    inline float GridX, GridR, Right, W, NextX, PrevX;
    inline void Update(bool week, bool fiscal) {
        WeekW = week ? ColW : 0;
        FiscalW = fiscal ? ColW : 0;
        GridX = Left + WeekW;
        GridR = GridX + 7 * CellW;
        Right = GridR + FiscalW;
        W = Right + Left;
        NextX = Right - Btn;
        PrevX = Right - 2 * Btn - 2;
    }
}

enum Hit { H_NONE, H_PREV, H_NEXT, H_DAY, H_TODAY, H_TODAYKEY, H_GOTO, H_SETTINGS, H_INPUT, H_SETROW, H_SETSEG };

struct Theme {
    D2D1_COLOR_F tint, text, text2, text3, hover, press, divider;
    D2D1_COLOR_F accentFill, onAccent, accentFg, inputBg, inputBorder, error, keycap, toggleOff;
};

static D2D1_COLOR_F Rgb(UINT rgb, float a = 1.f) {
    return D2D1::ColorF(((rgb >> 16) & 0xFF) / 255.f, ((rgb >> 8) & 0xFF) / 255.f, (rgb & 0xFF) / 255.f, a);
}
static D2D1_COLOR_F WithAlpha(D2D1_COLOR_F c, float a) { c.a *= a; return c; }

// ---------------------------------------------------------------------------
// Settings (persisted to %APPDATA%\CalendarWin\settings.ini)
// ---------------------------------------------------------------------------
struct Settings {
    int weekStart = 2;        // 0 = follow Windows region, 1 = Sunday, 2 = Monday
    bool showWeek = true;     // calendar week numbers (left)
    bool showFiscal = true;   // fiscal weeks (right)
    bool fiscalHeader = true; // "FY26 · Q4 · P10" next to the month title
};

static const int kSettingRows = 4;

// ---------------------------------------------------------------------------
// App state
// ---------------------------------------------------------------------------
static const wchar_t* kClass = L"CalendarWin.Widget";

struct App {
    HWND hwnd = nullptr;
    UINT dpi = 96;

    ID2D1Factory* d2d = nullptr;
    IDWriteFactory* dw = nullptr;
    ID2D1HwndRenderTarget* rt = nullptr;
    ID2D1SolidColorBrush* brush = nullptr;
    ID2D1Layer* layer = nullptr;
    ID2D1StrokeStyle* roundStroke = nullptr;
    IDWriteTextFormat *fTitle = nullptr, *fYear = nullptr, *fDow = nullptr, *fDay = nullptr, *fDayBold = nullptr,
                      *fWeek = nullptr, *fFooter = nullptr, *fKey = nullptr, *fInput = nullptr, *fPill = nullptr,
                      *fSetLabel = nullptr, *fSetSub = nullptr, *fSeg = nullptr;

    Settings set;

    // view
    int viewY = 2026, viewM = 1;
    int prevY = 2026, prevM = 1;
    int animDir = 0;
    double animStart = 0;
    bool animating = false;
    int todayZ = 0;
    int selZ = INT_MIN;

    // locale
    int localeFirstDow = 0;
    int firstDow = 0;      // effective, 0 = Sunday
    int dateOrder = 0;     // 0 MDY, 1 DMY, 2 YMD
    std::wstring monthNames[12];
    std::wstring dowNames[7]; // index 0 = Sunday

    // theme
    bool dark = true;
    Theme th{};

    // interaction
    Hit hover = H_NONE; int hoverCell = -1;
    Hit press = H_NONE; int pressCell = -1;
    bool tracking = false;
    int wheelAccum = 0;
    D2D1_RECT_F todayRect{}, gotoRect{}, todayKeyRect{}, settingsRect{};

    // go-to input
    bool inputOpen = false;
    std::wstring input;
    bool inputError = false;
    bool caretOn = true;

    // settings panel
    bool settingsOpen = false;
    int setFocus = -1; // keyboard focus row, -1 = none shown

    std::wstring toast; // brief footer confirmation, e.g. "Copied 11/17"
};
static App g;

static double Now() {
    static LARGE_INTEGER f = [] { LARGE_INTEGER x; QueryPerformanceFrequency(&x); return x; }();
    LARGE_INTEGER c; QueryPerformanceCounter(&c);
    return double(c.QuadPart) / double(f.QuadPart);
}

static void Invalidate() { InvalidateRect(g.hwnd, nullptr, FALSE); }

// ---------------------------------------------------------------------------
// Settings persistence
// ---------------------------------------------------------------------------
static std::wstring SettingsPath() {
    wchar_t buf[MAX_PATH] = {};
    DWORD n = GetEnvironmentVariableW(L"APPDATA", buf, MAX_PATH);
    std::wstring dir = (n && n < MAX_PATH) ? std::wstring(buf) + L"\\CalendarWin" : L".";
    CreateDirectoryW(dir.c_str(), nullptr);
    return dir + L"\\settings.ini";
}

static std::wstring IniGet(const std::wstring& path, const wchar_t* key, const wchar_t* def) {
    wchar_t buf[64] = {};
    GetPrivateProfileStringW(L"Calendar", key, def, buf, 64, path.c_str());
    std::wstring s = buf;
    for (auto& c : s) c = wchar_t(towlower(c));
    return s;
}

static bool IniBool(const std::wstring& path, const wchar_t* key, bool def) {
    std::wstring v = IniGet(path, key, def ? L"true" : L"false");
    return v == L"true" || v == L"1" || v == L"yes" || v == L"on";
}

static void LoadSettings() {
    std::wstring p = SettingsPath();
    std::wstring ws = IniGet(p, L"WeekStart", L"monday");
    g.set.weekStart = ws == L"auto" ? 0 : ws == L"sunday" ? 1 : 2;
    g.set.showWeek = IniBool(p, L"ShowWeekNumbers", true);
    g.set.showFiscal = IniBool(p, L"ShowFiscalWeeks", true);
    g.set.fiscalHeader = IniBool(p, L"ShowFiscalPeriod", true);
}

static void SaveSettings() {
    std::wstring p = SettingsPath();
    static const wchar_t* ws[] = {L"auto", L"sunday", L"monday"};
    WritePrivateProfileStringW(L"Calendar", L"WeekStart", ws[g.set.weekStart], p.c_str());
    WritePrivateProfileStringW(L"Calendar", L"ShowWeekNumbers", g.set.showWeek ? L"true" : L"false", p.c_str());
    WritePrivateProfileStringW(L"Calendar", L"ShowFiscalWeeks", g.set.showFiscal ? L"true" : L"false", p.c_str());
    WritePrivateProfileStringW(L"Calendar", L"ShowFiscalPeriod", g.set.fiscalHeader ? L"true" : L"false", p.c_str());
}

// ---------------------------------------------------------------------------
// Locale + theme
// ---------------------------------------------------------------------------
static std::wstring LocaleStr(LCTYPE t) {
    wchar_t buf[128] = {};
    GetLocaleInfoEx(LOCALE_NAME_USER_DEFAULT, t, buf, 128);
    return buf;
}

static void LoadLocale() {
    for (int i = 0; i < 12; i++) g.monthNames[i] = LocaleStr(LOCALE_SMONTHNAME1 + i);
    for (int i = 0; i < 7; i++) g.dowNames[(i + 1) % 7] = LocaleStr(LOCALE_SSHORTESTDAYNAME1 + i); // Mon..Sun
    g.localeFirstDow = (_wtoi(LocaleStr(LOCALE_IFIRSTDAYOFWEEK).c_str()) + 1) % 7;
    g.dateOrder = _wtoi(LocaleStr(LOCALE_IDATE).c_str());
}

static bool ReadAccentPalette(BYTE (&pal)[32]) {
    DWORD size = sizeof(pal);
    return RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\Accent",
                        L"AccentPalette", RRF_RT_REG_BINARY, nullptr, pal, &size) == ERROR_SUCCESS && size >= 32;
}

static void LoadTheme() {
    DWORD light = 0, size = sizeof(light);
    if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
                     L"AppsUseLightTheme", RRF_RT_REG_DWORD, nullptr, &light, &size) != ERROR_SUCCESS)
        light = 1;
    g.dark = light == 0;

    // AccentPalette: 8 RGBA entries — Light3, Light2, Light1, Accent, Dark1, Dark2, Dark3, (unused)
    UINT accent = g.dark ? 0x60CDFF : 0x005FB8;
    BYTE pal[32];
    if (ReadAccentPalette(pal)) {
        int i = g.dark ? 1 : 4;
        accent = (pal[i * 4] << 16) | (pal[i * 4 + 1] << 8) | pal[i * 4 + 2];
    }

    Theme& t = g.th;
    if (g.dark) {
        t.tint = Rgb(0x202020, 0.35f);
        t.text = Rgb(0xFFFFFF, 0.96f);
        t.text2 = Rgb(0xFFFFFF, 0.68f);
        t.text3 = Rgb(0xFFFFFF, 0.34f);
        t.hover = Rgb(0xFFFFFF, 0.07f);
        t.press = Rgb(0xFFFFFF, 0.04f);
        t.divider = Rgb(0xFFFFFF, 0.08f);
        t.onAccent = Rgb(0x000000, 0.92f);
        t.inputBg = Rgb(0xFFFFFF, 0.06f);
        t.inputBorder = Rgb(0xFFFFFF, 0.10f);
        t.error = Rgb(0xFF99A4);
        t.keycap = Rgb(0xFFFFFF, 0.12f);
        t.toggleOff = Rgb(0xFFFFFF, 0.60f);
    } else {
        t.tint = Rgb(0xF9F9F9, 0.40f);
        t.text = Rgb(0x000000, 0.90f);
        t.text2 = Rgb(0x000000, 0.62f);
        t.text3 = Rgb(0x000000, 0.32f);
        t.hover = Rgb(0x000000, 0.05f);
        t.press = Rgb(0x000000, 0.03f);
        t.divider = Rgb(0x000000, 0.07f);
        t.onAccent = Rgb(0xFFFFFF);
        t.inputBg = Rgb(0xFFFFFF, 0.70f);
        t.inputBorder = Rgb(0x000000, 0.08f);
        t.error = Rgb(0xC42B1C);
        t.keycap = Rgb(0x000000, 0.08f);
        t.toggleOff = Rgb(0x000000, 0.55f);
    }
    t.accentFill = Rgb(accent);
    t.accentFg = Rgb(accent);
}

static void ApplyWindowTheme() {
    BOOL dark = g.dark;
    DwmSetWindowAttribute(g.hwnd, 20 /*DWMWA_USE_IMMERSIVE_DARK_MODE*/, &dark, sizeof(dark));
    int corner = 2; // DWMWCP_ROUND
    DwmSetWindowAttribute(g.hwnd, 33 /*DWMWA_WINDOW_CORNER_PREFERENCE*/, &corner, sizeof(corner));
    int backdrop = 3; // DWMSBT_TRANSIENTWINDOW (acrylic)
    DwmSetWindowAttribute(g.hwnd, 38 /*DWMWA_SYSTEMBACKDROP_TYPE*/, &backdrop, sizeof(backdrop));
    MARGINS m = {-1, -1, -1, -1};
    DwmExtendFrameIntoClientArea(g.hwnd, &m);
}

// Recompute layout + effective week start; resize the window around its current center.
static void ApplySettings() {
    g.firstDow = g.set.weekStart == 0 ? g.localeFirstDow : g.set.weekStart == 1 ? 0 : 1;
    float oldW = L::W;
    L::Update(g.set.showWeek, g.set.showFiscal);
    if (g.hwnd && oldW != L::W) {
        RECT r; GetWindowRect(g.hwnd, &r);
        int w = int(std::lround(L::W * g.dpi / 96.0));
        int cx = (r.left + r.right) / 2;
        SetWindowPos(g.hwnd, nullptr, cx - w / 2, r.top, w, r.bottom - r.top, SWP_NOZORDER | SWP_NOACTIVATE);
    }
}

// ---------------------------------------------------------------------------
// Direct2D / DirectWrite resources
// ---------------------------------------------------------------------------
static IDWriteTextFormat* MakeFormat(const wchar_t* family, DWRITE_FONT_WEIGHT w, float size,
                                     DWRITE_TEXT_ALIGNMENT ta, DWRITE_PARAGRAPH_ALIGNMENT pa) {
    IDWriteTextFormat* f = nullptr;
    g.dw->CreateTextFormat(family, nullptr, w, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, size, L"", &f);
    if (f) {
        f->SetTextAlignment(ta);
        f->SetParagraphAlignment(pa);
        f->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
    }
    return f;
}

static void CreateDeviceIndependent() {
    D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, &g.d2d);
    DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory), reinterpret_cast<IUnknown**>(&g.dw));
    g.d2d->CreateStrokeStyle(D2D1::StrokeStyleProperties(D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND,
                                                         D2D1_LINE_JOIN_ROUND), nullptr, 0, &g.roundStroke);
    const auto L_ = DWRITE_TEXT_ALIGNMENT_LEADING, C_ = DWRITE_TEXT_ALIGNMENT_CENTER;
    const auto PC = DWRITE_PARAGRAPH_ALIGNMENT_CENTER;
    g.fTitle    = MakeFormat(L"Segoe UI Variable Display", DWRITE_FONT_WEIGHT_SEMI_BOLD, 24, L_, PC);
    g.fYear     = MakeFormat(L"Segoe UI Variable Display", DWRITE_FONT_WEIGHT_NORMAL, 24, L_, PC);
    g.fDow      = MakeFormat(L"Segoe UI Variable Text", DWRITE_FONT_WEIGHT_SEMI_BOLD, 12, C_, PC);
    g.fDay      = MakeFormat(L"Segoe UI Variable Text", DWRITE_FONT_WEIGHT_NORMAL, 14, C_, PC);
    g.fDayBold  = MakeFormat(L"Segoe UI Variable Text", DWRITE_FONT_WEIGHT_SEMI_BOLD, 14, C_, PC);
    g.fWeek     = MakeFormat(L"Segoe UI Variable Small", DWRITE_FONT_WEIGHT_NORMAL, 11, C_, PC);
    g.fFooter   = MakeFormat(L"Segoe UI Variable Text", DWRITE_FONT_WEIGHT_NORMAL, 12.5f, L_, PC);
    g.fKey      = MakeFormat(L"Segoe UI Variable Small", DWRITE_FONT_WEIGHT_SEMI_BOLD, 10.5f, C_, PC);
    g.fInput    = MakeFormat(L"Segoe UI Variable Text", DWRITE_FONT_WEIGHT_NORMAL, 15, L_, PC);
    g.fPill     = MakeFormat(L"Segoe UI Variable Small", DWRITE_FONT_WEIGHT_SEMI_BOLD, 11, C_, PC);
    g.fSetLabel = MakeFormat(L"Segoe UI Variable Text", DWRITE_FONT_WEIGHT_NORMAL, 14, L_, PC);
    g.fSetSub   = MakeFormat(L"Segoe UI Variable Small", DWRITE_FONT_WEIGHT_NORMAL, 11.5f, L_, PC);
    g.fSeg      = MakeFormat(L"Segoe UI Variable Text", DWRITE_FONT_WEIGHT_NORMAL, 12.5f, C_, PC);
}

static void DiscardDevice() {
    SafeRelease(g.layer);
    SafeRelease(g.brush);
    SafeRelease(g.rt);
}

static bool EnsureDevice() {
    if (g.rt) return true;
    RECT rc; GetClientRect(g.hwnd, &rc);
    auto props = D2D1::RenderTargetProperties(
        D2D1_RENDER_TARGET_TYPE_DEFAULT,
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED),
        float(g.dpi), float(g.dpi));
    auto hprops = D2D1::HwndRenderTargetProperties(g.hwnd, D2D1::SizeU(rc.right - rc.left, rc.bottom - rc.top));
    if (FAILED(g.d2d->CreateHwndRenderTarget(props, hprops, &g.rt))) return false;
    g.rt->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
    g.rt->CreateSolidColorBrush(D2D1::ColorF(0, 0, 0), &g.brush);
    g.rt->CreateLayer(&g.layer);
    return true;
}

// ---------------------------------------------------------------------------
// Drawing helpers
// ---------------------------------------------------------------------------
static void DrawStr(const std::wstring& s, IDWriteTextFormat* f, D2D1_RECT_F r, D2D1_COLOR_F c) {
    g.brush->SetColor(c);
    g.rt->DrawText(s.c_str(), UINT32(s.size()), f, r, g.brush, D2D1_DRAW_TEXT_OPTIONS_NONE);
}

static float TextWidth(const std::wstring& s, IDWriteTextFormat* f) {
    IDWriteTextLayout* tl = nullptr;
    float w = 0;
    if (SUCCEEDED(g.dw->CreateTextLayout(s.c_str(), UINT32(s.size()), f, 10000.f, 100.f, &tl))) {
        DWRITE_TEXT_METRICS m; tl->GetMetrics(&m);
        w = m.widthIncludingTrailingWhitespace;
        tl->Release();
    }
    return w;
}

static void FillRound(D2D1_RECT_F r, float rad, D2D1_COLOR_F c) {
    g.brush->SetColor(c);
    g.rt->FillRoundedRectangle(D2D1::RoundedRect(r, rad, rad), g.brush);
}

static void StrokeRound(D2D1_RECT_F r, float rad, D2D1_COLOR_F c, float w) {
    g.brush->SetColor(c);
    float h = w / 2;
    g.rt->DrawRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(r.left + h, r.top + h, r.right - h, r.bottom - h), rad, rad), g.brush, w);
}

static void PushFade(D2D1_RECT_F clip, float opacity) {
    g.rt->PushLayer(D2D1::LayerParameters(clip, nullptr, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE,
                                          D2D1::IdentityMatrix(), opacity), g.layer);
}

static int WeekNumber(int rowStart) {
    int y, m, d;
    if (g.firstDow == 1) { // ISO 8601
        int thu = rowStart + 3;
        CivilFromDays(thu, y, m, d);
        return (thu - DaysFromCivil(y, 1, 1)) / 7 + 1;
    }
    int rowEnd = rowStart + 6; // week containing Jan 1 is week 1
    CivilFromDays(rowEnd, y, m, d);
    int jan1 = DaysFromCivil(y, 1, 1);
    int off = Mod(DayOfWeek(jan1) - g.firstDow, 7);
    return (rowEnd - jan1 + off) / 7 + 1;
}

// Fiscal week for a grid row: the fiscal week of the row's Monday (exact when rows start on Monday).
static int RowFiscalWeek(int rowStart) { return FiscalOf(rowStart + Mod(1 - g.firstDow, 7)).week; }

static int GridStart(int y, int m) {
    int first = DaysFromCivil(y, m, 1);
    return first - Mod(DayOfWeek(first) - g.firstDow, 7);
}

static D2D1_POINT_2F CellCenter(int idx) {
    return D2D1::Point2F(L::GridX + (idx % 7) * L::CellW + L::CellW / 2,
                         L::GridY + (idx / 7) * L::CellH + L::CellH / 2);
}

static std::wstring FiscalLabel(int y, int m) {
    Fiscal f = FiscalOf(DaysFromCivil(y, m, 15)); // mid-month represents the fiscal period
    wchar_t buf[48];
    swprintf_s(buf, L"FY%02d · Q%d · P%d", f.year % 100, f.quarter, f.period);
    return buf;
}

static void DrawTitle(int y, int m, float dy, float alpha) {
    D2D1_RECT_F clip = D2D1::RectF(L::Left, L::HeaderY, L::PrevX - 4, L::HeaderY + L::HeaderH);
    PushFade(clip, alpha);
    const std::wstring& month = g.monthNames[m - 1];
    float x = L::Left + 6;
    D2D1_RECT_F r = D2D1::RectF(x, L::HeaderY + dy, 1000, L::HeaderY + L::HeaderH + dy);
    DrawStr(month, g.fTitle, r, g.th.text);
    std::wstring year = std::to_wstring(y);
    r.left = x + TextWidth(month, g.fTitle) + 8;
    DrawStr(year, g.fYear, r, g.th.text2);
    if (g.set.fiscalHeader) {
        std::wstring lbl = FiscalLabel(y, m);
        float px = r.left + TextWidth(year, g.fYear) + 12;
        float pw = TextWidth(lbl, g.fPill) + 14;
        if (px + pw <= L::PrevX - 6) {
            float cy = L::HeaderY + L::HeaderH / 2 + 2 + dy;
            D2D1_RECT_F pr = D2D1::RectF(px, cy - 10, px + pw, cy + 10);
            FillRound(pr, 10, WithAlpha(g.th.accentFill, 0.16f));
            DrawStr(lbl, g.fPill, pr, g.th.accentFg);
        }
    }
    g.rt->PopLayer();
}

static void DrawGrid(int y, int m, float dy, float alpha, bool interactive) {
    D2D1_RECT_F clip = D2D1::RectF(L::Left, L::GridY, L::Right, L::GridBottom);
    PushFade(clip, alpha);
    const Theme& t = g.th;
    int start = GridStart(y, m);
    for (int r = 0; r < 6; r++) {
        float top = L::GridY + r * L::CellH + dy;
        int rowStart = start + r * 7;
        if (g.set.showWeek)
            DrawStr(std::to_wstring(WeekNumber(rowStart)), g.fWeek,
                    D2D1::RectF(L::Left, top, L::GridX, top + L::CellH), t.text3);
        if (g.set.showFiscal)
            DrawStr(std::to_wstring(RowFiscalWeek(rowStart)), g.fWeek,
                    D2D1::RectF(L::GridR, top, L::Right, top + L::CellH), WithAlpha(t.accentFg, 0.85f));
        for (int c = 0; c < 7; c++) {
            int idx = r * 7 + c, z = rowStart + c;
            int yy, mm, dd; CivilFromDays(z, yy, mm, dd);
            bool inMonth = mm == m, isToday = z == g.todayZ, isSel = z == g.selZ;
            D2D1_POINT_2F p = CellCenter(idx); p.y += dy;
            auto circle = D2D1::Ellipse(p, 17, 17);
            if (interactive && g.hover == H_DAY && g.hoverCell == idx && !isToday) {
                g.brush->SetColor(g.press == H_DAY && g.pressCell == idx ? t.press : t.hover);
                g.rt->FillEllipse(circle, g.brush);
            }
            D2D1_COLOR_F fg = inMonth ? t.text : t.text3;
            if (isToday) {
                g.brush->SetColor(inMonth ? t.accentFill : WithAlpha(t.accentFill, 0.55f));
                g.rt->FillEllipse(circle, g.brush);
                fg = t.onAccent;
                if (isSel) {
                    g.brush->SetColor(t.accentFill);
                    g.rt->DrawEllipse(D2D1::Ellipse(p, 20, 20), g.brush, 1.5f);
                }
            } else if (isSel) {
                g.brush->SetColor(t.accentFg);
                g.rt->DrawEllipse(D2D1::Ellipse(p, 16.25f, 16.25f), g.brush, 1.5f);
            }
            DrawStr(std::to_wstring(dd), isToday || isSel ? g.fDayBold : g.fDay,
                    D2D1::RectF(p.x - L::CellW / 2, p.y - L::CellH / 2, p.x + L::CellW / 2, p.y + L::CellH / 2), fg);
        }
    }
    g.rt->PopLayer();
}

static void DrawChevronButton(Hit which, float x, bool up) {
    D2D1_RECT_F r = D2D1::RectF(x, L::BtnY, x + L::Btn, L::BtnY + L::Btn);
    bool pressed = g.press == which && g.hover == which;
    if (g.hover == which) FillRound(r, 6, pressed ? g.th.press : g.th.hover);
    float cx = x + L::Btn / 2, cy = L::BtnY + L::Btn / 2 + (pressed ? 1.f : 0.f);
    float w = 5.5f, h = 3.f * (up ? -1 : 1);
    g.brush->SetColor(pressed ? g.th.text2 : g.th.text);
    g.rt->DrawLine(D2D1::Point2F(cx - w, cy - h), D2D1::Point2F(cx, cy + h), g.brush, 1.4f, g.roundStroke);
    g.rt->DrawLine(D2D1::Point2F(cx, cy + h), D2D1::Point2F(cx + w, cy - h), g.brush, 1.4f, g.roundStroke);
}

static float DrawKeycap(float x, float cy, const std::wstring& key) {
    float w = std::max(18.f, TextWidth(key, g.fKey) + 10);
    D2D1_RECT_F r = D2D1::RectF(x, cy - 9, x + w, cy + 9);
    FillRound(r, 4, g.th.keycap);
    DrawStr(key, g.fKey, r, g.th.text2);
    return w;
}

static void DrawInput() {
    const Theme& t = g.th;
    D2D1_RECT_F r = D2D1::RectF(L::Left + 2, L::HeaderY + 7, L::Right, L::HeaderY + L::HeaderH - 7);
    FillRound(r, 6, t.inputBg);
    StrokeRound(r, 6, t.inputBorder, 1);
    // Fluent-style focus underline
    g.rt->PushAxisAlignedClip(D2D1::RectF(r.left, r.bottom - 2, r.right, r.bottom), D2D1_ANTIALIAS_MODE_ALIASED);
    FillRound(r, 6, g.inputError ? t.error : t.accentFg);
    g.rt->PopAxisAlignedClip();

    D2D1_RECT_F tr = D2D1::RectF(r.left + 12, r.top, r.right - 12, r.bottom);
    if (g.input.empty()) {
        DrawStr(L"Go to — 5/28, 5/28/2027, May 2027, FW12", g.fInput, tr, t.text3);
    } else {
        DrawStr(g.input, g.fInput, tr, t.text);
    }
    if (g.caretOn) {
        float cx = tr.left + (g.input.empty() ? 0 : TextWidth(g.input, g.fInput)) + 1;
        g.brush->SetColor(t.text);
        g.rt->FillRectangle(D2D1::RectF(cx, r.top + 9, cx + 1.2f, r.bottom - 9), g.brush);
    }
}

// ---------------------------------------------------------------------------
// Settings panel
// ---------------------------------------------------------------------------
static D2D1_RECT_F SetRowRect(int row) {
    float top = L::SetY + row * L::SetRowH;
    return D2D1::RectF(L::Left + 2, top, L::Right - 2, top + L::SetRowH - 6);
}

static const int kSegCount = 3;
static const float kSegW = 56, kSegH = 28;
static D2D1_RECT_F SegRect(int i) {
    D2D1_RECT_F row = SetRowRect(0);
    float right = row.right - 10, cy = (row.top + row.bottom) / 2;
    float left = right - kSegCount * kSegW;
    return D2D1::RectF(left + i * kSegW, cy - kSegH / 2, left + (i + 1) * kSegW, cy + kSegH / 2);
}

static void DrawToggle(D2D1_RECT_F row, bool on) {
    const Theme& t = g.th;
    float cy = (row.top + row.bottom) / 2, right = row.right - 10;
    D2D1_RECT_F r = D2D1::RectF(right - 40, cy - 10, right, cy + 10);
    if (on) {
        FillRound(r, 10, t.accentFill);
        g.brush->SetColor(t.onAccent);
        g.rt->FillEllipse(D2D1::Ellipse(D2D1::Point2F(r.right - 10, cy), 6, 6), g.brush);
    } else {
        StrokeRound(r, 10, t.toggleOff, 1);
        g.brush->SetColor(t.toggleOff);
        g.rt->FillEllipse(D2D1::Ellipse(D2D1::Point2F(r.left + 10, cy), 5, 5), g.brush);
    }
}

static void DrawSettings() {
    const Theme& t = g.th;
    static const wchar_t* labels[kSettingRows] = {L"Week starts on", L"Week numbers", L"Fiscal weeks", L"Fiscal period in header"};
    const wchar_t* subs[kSettingRows] = {
        L"Auto follows your Windows region",
        g.firstDow == 1 ? L"ISO 8601 · left column" : L"Week with Jan 1 is week 1 · left column",
        L"52/53-wk year ending Sun nearest Jan 31",
        L"Shows FY · quarter · period (4-4-5)",
    };
    for (int i = 0; i < kSettingRows; i++) {
        D2D1_RECT_F r = SetRowRect(i);
        bool hot = (g.hover == H_SETROW && g.hoverCell == i) || (g.hover == H_SETSEG && i == 0);
        if (hot) FillRound(r, 6, g.press == H_SETROW && g.pressCell == i ? t.press : t.hover);
        if (g.setFocus == i) StrokeRound(r, 6, t.text2, 1.5f);
        float mid = (r.top + r.bottom) / 2;
        DrawStr(labels[i], g.fSetLabel, D2D1::RectF(r.left + 10, mid - 19, r.right, mid + 1), t.text);
        DrawStr(subs[i], g.fSetSub, D2D1::RectF(r.left + 10, mid + 1, r.right, mid + 17), t.text3);
        if (i == 0) {
            static const wchar_t* segs[kSegCount] = {L"Auto", L"Sun", L"Mon"};
            D2D1_RECT_F all = D2D1::RectF(SegRect(0).left, SegRect(0).top, SegRect(2).right, SegRect(0).bottom);
            FillRound(all, 6, t.inputBg);
            StrokeRound(all, 6, t.inputBorder, 1);
            for (int s = 0; s < kSegCount; s++) {
                D2D1_RECT_F sr = SegRect(s);
                bool selected = g.set.weekStart == s;
                if (selected) {
                    D2D1_RECT_F in = D2D1::RectF(sr.left + 2, sr.top + 2, sr.right - 2, sr.bottom - 2);
                    FillRound(in, 4, t.accentFill);
                } else if (g.hover == H_SETSEG && g.hoverCell == s) {
                    D2D1_RECT_F in = D2D1::RectF(sr.left + 2, sr.top + 2, sr.right - 2, sr.bottom - 2);
                    FillRound(in, 4, t.hover);
                }
                DrawStr(segs[s], g.fSeg, sr, selected ? t.onAccent : t.text);
            }
        } else {
            bool on = i == 1 ? g.set.showWeek : i == 2 ? g.set.showFiscal : g.set.fiscalHeader;
            DrawToggle(r, on);
        }
    }
    float noteY = L::SetY + kSettingRows * L::SetRowH + 6;
    DrawStr(L"Saved to %APPDATA%\\CalendarWin\\settings.ini", g.fSetSub,
            D2D1::RectF(L::Left + 12, noteY, L::Right, noteY + 18), t.text3);
}

static void ChangeSetting(int row, int dir) { // dir: 0 = toggle/cycle, -1/+1 = left/right
    switch (row) {
    case 0:
        if (dir == 0) g.set.weekStart = (g.set.weekStart + 1) % kSegCount;
        else g.set.weekStart = std::clamp(g.set.weekStart + dir, 0, kSegCount - 1);
        break;
    case 1: g.set.showWeek = dir == 0 ? !g.set.showWeek : dir > 0; break;
    case 2: g.set.showFiscal = dir == 0 ? !g.set.showFiscal : dir > 0; break;
    case 3: g.set.fiscalHeader = dir == 0 ? !g.set.fiscalHeader : dir > 0; break;
    }
    SaveSettings();
    ApplySettings();
    Invalidate();
}

// ---------------------------------------------------------------------------
// Footer
// ---------------------------------------------------------------------------
static void DrawFooter() {
    const Theme& t = g.th;
    g.brush->SetColor(t.divider);
    g.rt->FillRectangle(D2D1::RectF(L::Left + 4, L::FooterY - 1, L::Right - 4, L::FooterY), g.brush);
    float cy = L::FooterY + L::FooterH / 2 + 2;
    float right = L::Right - 6;

    // Right side: shortcut hints, laid out right-to-left. Each is clickable when it has a Hit.
    auto hint = [&](Hit h, const wchar_t* key, const wchar_t* label, D2D1_RECT_F* rect, D2D1_COLOR_F labelColor) {
        std::wstring lbl = label;
        float w = TextWidth(lbl, g.fFooter);
        float kw = std::max(18.f, TextWidth(key, g.fKey) + 10);
        float total = w + 6 + kw;
        if (rect) {
            *rect = D2D1::RectF(right - total - 6, cy - 12, right + 6, cy + 12);
            if (g.hover == h) FillRound(*rect, 5, g.press == h ? t.press : t.hover);
        }
        DrawStr(lbl, g.fFooter, D2D1::RectF(right - w, cy - 12, right, cy + 12),
                rect && g.hover == h ? t.text2 : labelColor);
        DrawKeycap(right - total, cy, key);
        right -= total + 16;
    };

    g.gotoRect = g.todayKeyRect = g.settingsRect = D2D1::RectF(0, 0, 0, 0);
    if (g.inputOpen) {
        if (g.inputError) {
            std::wstring msg = L"Couldn’t read that date";
            float w = TextWidth(msg, g.fFooter);
            DrawStr(msg, g.fFooter, D2D1::RectF(right - w, cy - 12, right, cy + 12), t.error);
            right -= w + 16;
        } else {
            hint(H_NONE, L"Esc", L"Cancel", nullptr, t.text3);
            hint(H_NONE, L"Enter", L"Go", nullptr, t.text3);
        }
    } else if (g.settingsOpen) {
        hint(H_SETTINGS, L"Esc", L"Done", &g.settingsRect, t.text3);
    } else {
        hint(H_SETTINGS, L"S", L"Settings", &g.settingsRect, t.text3);
        hint(H_GOTO, L"G", L"Go to date", &g.gotoRect, t.text3);
        hint(H_TODAYKEY, L"T", L"Today", &g.todayKeyRect, t.text3);
    }

    float avail = right - (L::Left + 8) - 4;
    if (!g.toast.empty()) { // confirmation replaces the date briefly
        g.todayRect = D2D1::RectF(0, 0, 0, 0);
        float x = L::Left + 10;
        g.brush->SetColor(t.accentFg);
        g.rt->DrawLine(D2D1::Point2F(x, cy), D2D1::Point2F(x + 3.5f, cy + 3.5f), g.brush, 1.5f, g.roundStroke);
        g.rt->DrawLine(D2D1::Point2F(x + 3.5f, cy + 3.5f), D2D1::Point2F(x + 10, cy - 4), g.brush, 1.5f, g.roundStroke);
        DrawStr(g.toast, g.fFooter, D2D1::RectF(x + 17, cy - 12, right, cy + 12), t.accentFg);
        return;
    }

    // Left side: today's date, in the longest format that fits.
    SYSTEMTIME st; GetLocalTime(&st);
    std::wstring today;
    for (const wchar_t* fmt : {L"dddd, MMMM d, yyyy", L"ddd, MMM d, yyyy", L"ddd, MMM d", L"MMM d"}) {
        wchar_t buf[96] = {};
        GetDateFormatEx(LOCALE_NAME_USER_DEFAULT, 0, &st, fmt, buf, 96, nullptr);
        if (TextWidth(buf, g.fFooter) <= avail) { today = buf; break; }
    }
    if (today.empty()) { g.todayRect = D2D1::RectF(0, 0, 0, 0); return; }
    float tw = TextWidth(today, g.fFooter);
    g.todayRect = D2D1::RectF(L::Left + 2, cy - 12, L::Left + 14 + tw, cy + 12);
    if (g.hover == H_TODAY) FillRound(g.todayRect, 5, g.press == H_TODAY ? t.press : t.hover);
    DrawStr(today, g.fFooter, D2D1::RectF(L::Left + 8, cy - 12, L::Right, cy + 12), g.hover == H_TODAY ? t.accentFg : t.text2);
}

static void Render() {
    if (!EnsureDevice()) return;
    const Theme& t = g.th;
    g.rt->BeginDraw();
    g.rt->Clear(D2D1::ColorF(0, 0, 0, 0));
    g.brush->SetColor(t.tint);
    g.rt->FillRectangle(D2D1::RectF(0, 0, L::W + 50, L::H + 50), g.brush);

    if (g.settingsOpen) {
        DrawStr(L"Settings", g.fTitle, D2D1::RectF(L::Left + 6, L::HeaderY, L::Right, L::HeaderY + L::HeaderH), t.text);
        DrawSettings();
        DrawFooter();
        if (g.rt->EndDraw() == D2DERR_RECREATE_TARGET) DiscardDevice();
        return;
    }

    double e = 1;
    if (g.animating) {
        double p = (Now() - g.animStart) / 0.26;
        if (p >= 1) { g.animating = false; p = 1; }
        e = 1 - std::pow(1 - p, 3);
    }
    float fe = float(e), dir = float(g.animDir);

    // Header
    if (g.inputOpen) {
        DrawInput();
    } else {
        if (g.animating) {
            DrawTitle(g.prevY, g.prevM, -dir * 14 * fe, 1 - fe);
            DrawTitle(g.viewY, g.viewM, dir * 14 * (1 - fe), fe);
        } else {
            DrawTitle(g.viewY, g.viewM, 0, 1);
        }
        DrawChevronButton(H_PREV, L::PrevX, true);
        DrawChevronButton(H_NEXT, L::NextX, false);
    }

    // Column headers
    for (int c = 0; c < 7; c++) {
        float x = L::GridX + c * L::CellW;
        DrawStr(g.dowNames[(g.firstDow + c) % 7], g.fDow, D2D1::RectF(x, L::DowY, x + L::CellW, L::DowY + L::DowH), t.text2);
    }
    g.brush->SetColor(t.divider);
    if (g.set.showWeek) {
        DrawStr(L"Wk", g.fWeek, D2D1::RectF(L::Left, L::DowY, L::GridX, L::DowY + L::DowH), t.text3);
        g.rt->FillRectangle(D2D1::RectF(L::GridX - 1, L::GridY + 8, L::GridX, L::GridBottom - 8), g.brush);
    }
    if (g.set.showFiscal) {
        DrawStr(L"FW", g.fWeek, D2D1::RectF(L::GridR, L::DowY, L::Right, L::DowY + L::DowH), WithAlpha(t.accentFg, 0.85f));
        g.brush->SetColor(t.divider);
        g.rt->FillRectangle(D2D1::RectF(L::GridR, L::GridY + 8, L::GridR + 1, L::GridBottom - 8), g.brush);
    }

    // Grid
    if (g.animating) {
        float off = 3 * L::CellH * 0.35f;
        DrawGrid(g.prevY, g.prevM, -dir * off * fe, 1 - fe, false);
        DrawGrid(g.viewY, g.viewM, dir * off * (1 - fe), fe, false);
    } else {
        DrawGrid(g.viewY, g.viewM, 0, 1, true);
    }

    DrawFooter();

    if (g.rt->EndDraw() == D2DERR_RECREATE_TARGET) DiscardDevice();
}

// ---------------------------------------------------------------------------
// Navigation
// ---------------------------------------------------------------------------
static void NavigateTo(int y, int m) {
    if (y < 1) { y = 1; m = 1; }
    if (y > 9999) { y = 9999; m = 12; }
    if (y == g.viewY && m == g.viewM) { Invalidate(); return; }
    int dir = (y * 12 + m) > (g.viewY * 12 + g.viewM) ? 1 : -1;
    g.prevY = g.viewY; g.prevM = g.viewM;
    g.viewY = y; g.viewM = m;
    g.animDir = dir;
    g.animStart = Now();
    g.animating = true;
    Invalidate();
}

static void Step(int delta) {
    int idx = g.viewY * 12 + (g.viewM - 1) + delta;
    int y = idx / 12, m = idx % 12 + 1;
    if (g.selZ != INT_MIN && y >= 1 && y <= 9999) { // carry the selected day into the new month
        int sy, sm, sd; CivilFromDays(g.selZ, sy, sm, sd);
        g.selZ = DaysFromCivil(y, m, std::min(sd, DaysInMonth(y, m)));
    }
    NavigateTo(y, m);
}

// Arrow-key movement within the grid. The first press only places the cursor.
static void MoveSelection(int deltaDays) {
    int y, m, d;
    if (g.selZ == INT_MIN) {
        CivilFromDays(g.todayZ, y, m, d);
        g.selZ = (y == g.viewY && m == g.viewM) ? g.todayZ : DaysFromCivil(g.viewY, g.viewM, 1);
        Invalidate();
        return;
    }
    CivilFromDays(g.selZ, y, m, d);
    if (y != g.viewY || m != g.viewM) // selection is off-screen: bring it into the shown month first
        g.selZ = DaysFromCivil(g.viewY, g.viewM, std::min(d, DaysInMonth(g.viewY, g.viewM)));
    int z = g.selZ + deltaDays;
    CivilFromDays(z, y, m, d);
    if (y < 1 || y > 9999) return;
    g.selZ = z;
    NavigateTo(y, m);
}

// Short-date pattern from the Windows region with the year (and its separator) removed, e.g. M/d/yyyy -> M/d.
static std::wstring ShortDateNoYear() {
    std::wstring p = LocaleStr(LOCALE_SSHORTDATE), out;
    size_t ys = p.find(L'y');
    if (ys == std::wstring::npos) return p;
    size_t ye = p.find_first_not_of(L'y', ys);
    if (ye == std::wstring::npos) ye = p.size();
    if (ys > 0 && !iswalpha(p[ys - 1])) ys--;                                    // M/d/yyyy  -> drop "/yyyy"
    else if (ye < p.size() && !iswalpha(p[ye]) && p[ye] != L'\'') ye++;          // yyyy-MM-dd -> drop "yyyy-"
    return p.substr(0, ys) + p.substr(ye);
}

static void ShowToast(const std::wstring& text) {
    g.toast = text;
    SetTimer(g.hwnd, 3, 1600, nullptr);
    Invalidate();
}

static void CopySelectedDate() {
    int z = g.selZ != INT_MIN ? g.selZ : g.todayZ;
    int y, m, d; CivilFromDays(z, y, m, d);
    SYSTEMTIME st{}; st.wYear = WORD(y); st.wMonth = WORD(m); st.wDay = WORD(d);
    wchar_t buf[64] = {};
    if (!GetDateFormatEx(LOCALE_NAME_USER_DEFAULT, 0, &st, ShortDateNoYear().c_str(), buf, 64, nullptr)) return;
    std::wstring text = buf;
    if (!OpenClipboard(g.hwnd)) return;
    EmptyClipboard();
    size_t bytes = (text.size() + 1) * sizeof(wchar_t);
    if (HGLOBAL mem = GlobalAlloc(GMEM_MOVEABLE, bytes)) {
        memcpy(GlobalLock(mem), text.c_str(), bytes);
        GlobalUnlock(mem);
        if (!SetClipboardData(CF_UNICODETEXT, mem)) GlobalFree(mem);
    }
    CloseClipboard();
    ShowToast(L"Copied " + text);
}

static void GoToday() {
    g.todayZ = TodaySerial();
    int y, m, d; CivilFromDays(g.todayZ, y, m, d);
    g.selZ = INT_MIN;
    NavigateTo(y, m);
}

// ---------------------------------------------------------------------------
// Date parsing for the go-to box
//   5/28         -> May 28 of the displayed year
//   5/28/2027    -> May 28, 2027 (2-digit years -> 20xx)
//   5/2027       -> May 2027
//   2027         -> displayed month in 2027
//   May, may 28, may 2027, 28 may 2027, today
// Numeric order follows the Windows short-date setting (M/D, D/M or Y/M/D).
// ---------------------------------------------------------------------------
struct Tok { bool num; int val; int digits; std::wstring word; };

static int MatchMonth(const std::wstring& w) {
    static const wchar_t* en[] = {L"january", L"february", L"march", L"april", L"may", L"june", L"july",
                                  L"august", L"september", L"october", L"november", L"december"};
    if (w.size() < 3) return 0;
    for (int i = 0; i < 12; i++) {
        std::wstring loc = g.monthNames[i];
        for (auto& ch : loc) ch = wchar_t(towlower(ch));
        if (loc.compare(0, w.size(), w) == 0 || std::wstring(en[i]).compare(0, w.size(), w) == 0) return i + 1;
    }
    return 0;
}

static int NormYear(const Tok& t) { return t.digits <= 2 ? 2000 + t.val : t.val; }
static bool IsYearTok(const Tok& t) { return t.digits >= 3 || t.val > 31; }

static bool ParseDate(const std::wstring& s, int& y, int& m, int& d) {
    std::vector<Tok> toks;
    for (size_t i = 0; i < s.size();) {
        wchar_t c = s[i];
        if (iswdigit(c)) {
            Tok t{true, 0, 0, L""};
            while (i < s.size() && iswdigit(s[i])) {
                if (t.digits < 6) t.val = t.val * 10 + (s[i] - L'0');
                t.digits++; i++;
            }
            std::wstring suf; // ordinal suffix: 28th
            while (i < s.size() && iswalpha(s[i])) suf += wchar_t(towlower(s[i++]));
            if (!suf.empty() && suf != L"st" && suf != L"nd" && suf != L"rd" && suf != L"th") return false;
            toks.push_back(t);
        } else if (iswalpha(c)) {
            Tok t{false, 0, 0, L""};
            while (i < s.size() && iswalpha(s[i])) t.word += wchar_t(towlower(s[i++]));
            toks.push_back(t);
        } else {
            i++;
        }
    }
    if (toks.empty()) return false;

    y = g.viewY; m = g.viewM; d = 0;
    if (toks.size() == 1 && !toks[0].num && (toks[0].word == L"today" || toks[0].word == L"t" || toks[0].word == L"now")) {
        int z = TodaySerial(); CivilFromDays(z, y, m, d); return true;
    }

    int month = 0;
    std::vector<Tok> nums;
    for (auto& t : toks) {
        if (t.num) { nums.push_back(t); continue; }
        if (month) return false;
        month = MatchMonth(t.word);
        if (!month) return false;
    }

    if (month) {
        m = month;
        if (nums.size() == 1) {
            if (IsYearTok(nums[0])) y = NormYear(nums[0]); else d = nums[0].val;
        } else if (nums.size() == 2) {
            const Tok *dayT = &nums[0], *yearT = &nums[1];
            if (IsYearTok(nums[0]) && !IsYearTok(nums[1])) std::swap(dayT, yearT);
            if (IsYearTok(*dayT)) return false;
            d = dayT->val; y = NormYear(*yearT);
        } else if (nums.size() > 2) {
            return false;
        }
    } else if (nums.size() == 1) {
        const Tok& a = nums[0];
        if (IsYearTok(a)) y = NormYear(a);
        else if (a.val >= 1 && a.val <= 12) m = a.val;
        else d = a.val;
    } else if (nums.size() == 2) {
        const Tok &a = nums[0], &b = nums[1];
        if (a.digits >= 3) { y = a.val; m = b.val; }                 // 2027/5
        else if (IsYearTok(b)) { m = a.val; y = NormYear(b); }       // 5/2027
        else if (g.dateOrder == 1) { d = a.val; m = b.val; }         // D/M
        else { m = a.val; d = b.val; }                               // M/D
    } else if (nums.size() == 3) {
        const Tok &a = nums[0], &b = nums[1], &c = nums[2];
        if (a.digits >= 3 || g.dateOrder == 2) { y = NormYear(a); m = b.val; d = c.val; }
        else if (g.dateOrder == 1) { d = a.val; m = b.val; y = NormYear(c); }
        else { m = a.val; d = b.val; y = NormYear(c); }
    } else {
        return false;
    }

    if (y < 1 || y > 9999 || m < 1 || m > 12) return false;
    if (d != 0 && (d < 1 || d > DaysInMonth(y, m))) return false;
    return true;
}

// "FW12" -> fiscal week 12 of the fiscal year being viewed; "FW12 2027" / "FW12/27" pick the fiscal year.
// Returns the Monday that starts that fiscal week.
static bool ParseFiscalWeek(const std::wstring& s, int& z) {
    std::wstring t;
    for (wchar_t c : s) t += wchar_t(towlower(c));
    size_t i = t.find_first_not_of(L' ');
    if (i == std::wstring::npos || t.compare(i, 2, L"fw") != 0) return false;
    i += 2;
    while (i < t.size() && t[i] == L' ') i++;
    int week = 0, wd = 0;
    while (i < t.size() && iswdigit(t[i]) && wd < 3) { week = week * 10 + (t[i++] - L'0'); wd++; }
    if (wd == 0) return false;
    int fy = FiscalOf(DaysFromCivil(g.viewY, g.viewM, 15)).year;
    while (i < t.size() && (t[i] == L' ' || t[i] == L'/' || t[i] == L'-' || t[i] == L',')) i++;
    if (i < t.size()) {
        int y = 0, yd = 0;
        while (i < t.size() && iswdigit(t[i]) && yd < 5) { y = y * 10 + (t[i++] - L'0'); yd++; }
        if (i != t.size() || (yd != 2 && yd != 4)) return false;
        fy = yd == 2 ? 2000 + y : y;
    }
    if (fy < 2 || fy > 9998 || week < 1) return false;
    int start = FiscalYearStart(fy) + (week - 1) * 7;
    if (start >= FiscalYearStart(fy + 1)) return false; // e.g. FW53 in a 52-week year
    z = start;
    return true;
}

static void OpenInput() {
    g.inputOpen = true;
    g.input.clear();
    g.inputError = false;
    g.caretOn = true;
    SetTimer(g.hwnd, 1, GetCaretBlinkTime(), nullptr);
    Invalidate();
}

static void CloseInput() {
    g.inputOpen = false;
    KillTimer(g.hwnd, 1);
    Invalidate();
}

static void CommitInput() {
    int y, m, d, fz;
    if (ParseFiscalWeek(g.input, fz)) {
        g.selZ = fz;
        CivilFromDays(fz, y, m, d);
        CloseInput();
        NavigateTo(y, m);
        return;
    }
    if (!ParseDate(g.input, y, m, d)) {
        g.inputError = true;
        Invalidate();
        return;
    }
    g.selZ = d ? DaysFromCivil(y, m, d) : INT_MIN;
    CloseInput();
    NavigateTo(y, m);
}

static void OpenSettings() {
    if (g.inputOpen) CloseInput();
    g.settingsOpen = true;
    g.setFocus = -1;
    g.animating = false;
    g.hover = H_NONE;
    Invalidate();
}

static void CloseSettings() {
    g.settingsOpen = false;
    g.hover = H_NONE;
    Invalidate();
}

// ---------------------------------------------------------------------------
// Hit testing
// ---------------------------------------------------------------------------
static bool In(const D2D1_RECT_F& r, float x, float y) { return x >= r.left && x < r.right && y >= r.top && y < r.bottom; }

static Hit HitTest(float x, float y, int& cell) {
    cell = -1;
    if (g.settingsOpen) {
        if (In(g.settingsRect, x, y)) return H_SETTINGS;
        for (int s = 0; s < kSegCount; s++) if (In(SegRect(s), x, y)) { cell = s; return H_SETSEG; }
        for (int r = 1; r < kSettingRows; r++) if (In(SetRowRect(r), x, y)) { cell = r; return H_SETROW; }
        return H_NONE;
    }
    if (g.inputOpen) {
        if (In(D2D1::RectF(L::Left, L::HeaderY, L::Right, L::HeaderY + L::HeaderH), x, y)) return H_INPUT;
    } else {
        if (In(D2D1::RectF(L::PrevX, L::BtnY, L::PrevX + L::Btn, L::BtnY + L::Btn), x, y)) return H_PREV;
        if (In(D2D1::RectF(L::NextX, L::BtnY, L::NextX + L::Btn, L::BtnY + L::Btn), x, y)) return H_NEXT;
        if (In(g.gotoRect, x, y)) return H_GOTO;
        if (In(g.todayKeyRect, x, y)) return H_TODAYKEY;
        if (In(g.settingsRect, x, y)) return H_SETTINGS;
    }
    if (x >= L::GridX && x < L::GridR && y >= L::GridY && y < L::GridBottom) {
        int c = int((x - L::GridX) / L::CellW), r = int((y - L::GridY) / L::CellH);
        cell = r * 7 + c;
        return H_DAY;
    }
    if (In(g.todayRect, x, y)) return H_TODAY;
    return H_NONE;
}

static void ClientDip(LPARAM lp, float& x, float& y) {
    x = GET_X_LPARAM(lp) * 96.f / g.dpi;
    y = GET_Y_LPARAM(lp) * 96.f / g.dpi;
}

static void Activate(Hit h, int cell) {
    switch (h) {
    case H_PREV: Step(-1); break;
    case H_NEXT: Step(+1); break;
    case H_TODAY:
    case H_TODAYKEY: GoToday(); break;
    case H_GOTO: OpenInput(); break;
    case H_SETTINGS: if (g.settingsOpen) CloseSettings(); else OpenSettings(); break;
    case H_SETROW: ChangeSetting(cell, 0); break;
    case H_SETSEG: g.set.weekStart = cell; SaveSettings(); ApplySettings(); Invalidate(); break;
    case H_DAY: {
        int z = GridStart(g.viewY, g.viewM) + cell;
        g.selZ = z;
        int y, m, d; CivilFromDays(z, y, m, d);
        NavigateTo(y, m);
        break;
    }
    default: break;
    }
}

// ---------------------------------------------------------------------------
// Window placement
// ---------------------------------------------------------------------------
static void CenterOnCursorMonitor(HWND hwnd) {
    POINT pt; GetCursorPos(&pt);
    HMONITOR mon = MonitorFromPoint(pt, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi{sizeof(mi)}; GetMonitorInfo(mon, &mi);
    UINT dx = 96, dy = 96; GetDpiForMonitor(mon, MDT_EFFECTIVE_DPI, &dx, &dy);
    int w = int(std::lround(L::W * dx / 96.0)), h = int(std::lround(L::H * dx / 96.0));
    const RECT& wa = mi.rcWork;
    int x = wa.left + (wa.right - wa.left - w) / 2;
    int y = wa.top + (wa.bottom - wa.top - h) / 2;
    SetWindowPos(hwnd, nullptr, x, y, w, h, SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
}

// ---------------------------------------------------------------------------
// Window procedure
// ---------------------------------------------------------------------------
static const UINT WM_APP_SUMMON = WM_APP + 1;

static void OnSettingsKey(WPARAM vk) {
    switch (vk) {
    case VK_UP:    g.setFocus = g.setFocus <= 0 ? 0 : g.setFocus - 1; break;
    case VK_DOWN:  g.setFocus = g.setFocus < 0 ? 0 : std::min(g.setFocus + 1, kSettingRows - 1); break;
    case VK_LEFT:  if (g.setFocus >= 0) { ChangeSetting(g.setFocus, -1); return; } break;
    case VK_RIGHT: if (g.setFocus >= 0) { ChangeSetting(g.setFocus, +1); return; } break;
    default: return;
    }
    Invalidate();
}

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_CREATE:
        g.hwnd = hwnd;
        g.dpi = GetDpiForWindow(hwnd);
        ApplyWindowTheme();
        SetTimer(hwnd, 2, 30000, nullptr); // midnight rollover check
        return 0;

    case WM_ACTIVATE:
        // Flyout behavior: clicking anywhere else closes the widget.
        if (LOWORD(wp) == WA_INACTIVE) DestroyWindow(hwnd);
        return 0;

    case WM_NCCALCSIZE:
        if (wp) return 0; // whole window is client area
        break;

    case WM_NCHITTEST: {
        POINT p{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        ScreenToClient(hwnd, &p);
        int cell;
        Hit h = HitTest(p.x * 96.f / g.dpi, p.y * 96.f / g.dpi, cell);
        return h == H_NONE ? HTCAPTION : HTCLIENT; // empty space drags the widget
    }

    case WM_NCLBUTTONDBLCLK:
        return 0; // never maximize
    case WM_SYSCOMMAND:
        if ((wp & 0xFFF0) == SC_MAXIMIZE || (wp & 0xFFF0) == SC_SIZE) return 0;
        break;

    case WM_ERASEBKGND:
        return 1;

    case WM_PAINT: {
        PAINTSTRUCT ps; BeginPaint(hwnd, &ps);
        Render();
        EndPaint(hwnd, &ps);
        if (g.animating) Invalidate();
        return 0;
    }

    case WM_SIZE:
        if (g.rt) g.rt->Resize(D2D1::SizeU(LOWORD(lp), HIWORD(lp)));
        Invalidate();
        return 0;

    case WM_DPICHANGED: {
        g.dpi = HIWORD(wp);
        if (g.rt) g.rt->SetDpi(float(g.dpi), float(g.dpi));
        const RECT* r = reinterpret_cast<const RECT*>(lp);
        SetWindowPos(hwnd, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top, SWP_NOZORDER | SWP_NOACTIVATE);
        return 0;
    }

    case WM_SETTINGCHANGE:
        if (lp && lstrcmpW(reinterpret_cast<LPCWSTR>(lp), L"ImmersiveColorSet") == 0) {
            LoadTheme(); ApplyWindowTheme(); Invalidate();
        } else if (lp && lstrcmpW(reinterpret_cast<LPCWSTR>(lp), L"intl") == 0) {
            LoadLocale(); ApplySettings(); Invalidate();
        }
        return 0;
    case WM_DWMCOLORIZATIONCOLORCHANGED:
        LoadTheme(); Invalidate();
        return 0;

    case WM_TIMER:
        if (wp == 1) { g.caretOn = !g.caretOn; Invalidate(); }
        else if (wp == 3) { KillTimer(hwnd, 3); g.toast.clear(); Invalidate(); }
        else if (wp == 2) {
            int z = TodaySerial();
            if (z != g.todayZ) { g.todayZ = z; Invalidate(); }
        }
        return 0;

    case WM_MOUSEMOVE: {
        if (!g.tracking) {
            TRACKMOUSEEVENT tme{sizeof(tme), TME_LEAVE, hwnd, 0};
            TrackMouseEvent(&tme);
            g.tracking = true;
        }
        float x, y; ClientDip(lp, x, y);
        int cell; Hit h = HitTest(x, y, cell);
        if (h != g.hover || cell != g.hoverCell) {
            g.hover = h; g.hoverCell = cell;
            Invalidate();
        }
        return 0;
    }
    case WM_MOUSELEAVE:
        g.tracking = false;
        g.hover = H_NONE; g.hoverCell = -1;
        Invalidate();
        return 0;

    case WM_LBUTTONDOWN: {
        float x, y; ClientDip(lp, x, y);
        g.press = HitTest(x, y, g.pressCell);
        if (g.inputOpen && g.press != H_INPUT) { CloseInput(); }
        SetCapture(hwnd);
        Invalidate();
        return 0;
    }
    case WM_LBUTTONUP: {
        ReleaseCapture();
        float x, y; ClientDip(lp, x, y);
        int cell; Hit h = HitTest(x, y, cell);
        Hit pressed = g.press; int pressedCell = g.pressCell;
        g.press = H_NONE; g.pressCell = -1;
        if (h == pressed && cell == pressedCell) Activate(h, cell);
        Invalidate();
        return 0;
    }

    case WM_MOUSEWHEEL:
        if (g.inputOpen || g.settingsOpen) return 0;
        g.wheelAccum += GET_WHEEL_DELTA_WPARAM(wp);
        while (g.wheelAccum >= WHEEL_DELTA) { g.wheelAccum -= WHEEL_DELTA; Step(-1); }
        while (g.wheelAccum <= -WHEEL_DELTA) { g.wheelAccum += WHEEL_DELTA; Step(+1); }
        return 0;

    case WM_KEYDOWN:
        if (wp == VK_ESCAPE) {
            if (g.inputOpen) CloseInput();
            else if (g.settingsOpen) CloseSettings();
            else DestroyWindow(hwnd);
            return 0;
        }
        if (g.inputOpen) return 0;
        if (g.settingsOpen) { OnSettingsKey(wp); return 0; }
        switch (wp) {
        case VK_NEXT: Step(+1); break;
        case VK_PRIOR: Step(-1); break;
        case VK_HOME: GoToday(); break;
        case VK_LEFT: MoveSelection(-1); break;
        case VK_RIGHT: MoveSelection(+1); break;
        case VK_UP: MoveSelection(-7); break;
        case VK_DOWN: MoveSelection(+7); break;
        }
        return 0;

    case WM_CHAR: {
        wchar_t c = wchar_t(wp);
        if (g.settingsOpen) {
            if (c == L's' || c == L'S') CloseSettings();
            else if ((c == L' ' || c == L'\r') && g.setFocus >= 0) ChangeSetting(g.setFocus, 0);
            return 0;
        }
        if (!g.inputOpen) {
            if (c == L'g' || c == L'G') OpenInput();
            else if (c == L't' || c == L'T') GoToday();
            else if (c == L's' || c == L'S') OpenSettings();
            else if (c == L'\r') CopySelectedDate();
            return 0;
        }
        if (c == L'\r') { CommitInput(); return 0; }
        if (c == 8) { if (!g.input.empty()) g.input.pop_back(); }
        else if (c == 127) { g.input.clear(); } // Ctrl+Backspace
        else if (c == 22) { // Ctrl+V
            if (OpenClipboard(hwnd)) {
                if (HANDLE clip = GetClipboardData(CF_UNICODETEXT)) {
                    if (auto* p = static_cast<const wchar_t*>(GlobalLock(clip))) {
                        for (; *p && g.input.size() < 40; ++p) if (*p >= 32) g.input += *p;
                        GlobalUnlock(clip);
                    }
                }
                CloseClipboard();
            }
        } else if (c >= 32 && g.input.size() < 40) {
            g.input += c;
        } else {
            return 0;
        }
        g.inputError = false;
        g.caretOn = true;
        SetTimer(hwnd, 1, GetCaretBlinkTime(), nullptr);
        Invalidate();
        return 0;
    }

    case WM_APP_SUMMON:
        if (IsIconic(hwnd)) ShowWindow(hwnd, SW_RESTORE);
        CenterOnCursorMonitor(hwnd);
        if (g.inputOpen) CloseInput();
        if (g.settingsOpen) CloseSettings();
        GoToday();
        SetForegroundWindow(hwnd);
        return 0;

    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProc(hwnd, msg, wp, lp);
}

// ---------------------------------------------------------------------------
// Entry point
// ---------------------------------------------------------------------------
int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR, int) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    // Single instance: a second launch re-summons the existing widget.
    HANDLE mutex = CreateMutexW(nullptr, TRUE, L"Local\\CalendarWin.SingleInstance");
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        if (HWND existing = FindWindowW(kClass, nullptr)) {
            DWORD pid = 0; GetWindowThreadProcessId(existing, &pid);
            AllowSetForegroundWindow(pid);
            SetForegroundWindow(existing);
            PostMessageW(existing, WM_APP_SUMMON, 0, 0);
        }
        return 0;
    }

    LoadLocale();
    LoadSettings();
    ApplySettings();
    LoadTheme();
    CreateDeviceIndependent();
    g.todayZ = TodaySerial();
    { int d; CivilFromDays(g.todayZ, g.viewY, g.viewM, d); }

    WNDCLASSEXW wc{sizeof(wc)};
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = WndProc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hIcon = static_cast<HICON>(LoadImageW(inst, MAKEINTRESOURCE(1), IMAGE_ICON,
                                             GetSystemMetrics(SM_CXICON), GetSystemMetrics(SM_CYICON), 0));
    wc.hIconSm = static_cast<HICON>(LoadImageW(inst, MAKEINTRESOURCE(1), IMAGE_ICON,
                                               GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), 0));
    wc.lpszClassName = kClass;
    RegisterClassExW(&wc);

    // WS_CAPTION (without WS_SYSMENU / WS_THICKFRAME / WS_MAXIMIZEBOX) gives us the DWM shadow,
    // rounded corners and backdrop with no visible caption buttons, no resizing, no maximizing.
    // WS_EX_TOOLWINDOW keeps it out of the taskbar and Alt+Tab, like a system flyout.
    HWND hwnd = CreateWindowExW(WS_EX_TOOLWINDOW, kClass, L"Calendar", WS_OVERLAPPED | WS_CAPTION | WS_MINIMIZEBOX,
                                CW_USEDEFAULT, CW_USEDEFAULT, 400, 400, nullptr, nullptr, inst, nullptr);
    if (!hwnd) return 1;
    CenterOnCursorMonitor(hwnd);
    ShowWindow(hwnd, SW_SHOW);
    SetForegroundWindow(hwnd);

    MSG m;
    while (GetMessageW(&m, nullptr, 0, 0)) {
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }

    DiscardDevice();
    for (auto* f : {g.fTitle, g.fYear, g.fDow, g.fDay, g.fDayBold, g.fWeek, g.fFooter, g.fKey, g.fInput, g.fPill,
                    g.fSetLabel, g.fSetSub, g.fSeg})
        if (f) f->Release();
    SafeRelease(g.roundStroke);
    SafeRelease(g.dw);
    SafeRelease(g.d2d);
    if (mutex) CloseHandle(mutex);
    return 0;
}
