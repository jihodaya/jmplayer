#include "windowplacement.h"

#include <QApplication>
#include <QMetaObject>
#include <QWidget>

namespace WindowPlacement {

bool VisibleFrame(HWND hwnd, RECT* pOut)
{
    using PFN_DwmGetWindowAttribute = HRESULT (WINAPI*)(HWND, DWORD, PVOID, DWORD);
    static PFN_DwmGetWindowAttribute pGet = [] {
        HMODULE h = LoadLibraryW(L"dwmapi.dll");      // loaded lazily: no import needed
        return h ? (PFN_DwmGetWindowAttribute) GetProcAddress(h, "DwmGetWindowAttribute")
                 : nullptr;
    }();

    constexpr DWORD kExtendedFrameBounds = 9;         // DWMWA_EXTENDED_FRAME_BOUNDS
    if (pGet && SUCCEEDED(pGet(hwnd, kExtendedFrameBounds, pOut, sizeof(RECT))))
        return true;
    return GetWindowRect(hwnd, pOut) != FALSE;
}

bool MainWindowFrame(RECT* pOut)
{
    for (QWidget* w : QApplication::topLevelWidgets()) {
        if (!w->isWindow() || !w->isVisible())
            continue;
        if (qstrcmp(w->metaObject()->className(), "MainWindow") != 0)
            continue;

        if (VisibleFrame((HWND) w->winId(), pOut))
            return true;

        const QRect g = w->frameGeometry();
        pOut->left   = g.x();
        pOut->top    = g.y();
        pOut->right  = g.x() + g.width();
        pOut->bottom = g.y() + g.height();
        return true;
    }
    return false;
}

void PlaceBeside(HWND hwnd, const RECT& ref, int nGap)
{
    const int refX = ref.left;
    const int refY = ref.top;
    const int refW = ref.right - ref.left;
    const int refH = ref.bottom - ref.top;
    if (!hwnd || refW <= 0)
        return;

    RECT rWin {}, rVis {};
    if (!GetWindowRect(hwnd, &rWin))
        return;
    if (!VisibleFrame(hwnd, &rVis))
        rVis = rWin;

    // Everything below is worked out in painted coordinates, so what lines up on
    // screen is what the user actually sees.
    const int w = rVis.right - rVis.left;
    const int h = rVis.bottom - rVis.top;

    // SetWindowPos moves the window RECTANGLE, not the painted area, so the
    // difference between the two has to come back off at the end.
    const int dx = rVis.left - rWin.left;
    const int dy = rVis.top - rWin.top;

    // Sit just above jmp, centred on it. If there isn't room above (jmp near the
    // top of the screen), drop it underneath instead of shoving it off-screen.
    int x = refX + (refW - w) / 2;
    int y = refY - h - nGap;
    if (y < 0)
        y = refY + refH + nGap;

    // Keep it on the monitor jmp is on - centring can push it past an edge when
    // jmp sits near one, or when the companion window is wider than jmp.
    HMONITOR hMon = MonitorFromPoint(POINT { refX + refW / 2, refY + refH / 2 },
                                     MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi { sizeof(MONITORINFO) };
    if (GetMonitorInfoW(hMon, &mi)) {
        const RECT& wa = mi.rcWork;
        if (x < wa.left)       x = wa.left;
        if (x + w > wa.right)  x = wa.right - w;
        if (y < wa.top)        y = wa.top;
        if (y + h > wa.bottom) y = wa.bottom - h;
    }

    SetWindowPos(hwnd, nullptr, x - dx, y - dy, 0, 0,
                 SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
}

void ApplyDarkTitleBar(QWidget* pWidget)
{
    if (!pWidget)
        return;

    HWND hwnd = (HWND) pWidget->winId();
    if (!hwnd)
        return;

    // Loaded lazily so nothing has to link dwmapi for this.
    using PFN_DwmSetWindowAttribute = HRESULT (WINAPI*)(HWND, DWORD, LPCVOID, DWORD);
    static PFN_DwmSetWindowAttribute pSet = [] {
        HMODULE h = LoadLibraryW(L"dwmapi.dll");
        return h ? (PFN_DwmSetWindowAttribute) GetProcAddress(h, "DwmSetWindowAttribute")
                 : nullptr;
    }();
    if (!pSet)
        return;

    BOOL dark = TRUE;
    pSet(hwnd, 20, &dark, sizeof(dark));          // DWMWA_USE_IMMERSIVE_DARK_MODE

    COLORREF textColor = RGB(255, 255, 255);
    pSet(hwnd, 36, &textColor, sizeof(textColor)); // DWMWA_TEXT_COLOR
}

void PlaceBesideMainWindow(QWidget* pWidget, int nGap)
{
    if (!pWidget)
        return;

    RECT ref {};
    if (!MainWindowFrame(&ref))
        return;

    PlaceBeside((HWND) pWidget->winId(), ref, nGap);
}

} // namespace WindowPlacement
