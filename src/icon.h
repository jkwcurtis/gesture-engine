#ifndef ICON_H
#define ICON_H

#include <windows.h>

#define IDI_GESTURE_ICON 100

/*
 * Load the gesture engine icon from embedded resources.
 * size: desired icon size (16 for tray, 32 for larger displays).
 * Returns HICON (caller must DestroyIcon when done).
 */
static HICON CreateGestureIcon(int size) {
    return (HICON)LoadImageW(
        GetModuleHandleW(NULL),
        MAKEINTRESOURCEW(IDI_GESTURE_ICON),
        IMAGE_ICON,
        size, size,
        LR_DEFAULTCOLOR
    );
}

#endif /* ICON_H */
