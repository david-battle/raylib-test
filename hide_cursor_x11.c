#include <X11/Xlib.h>

// Replace the system cursor with an invisible one at the X11 level.
// Needed on WSLg, where GLFW's cursor hiding is ignored by the compositor.
// Kept separate from main.c because Xlib's Font type clashes with raylib's.
void HideCursorX11(unsigned long window) {
    Display *dpy = XOpenDisplay(NULL);
    if (!dpy) return;
    Pixmap blank = XCreatePixmap(dpy, DefaultRootWindow(dpy), 1, 1, 1);
    GC gc = XCreateGC(dpy, blank, 0, NULL);
    XSetForeground(dpy, gc, 0);
    XFillRectangle(dpy, blank, gc, 0, 0, 1, 1);
    XColor color = { 0 };
    Cursor invisible = XCreatePixmapCursor(dpy, blank, blank, &color, &color, 0, 0);
    XDefineCursor(dpy, (Window)window, invisible);
    XFreeGC(dpy, gc);
    XFreePixmap(dpy, blank);
    XFlush(dpy);
}
