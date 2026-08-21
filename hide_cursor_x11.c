#include <X11/Xlib.h>
#include <X11/cursorfont.h>

// Replace the system cursor with the "target" glyph from the X cursor
// font (circle-with-dot reticle), which doubles as the game's aim reticle
// (main.c draws nothing at the mouse). Needed on WSLg: the compositor ignores GLFW/raylib cursor hiding, AND silently drops
// custom pixmap cursors built with XCreatePixmapCursor (wslg#376/#1300) --
// they never reach the Windows side. Font cursors via XCreateFontCursor are
// the one thing that does get through, so the best achievable result here is
// swapping the arrow for an unobtrusive dot rather than true invisibility.
//
// `handle` is what raylib's GetWindowHandle() returns on X11: a POINTER to
// the X11 Window id (rcore_desktop_glfw.c stores it in a local), so it must
// be dereferenced. Passing the value itself used to fail with BadWindow.
//
// Kept separate from main.c because Xlib's Font type clashes with raylib's.
// Do NOT call raylib's HideCursor() alongside this: on X11 it installs
// GLFW's own cursor, overriding this one.
void HideCursorX11(void *handle) {
    Display *dpy = XOpenDisplay(NULL);
    if (!dpy) return;
    Cursor reticle = XCreateFontCursor(dpy, XC_target);
    XDefineCursor(dpy, *(Window *)handle, reticle);
    XFlush(dpy); // definition lives in the X server; safe to disconnect
    XCloseDisplay(dpy);
}
