#ifndef WINDOWPLACEMENT_H
#define WINDOWPLACEMENT_H

// Parking a companion window above jmp.
//
// Both built-in sound modules put a window on screen - Nuked-SC55's panel and
// the MT-32's display - and both have to land in the same place, or switching
// between them makes the desktop jump about. The rules are fiddlier than they
// look (DWM's invisible borders, per-monitor work areas, physical versus
// logical pixels), so they live here once rather than in each caller.
//
// Windows-only, like everything else that positions native windows here.

#include <windows.h>

class QWidget;

namespace WindowPlacement {

// A window's *painted* bounds, which is not its rectangle.
//
// Since Windows 10 a top-level window's GetWindowRect includes the invisible
// resize border - typically seven or eight pixels on the left, right and
// bottom - so centring on those numbers puts the window visibly off to one
// side. DWM knows the real bounds; ask it, and fall back to the plain
// rectangle if it will not answer.
bool VisibleFrame(HWND hwnd, RECT* pOut);

// jmp's main window, in physical pixels, or false when it is not up yet.
//
// Matched by class name rather than "first visible titled window": the channel
// monitor and the lyrics window are QMainWindows too and are top-level when
// floating, so any looser test can centre a companion on a side panel. The
// frame is read through the native handle because Qt reports logical pixels
// and SetWindowPos wants physical ones, and on a scaled display the two
// disagree.
bool MainWindowFrame(RECT* pOut);

// Parks `hwnd` just above `ref`, centred on it, dropping underneath instead
// when there is no room, and clamped to the work area of whichever monitor
// `ref` is on. Sizes are taken from the window's painted bounds, and the
// difference between those and its rectangle is taken back off at the end -
// SetWindowPos moves the rectangle.
void PlaceBeside(HWND hwnd, const RECT& ref, int nGap = 8);

// Convenience: `PlaceBeside` against jmp's main window. Does nothing when
// either window is unavailable.
void PlaceBesideMainWindow(QWidget* pWidget, int nGap = 8);

// The dark title bar, with white caption text.
//
// A stylesheet stops at the client area, so a window styled to jmp's palette
// still gets whatever caption the system draws - which is white, and looks
// like a different application sitting on top of this one. DWM owns that,
// hence the two attributes.
//
// Attribute 20 is DWMWA_USE_IMMERSIVE_DARK_MODE and 36 is DWMWA_TEXT_COLOR;
// the latter needs Windows 11 22000+, and on older builds the call simply
// fails and the default caption text stays. Forcing white is deliberate: the
// default dark-mode caption reads as grey, especially while inactive.
//
// Must be called after the window has a native handle - showEvent is the
// reliable place, since winId() before that creates one early.
void ApplyDarkTitleBar(QWidget* pWidget);

} // namespace WindowPlacement

#endif // WINDOWPLACEMENT_H
