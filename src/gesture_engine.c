/*
 * gesture_engine.c — MX Master-style gesture engine for Razer Basilisk V3 Pro
 *
 * Native Win32 with dedicated hook thread. Zero interpreter overhead.
 * SendInput for atomic keystroke delivery. MOUSEEVENTF_HWHEEL for scroll.
 */

#define WIN32_LEAN_AND_MEAN
#define _CRT_SECURE_NO_WARNINGS
#include <windows.h>
#include <shellapi.h>
#include <stdio.h>
#include <string.h>
#include "config.h"
#include "icon.h"
#include "settings_ui.h"

/* ── Constants ──────────────────────────────────────────────────── */

#define WM_TRAYICON         (WM_USER + 1)
#define WM_F24_DOWN         (WM_USER + 2)
#define WM_F24_UP           (WM_USER + 3)
#define WM_DO_SCROLL        (WM_USER + 4)
#define WM_RELOAD_CONFIG    (WM_USER + 5)

#define TIMER_GESTURE       1
#define TIMER_CTRLCLICK     3

#define IDM_SETTINGS        1001
#define IDM_EXIT            1004

/* ── Shared State (hook thread ↔ main thread) ───────────────────── */

typedef struct {
    volatile BOOL   held;           /* F24 physically down */
    volatile BOOL   gestured;       /* gesture fired this cycle (max 1) */
    volatile BOOL   scrolled;       /* scroll used this cycle */
    volatile LONG   deltaX;         /* accumulated mouse delta */
    volatile LONG   deltaY;
    LONG            anchorX;        /* cursor pos at F24 down */
    LONG            anchorY;
    volatile DWORD  lockUntil;      /* tick deadline for post-release lock */
} GestureState;

static GestureState g_state;
static volatile BOOL g_paused = FALSE;
static GestureConfig g_config;

static HHOOK  g_mouseHook   = NULL;
static HHOOK  g_kbHook      = NULL;
static HANDLE g_hookThread   = NULL;
static DWORD  g_hookThreadId = 0;

static HWND   g_hwnd = NULL;
static NOTIFYICONDATAW g_nid;
static char   g_configPath[MAX_PATH];

/* ── Forward Declarations ───────────────────────────────────────── */

static LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM);
static LRESULT CALLBACK MouseHookProc(int, WPARAM, LPARAM);
static LRESULT CALLBACK KeyboardHookProc(int, WPARAM, LPARAM);
static DWORD WINAPI HookThreadProc(LPVOID);
static void SendKeystroke(const KeyAction *action);
static void SendCombo(const KeyCombo *combo);
static void SendHScroll(int direction);
static void TryFireGesture(void);
static void ReloadConfig(void);

static void CreateTrayIcon(void);
static void RemoveTrayIcon(void);
static const char* GetForegroundProcessName(void);

/* ── Hook Thread ────────────────────────────────────────────────── */
/* Dedicated thread for WH_MOUSE_LL + WH_KEYBOARD_LL hooks.        */
/* Only pumps messages — never calls SendInput, file I/O, etc.     */
/* Communicates with main thread via PostMessage + shared state.    */

static DWORD WINAPI HookThreadProc(LPVOID param) {
    (void)param;

    g_mouseHook = SetWindowsHookExW(WH_MOUSE_LL, MouseHookProc,
                                     GetModuleHandleW(NULL), 0);
    g_kbHook = SetWindowsHookExW(WH_KEYBOARD_LL, KeyboardHookProc,
                                  GetModuleHandleW(NULL), 0);

    if (!g_mouseHook || !g_kbHook) {
        MessageBoxW(NULL, L"Failed to install hooks.", L"Gesture Engine", MB_ICONERROR);
        PostMessageW(g_hwnd, WM_CLOSE, 0, 0);
        return 1;
    }

    MSG msg;
    while (GetMessageW(&msg, NULL, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    if (g_mouseHook) { UnhookWindowsHookEx(g_mouseHook); g_mouseHook = NULL; }
    if (g_kbHook)    { UnhookWindowsHookEx(g_kbHook);    g_kbHook = NULL; }
    return 0;
}

/* ── Mouse Hook (runs on hook thread) ───────────────────────────── */

static LRESULT CALLBACK MouseHookProc(int nCode, WPARAM wParam, LPARAM lParam) {
    if (nCode < 0)
        return CallNextHookEx(g_mouseHook, nCode, wParam, lParam);

    MSLLHOOKSTRUCT *ms = (MSLLHOOKSTRUCT *)lParam;

    /* WM_MOUSEMOVE — block while cursor is locked */
    if (wParam == WM_MOUSEMOVE) {
        BOOL locked = g_state.held ||
            (g_state.lockUntil != 0 && GetTickCount() < g_state.lockUntil);

        if (!locked)
            return CallNextHookEx(g_mouseHook, nCode, wParam, lParam);

        /* Accumulate deltas while actively tracking */
        if (g_state.held && !g_state.gestured && !g_state.scrolled) {
            InterlockedExchangeAdd(&g_state.deltaX,
                                   ms->pt.x - g_state.anchorX);
            InterlockedExchangeAdd(&g_state.deltaY,
                                   ms->pt.y - g_state.anchorY);
        }
        return 1;   /* block — cursor frozen */
    }

    /* WM_MOUSEWHEEL — intercept and convert to horizontal scroll */
    if (wParam == WM_MOUSEWHEEL && g_state.held && !g_state.gestured && g_config.scroll_enabled) {
        SHORT delta = (SHORT)HIWORD(ms->mouseData);
        g_state.scrolled = TRUE;
        /* Post to main thread: WPARAM carries direction */
        PostMessageW(g_hwnd, WM_DO_SCROLL, (delta > 0) ? 1 : -1, 0);
        return 1;   /* block vertical scroll */
    }

    return CallNextHookEx(g_mouseHook, nCode, wParam, lParam);
}

/* ── Keyboard Hook (runs on hook thread) ────────────────────────── */

static LRESULT CALLBACK KeyboardHookProc(int nCode, WPARAM wParam, LPARAM lParam) {
    if (nCode < 0)
        return CallNextHookEx(g_kbHook, nCode, wParam, lParam);

    KBDLLHOOKSTRUCT *kb = (KBDLLHOOKSTRUCT *)lParam;

    /* Only handle physical (non-injected) trigger key while not paused */
    if (kb->vkCode != g_config.trigger_vk || g_paused || (kb->flags & LLKHF_INJECTED))
        return CallNextHookEx(g_kbHook, nCode, wParam, lParam);

    if (wParam == WM_KEYDOWN || wParam == WM_SYSKEYDOWN) {
        if (!g_state.held) {            /* ignore key-repeat */
            POINT pt;
            GetCursorPos(&pt);

            /* Reset state atomically */
            InterlockedExchange(&g_state.deltaX, 0);
            InterlockedExchange(&g_state.deltaY, 0);
            g_state.anchorX   = pt.x;
            g_state.anchorY   = pt.y;
            g_state.gestured  = FALSE;
            g_state.scrolled  = FALSE;
            g_state.lockUntil = 0;

            MemoryBarrier();
            g_state.held = TRUE;        /* set LAST */

            /* Tell main thread to cancel any pending ctrl+click */
            PostMessageW(g_hwnd, WM_F24_DOWN, 0, 0);
        }
        return 1;   /* consume F24 */
    }

    if (wParam == WM_KEYUP || wParam == WM_SYSKEYUP) {
        if (g_state.held) {
            /* Set post-release lock BEFORE clearing held */
            g_state.lockUntil = GetTickCount() + (DWORD)g_config.post_release_lock_ms;
            MemoryBarrier();
            g_state.held = FALSE;

            PostMessageW(g_hwnd, WM_F24_UP, 0, 0);
        }
        return 1;   /* consume F24 */
    }

    return CallNextHookEx(g_kbHook, nCode, wParam, lParam);
}

/* ── Gesture Detection (main thread) ───────────────────────────── */

static void TryFireGesture(void) {
    if (g_state.gestured || g_state.scrolled)
        return;

    LONG dx = g_state.deltaX;
    LONG dy = g_state.deltaY;
    LONG absDx = labs(dx);
    LONG absDy = labs(dy);
    LONG maxD  = max(absDx, absDy);

    if (maxD < g_config.gesture_threshold)
        return;

    /* Determine primary axis direction */
    int direction;
    if (absDx > absDy)
        direction = (dx > 0) ? DIR_RIGHT : DIR_LEFT;
    else
        direction = (dy > 0) ? DIR_DOWN : DIR_UP;

    const char *proc = GetForegroundProcessName();
    const KeyAction *action = config_get_action(&g_config, proc, direction);

    if (action && action->type != ACTION_NONE) {
        g_state.gestured = TRUE;    /* prevent re-entry */
        MemoryBarrier();
        SendKeystroke(action);
    }
}

/* ── Get Foreground Process Name ────────────────────────────────── */

static const char* GetForegroundProcessName(void) {
    static char name[MAX_PATH];
    name[0] = '\0';

    HWND fg = GetForegroundWindow();
    if (!fg) return name;

    DWORD pid = 0;
    GetWindowThreadProcessId(fg, &pid);
    if (!pid) return name;

    HANDLE proc = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!proc) return name;

    DWORD size = MAX_PATH;
    if (QueryFullProcessImageNameA(proc, 0, name, &size)) {
        /* Extract just the filename */
        char *slash = strrchr(name, '\\');
        if (slash) memmove(name, slash + 1, strlen(slash + 1) + 1);
    } else {
        name[0] = '\0';
    }
    CloseHandle(proc);
    return name;
}

/* ── SendInput Helpers ──────────────────────────────────────────── */

/* Get mouse button flags for a VK code */
static DWORD mouse_down_flag(WORD vk) {
    if (vk == VK_LBUTTON) return MOUSEEVENTF_LEFTDOWN;
    if (vk == VK_RBUTTON) return MOUSEEVENTF_RIGHTDOWN;
    if (vk == VK_MBUTTON) return MOUSEEVENTF_MIDDLEDOWN;
    return 0;
}
static DWORD mouse_up_flag(WORD vk) {
    if (vk == VK_LBUTTON) return MOUSEEVENTF_LEFTUP;
    if (vk == VK_RBUTTON) return MOUSEEVENTF_RIGHTUP;
    if (vk == VK_MBUTTON) return MOUSEEVENTF_MIDDLEUP;
    return 0;
}

/* Send a single KeyCombo as an atomic SendInput batch */
static void SendCombo(const KeyCombo *combo) {
    if (combo->key == 0) return;

    int n = (combo->numModifiers + 1) * 2;
    INPUT *inputs = (INPUT *)calloc(n, sizeof(INPUT));
    if (!inputs) return;
    int idx = 0;

    /* Modifiers down */
    for (int i = 0; i < combo->numModifiers; i++) {
        inputs[idx].type       = INPUT_KEYBOARD;
        inputs[idx].ki.wVk     = combo->modifiers[i];
        inputs[idx].ki.dwFlags = 0;
        idx++;
    }

    /* Main key down */
    if (combo->isMouse) {
        inputs[idx].type       = INPUT_MOUSE;
        inputs[idx].mi.dwFlags = mouse_down_flag(combo->key);
    } else {
        inputs[idx].type       = INPUT_KEYBOARD;
        inputs[idx].ki.wVk     = combo->key;
        inputs[idx].ki.dwFlags = 0;
    }
    idx++;

    /* Main key up */
    if (combo->isMouse) {
        inputs[idx].type       = INPUT_MOUSE;
        inputs[idx].mi.dwFlags = mouse_up_flag(combo->key);
    } else {
        inputs[idx].type       = INPUT_KEYBOARD;
        inputs[idx].ki.wVk     = combo->key;
        inputs[idx].ki.dwFlags = KEYEVENTF_KEYUP;
    }
    idx++;

    /* Modifiers up (reverse order) */
    for (int i = combo->numModifiers - 1; i >= 0; i--) {
        inputs[idx].type       = INPUT_KEYBOARD;
        inputs[idx].ki.wVk     = combo->modifiers[i];
        inputs[idx].ki.dwFlags = KEYEVENTF_KEYUP;
        idx++;
    }

    SendInput((UINT)idx, inputs, sizeof(INPUT));
    free(inputs);
}

/* Send a full KeyAction (single combo, multi-step sequence, or run) */
static void SendKeystroke(const KeyAction *action) {
    if (action->type == ACTION_RUN) {
        ShellExecuteA(NULL, "open", action->runTarget, NULL, NULL, SW_SHOWNORMAL);
        return;
    }

    if (action->type != ACTION_KEYS || action->numSteps == 0)
        return;

    /* Send each step; delay between multi-step sequences */
    for (int s = 0; s < action->numSteps; s++) {
        if (s > 0) Sleep(50);   /* 50ms between chord steps (matches VS Code timing) */
        SendCombo(&action->steps[s]);
    }
}

static void SendHScroll(int direction) {
    if (g_config.reverse_scroll)
        direction = -direction;
    INPUT input;
    ZeroMemory(&input, sizeof(input));
    input.type          = INPUT_MOUSE;
    input.mi.dwFlags    = MOUSEEVENTF_HWHEEL;
    input.mi.mouseData  = (DWORD)(direction * 120 * g_config.scroll_multiplier);
    SendInput(1, &input, sizeof(INPUT));
}

/* ── Startup Registration ──────────────────────────────────────── */

#define STARTUP_REG_KEY   "Software\\Microsoft\\Windows\\CurrentVersion\\Run"
#define STARTUP_REG_VALUE "GestureEngine"

static void UpdateStartupRegistration(BOOL enable) {
    HKEY hKey;
    if (RegOpenKeyExA(HKEY_CURRENT_USER, STARTUP_REG_KEY, 0, KEY_SET_VALUE | KEY_QUERY_VALUE, &hKey) != ERROR_SUCCESS)
        return;

    if (enable) {
        /* Set value to the full exe path */
        char exePath[MAX_PATH];
        GetModuleFileNameA(NULL, exePath, MAX_PATH);
        RegSetValueExA(hKey, STARTUP_REG_VALUE, 0, REG_SZ,
                       (const BYTE *)exePath, (DWORD)strlen(exePath) + 1);
    } else {
        RegDeleteValueA(hKey, STARTUP_REG_VALUE);
    }
    RegCloseKey(hKey);
}

/* ── Config Reload ─────────────────────────────────────────────── */

static void ReloadConfig(void) {
    GestureConfig newCfg;
    if (config_load(&newCfg, g_configPath) == 0) {
        g_config = newCfg;
        UpdateStartupRegistration(g_config.launch_on_startup);
    }
}

/* ── Tray Icon ──────────────────────────────────────────────────── */

static HICON g_trayIcon = NULL;

static void CreateTrayIcon(void) {
    g_trayIcon = CreateGestureIcon(16);

    ZeroMemory(&g_nid, sizeof(g_nid));
    g_nid.cbSize           = sizeof(g_nid);
    g_nid.hWnd             = g_hwnd;
    g_nid.uID              = 1;
    g_nid.uFlags           = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    g_nid.uCallbackMessage = WM_TRAYICON;
    g_nid.hIcon            = g_trayIcon ? g_trayIcon
                           : LoadIconW(NULL, MAKEINTRESOURCEW(32512));
    wcscpy(g_nid.szTip, L"Gesture Engine");
    Shell_NotifyIconW(NIM_ADD, &g_nid);
}

static void RemoveTrayIcon(void) {
    Shell_NotifyIconW(NIM_DELETE, &g_nid);
    if (g_trayIcon) { DestroyIcon(g_trayIcon); g_trayIcon = NULL; }
}

static void ShowTrayMenu(void) {
    POINT pt;
    GetCursorPos(&pt);

    HMENU menu = CreatePopupMenu();
    AppendMenuW(menu, MF_STRING, IDM_SETTINGS, L"Settings...");
    AppendMenuW(menu, MF_SEPARATOR, 0, NULL);
    AppendMenuW(menu, MF_STRING, IDM_EXIT, L"Exit");

    SetForegroundWindow(g_hwnd);
    TrackPopupMenu(menu, TPM_BOTTOMALIGN | TPM_LEFTALIGN,
                   pt.x, pt.y, 0, g_hwnd, NULL);
    DestroyMenu(menu);
}

/* ── Window Proc (main thread message handler) ──────────────────── */

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {

    /* ── Hook thread notifications ──────────────────────────────── */

    case WM_F24_DOWN:
        /* Cancel pending ctrl+click timer; start gesture polling */
        KillTimer(hwnd, TIMER_CTRLCLICK);
        SetTimer(hwnd, TIMER_GESTURE, 16, NULL);
        return 0;

    case WM_F24_UP: {
        /* Stop gesture polling */
        KillTimer(hwnd, TIMER_GESTURE);

        /* Final gesture check (catches fast swipes) */
        if (!g_state.gestured && !g_state.scrolled)
            TryFireGesture();

        /* If still no action → schedule deferred ctrl+click */
        if (!g_state.gestured && !g_state.scrolled) {
            LONG total = labs(g_state.deltaX) + labs(g_state.deltaY);
            if (total < g_config.dead_zone)
                SetTimer(hwnd, TIMER_CTRLCLICK, 50, NULL);
        }
        return 0;
    }

    case WM_DO_SCROLL:
        SendHScroll((int)wParam);
        return 0;

    /* ── Timers ─────────────────────────────────────────────────── */

    case WM_TIMER:
        switch (wParam) {
        case TIMER_GESTURE:
            if (g_state.held && !g_state.gestured && !g_state.scrolled)
                TryFireGesture();
            break;
        /* TIMER_CONFIG removed — config reloads via UI Apply/OK */
        case TIMER_CTRLCLICK:
            KillTimer(hwnd, TIMER_CTRLCLICK);
            if (!g_state.gestured && !g_state.scrolled && !g_state.held)
                SendKeystroke(&g_config.tap_action);
            break;
        }
        return 0;

    /* ── Tray icon ──────────────────────────────────────────────── */

    case WM_TRAYICON:
        if (lParam == WM_LBUTTONUP) {
            ShowSettingsDialog(g_hwnd, g_configPath);
        }
        if (lParam == WM_RBUTTONUP || lParam == WM_CONTEXTMENU)
            ShowTrayMenu();
        return 0;

    case WM_RELOAD_CONFIG:
        ReloadConfig();
        return 0;

    case WM_COMMAND:
        switch (LOWORD(wParam)) {
        case IDM_SETTINGS:
            ShowSettingsDialog(g_hwnd, g_configPath);
            break;
        case IDM_EXIT:
            PostMessageW(hwnd, WM_CLOSE, 0, 0);
            break;
        }
        return 0;

    case WM_CLOSE:
        KillTimer(hwnd, TIMER_GESTURE);
        /* TIMER_CONFIG removed */
        KillTimer(hwnd, TIMER_CTRLCLICK);
        RemoveTrayIcon();

        /* Stop hook thread */
        if (g_hookThreadId)
            PostThreadMessageW(g_hookThreadId, WM_QUIT, 0, 0);
        if (g_hookThread) {
            WaitForSingleObject(g_hookThread, 2000);
            CloseHandle(g_hookThread);
        }

        DestroyWindow(hwnd);
        return 0;

    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }

    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

/* ── Entry Point ────────────────────────────────────────────────── */

int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE hPrev,
                    LPWSTR lpCmdLine, int nCmdShow)
{
    (void)hPrev; (void)lpCmdLine; (void)nCmdShow;

    /* Prevent multiple instances */
    HANDLE mutex = CreateMutexW(NULL, TRUE, L"GestureEngine_SingleInstance");
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        if (mutex) CloseHandle(mutex);
        MessageBoxW(NULL, L"Gesture Engine is already running.", L"Gesture Engine", MB_ICONINFORMATION);
        return 0;
    }

    /* Build config path: same directory as exe */
    {
        char exePath[MAX_PATH];
        GetModuleFileNameA(NULL, exePath, MAX_PATH);
        char *slash = strrchr(exePath, '\\');
        if (slash) {
            *(slash + 1) = '\0';
        } else {
            exePath[0] = '\0';
        }
        snprintf(g_configPath, sizeof(g_configPath), "%sconfig.json", exePath);
    }

    /* Load config */
    ZeroMemory(&g_state, sizeof(g_state));
    if (config_load(&g_config, g_configPath) != 0) {
        MessageBoxA(NULL, "Failed to load config.json.\n"
                    "Make sure it exists next to gesture_engine.exe.",
                    "Gesture Engine", MB_ICONWARNING);
        /* Continue with defaults */
    }

    /* Apply startup registration */
    UpdateStartupRegistration(g_config.launch_on_startup);

    /* Register window class */
    WNDCLASSEXW wc;
    ZeroMemory(&wc, sizeof(wc));
    wc.cbSize        = sizeof(wc);
    wc.lpfnWndProc   = WndProc;
    wc.hInstance      = hInstance;
    wc.lpszClassName  = L"GestureEngineClass";
    RegisterClassExW(&wc);

    /* Create hidden message-only window */
    g_hwnd = CreateWindowExW(0, L"GestureEngineClass", L"Gesture Engine",
                              0, 0, 0, 0, 0, HWND_MESSAGE, NULL, hInstance, NULL);

    /* Create tray icon */
    CreateTrayIcon();

    /* Start hook thread */
    g_hookThread = CreateThread(NULL, 0, HookThreadProc, NULL, 0, &g_hookThreadId);
    if (!g_hookThread) {
        MessageBoxW(NULL, L"Failed to start hook thread.", L"Gesture Engine", MB_ICONERROR);
        RemoveTrayIcon();
        return 1;
    }

    /* TIMER_GESTURE starts/stops on F24 down/up — not running 24/7 */

    /* Main message loop */
    MSG msg;
    while (GetMessageW(&msg, NULL, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    if (mutex) {
        ReleaseMutex(mutex);
        CloseHandle(mutex);
    }

    return (int)msg.wParam;
}
