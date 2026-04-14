/*
 * settings_ui.c — Win32 PropertySheet settings dialog for Gesture Engine
 *
 * Three tabs: General, Default Gestures, App Overrides.
 * Action picker via popup menus with category submenus.
 * Key recording via temporary low-level keyboard hook.
 */

#define WIN32_LEAN_AND_MEAN
#define _CRT_SECURE_NO_WARNINGS
#include <windows.h>
#include <commctrl.h>
#include <commdlg.h>
#include <psapi.h>
#include <shellapi.h>
#include <shlobj.h>
#include <stdio.h>
#include <string.h>
#include <ctype.h>

#include "config.h"
#include "actions.h"
#include "resource.h"
#include "settings_ui.h"

/* Link libraries specified in build.bat: comctl32, comdlg32, psapi, version, etc. */

/* ── State shared across dialog pages ──────────────────────────── */

static GestureConfig s_cfg;         /* working copy of config */
static char s_configPath[MAX_PATH];
static BOOL s_dirty = FALSE;
static BOOL s_generalInitDone = FALSE; /* guard against EN_CHANGE during init */
static BOOL s_dialogOpen = FALSE;   /* re-entrancy guard for settings dialog */
static HINSTANCE s_hInst;
static HFONT s_hFont = NULL;        /* Segoe UI for modern look */
static HWND s_engineHwnd = NULL;    /* engine's hidden window for reload messages */

#define WM_RELOAD_CONFIG (WM_USER + 5)

/* Mark a page dirty and enable the PropertySheet Apply button */
static void MarkDirty(HWND hwndPage) {
    s_dirty = TRUE;
    PropSheet_Changed(GetParent(hwndPage), hwndPage);
}

/* Save config and notify engine to reload */
static void SaveAndReload(void) {
    if (config_save(&s_cfg, s_configPath) == 0) {
        if (s_engineHwnd)
            PostMessageW(s_engineHwnd, WM_RELOAD_CONFIG, 0, 0);
    }
}

/* ── Key Recording State ───────────────────────────────────────── */

static HHOOK s_recHook = NULL;
static BOOL  s_recording = FALSE;
static WORD  s_recMods[MAX_MODIFIERS];
static int   s_recModCount = 0;
static WORD  s_recKey = 0;
static BOOL  s_recDone = FALSE;
static HWND  s_recOwner = NULL;

/* (Sequence recording state is managed locally in SetActionFromMenu) */

/* Which gesture button triggered the action picker */
static int   s_activeDirection = -1;
static BOOL  s_activeIsApp = FALSE;

/* Scroll-section controls (for enable/disable toggling) */
static HWND s_scrollEdit   = NULL;   /* IDC_SCROLL_SLIDER edit box */
static HWND s_scrollUpDown = NULL;   /* IDC_SCROLL_SLIDER + 500 up-down */
static HWND s_scrollLabel  = NULL;   /* "Scroll Multiplier" label */
static HWND s_reverseCheck = NULL;   /* IDC_REVERSE_CHECK */

/* ── Forward Declarations ──────────────────────────────────────── */

static INT_PTR CALLBACK GeneralPageProc(HWND, UINT, WPARAM, LPARAM);
static INT_PTR CALLBACK GesturesPageProc(HWND, UINT, WPARAM, LPARAM);
static INT_PTR CALLBACK AppsPageProc(HWND, UINT, WPARAM, LPARAM);
static INT_PTR CALLBACK AppPickerDlgProc(HWND, UINT, WPARAM, LPARAM);

static void ShowActionMenu(HWND hwnd, int direction, BOOL isApp);
static void FormatComboString(WORD *mods, int nMods, WORD key, char *buf, int bufLen);
static void UpdateScrollControls(BOOL enabled);

/* ── VK name lookup — uses vk_to_name from config.c ────────────── */
/* Handles generic modifier VKs that vk_to_name doesn't map */

static const char* VkDisplayName(WORD vk) {
    if (vk == VK_CONTROL) return "ctrl";
    if (vk == VK_SHIFT)   return "shift";
    if (vk == VK_MENU)    return "alt";
    return vk_to_name(vk);
}

static void FormatComboString(WORD *mods, int nMods, WORD key, char *buf, int bufLen) {
    buf[0] = '\0';
    for (int i = 0; i < nMods; i++) {
        if (buf[0]) strncat(buf, "+", bufLen - strlen(buf) - 1);
        strncat(buf, VkDisplayName(mods[i]), bufLen - strlen(buf) - 1);
    }
    if (key) {
        if (buf[0]) strncat(buf, "+", bufLen - strlen(buf) - 1);
        strncat(buf, VkDisplayName(key), bufLen - strlen(buf) - 1);
    }
}

/* ── Get display name for an action ────────────────────────────── */

static const char* GetActionDisplayName(const KeyAction *action) {
    if (action->type == ACTION_NONE) return "(none)";
    if (action->type == ACTION_RUN) return action->runTarget;

    /* Check if configStr matches a builtin */
    if (action->configStr[0]) {
        int count;
        const BuiltinAction *all = action_get_all(&count);
        for (int i = 0; i < count; i++) {
            if (_stricmp(action->configStr, all[i].id) == 0)
                return all[i].displayName;
        }
        return action->configStr;  /* custom keystroke string */
    }
    return "(none)";
}

/* ── Key Recording Hook ────────────────────────────────────────── */

static BOOL IsModifierVk(WORD vk) {
    return vk == VK_LCONTROL || vk == VK_RCONTROL || vk == VK_CONTROL ||
           vk == VK_LSHIFT   || vk == VK_RSHIFT   || vk == VK_SHIFT   ||
           vk == VK_LMENU    || vk == VK_RMENU     || vk == VK_MENU    ||
           vk == VK_LWIN     || vk == VK_RWIN;
}

/* Normalize modifier VK to left-hand version */
static WORD NormalizeMod(WORD vk) {
    if (vk == VK_RCONTROL || vk == VK_CONTROL) return VK_LCONTROL;
    if (vk == VK_RSHIFT   || vk == VK_SHIFT)   return VK_LSHIFT;
    if (vk == VK_RMENU    || vk == VK_MENU)     return VK_LMENU;
    if (vk == VK_RWIN)                           return VK_LWIN;
    return vk;
}

static LRESULT CALLBACK RecordHookProc(int nCode, WPARAM wParam, LPARAM lParam) {
    if (nCode >= 0 && s_recording) {
        KBDLLHOOKSTRUCT *kb = (KBDLLHOOKSTRUCT *)lParam;

        if (wParam == WM_KEYDOWN || wParam == WM_SYSKEYDOWN) {
            WORD vk = (WORD)kb->vkCode;

            /* Escape cancels */
            if (vk == VK_ESCAPE) {
                s_recDone = TRUE;
                s_recKey = 0;
                return 1;
            }

            if (IsModifierVk(vk)) {
                /* Add modifier if not already tracked */
                WORD norm = NormalizeMod(vk);
                BOOL found = FALSE;
                for (int i = 0; i < s_recModCount; i++) {
                    if (s_recMods[i] == norm) { found = TRUE; break; }
                }
                if (!found && s_recModCount < MAX_MODIFIERS)
                    s_recMods[s_recModCount++] = norm;
            } else {
                /* Main key pressed — recording complete */
                s_recKey = vk;
                s_recDone = TRUE;
            }
            return 1;  /* consume */
        }
        if (wParam == WM_KEYUP || wParam == WM_SYSKEYUP) {
            return 1;  /* consume all keys during recording */
        }
    }
    return CallNextHookEx(s_recHook, nCode, wParam, lParam);
}

/* Start key recording. Returns the formatted string in outBuf, or empty on cancel.
 * If sequence mode, appends to s_seqResult. */
static BOOL RecordKeystroke(HWND parent, char *outBuf, int outLen) {
    s_recMods[0] = s_recMods[1] = s_recMods[2] = s_recMods[3] = 0;
    s_recModCount = 0;
    s_recKey = 0;
    s_recDone = FALSE;
    s_recording = TRUE;
    s_recOwner = parent;

    s_recHook = SetWindowsHookExW(WH_KEYBOARD_LL, RecordHookProc, s_hInst, 0);
    if (!s_recHook) {
        s_recording = FALSE;
        return FALSE;
    }

    /* Modal message pump with 5s timeout */
    DWORD startTick = GetTickCount();
    MSG msg;
    while (!s_recDone && (GetTickCount() - startTick < 5000)) {
        if (PeekMessageW(&msg, NULL, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        } else {
            Sleep(10);
        }
    }

    UnhookWindowsHookEx(s_recHook);
    s_recHook = NULL;
    s_recording = FALSE;

    if (s_recKey == 0) {
        outBuf[0] = '\0';
        return FALSE;
    }

    FormatComboString(s_recMods, s_recModCount, s_recKey, outBuf, outLen);
    return TRUE;
}

/* ── Action Picker Popup Menu ──────────────────────────────────── */

static void BuildActionMenu(HMENU menu) {
    int count;
    const BuiltinAction *all = action_get_all(&count);

    /* Build submenus per category */
    const char **cats = ACTION_CATEGORIES;
    for (int c = 0; cats[c]; c++) {
        HMENU sub = CreatePopupMenu();
        for (int i = 0; i < count; i++) {
            if (_stricmp(all[i].category, cats[c]) == 0) {
                /* Convert display name to wide */
                WCHAR wname[128];
                MultiByteToWideChar(CP_UTF8, 0, all[i].displayName, -1, wname, 128);
                AppendMenuW(sub, MF_STRING, IDM_ACTION_BASE + i, wname);
            }
        }
        WCHAR wcat[64];
        MultiByteToWideChar(CP_UTF8, 0, cats[c], -1, wcat, 64);
        AppendMenuW(menu, MF_POPUP, (UINT_PTR)sub, wcat);
    }

    AppendMenuW(menu, MF_SEPARATOR, 0, NULL);
    AppendMenuW(menu, MF_STRING, IDM_ACTION_CUSTOM,   L"Custom Keystroke...");
    AppendMenuW(menu, MF_STRING, IDM_ACTION_SEQUENCE,  L"Key Sequence...");
    AppendMenuW(menu, MF_STRING, IDM_ACTION_RUN,       L"Run Program...");
    AppendMenuW(menu, MF_SEPARATOR, 0, NULL);
    AppendMenuW(menu, MF_STRING, IDM_ACTION_NONE,      L"None (disable)");
}

static void SetActionFromMenu(HWND hwnd, int menuId, int direction, BOOL isApp) {
    KeyAction *target;
    if (isApp) {
        /* Find selected app in the listbox and get its override */
        HWND appList = GetDlgItem(GetParent(hwnd), IDC_APP_LIST);
        int sel = (int)SendMessage(appList, LB_GETCURSEL, 0, 0);
        if (sel < 0 || sel >= s_cfg.numApps) return;
        target = &s_cfg.apps[sel].actions[direction];
        s_cfg.apps[sel].hasAction[direction] = TRUE;
    } else {
        target = &s_cfg.defaults[direction];
    }

    if (menuId == IDM_ACTION_NONE) {
        memset(target, 0, sizeof(*target));
        if (isApp) {
            HWND appList = GetDlgItem(GetParent(hwnd), IDC_APP_LIST);
            int sel = (int)SendMessage(appList, LB_GETCURSEL, 0, 0);
            if (sel >= 0) s_cfg.apps[sel].hasAction[direction] = FALSE;
        }
    } else if (menuId == IDM_ACTION_CUSTOM) {
        char buf[MAX_ACTION_STR] = {0};
        if (RecordKeystroke(hwnd, buf, sizeof(buf)) && buf[0]) {
            /* Re-parse the action to fill the struct properly */
            memset(target, 0, sizeof(*target));
            strncpy(target->configStr, buf, MAX_ACTION_STR - 1);
            /* Use a minimal inline parse for the UI — full parse happens on save/reload */
            target->type = ACTION_KEYS;
            target->numSteps = 1; /* placeholder until save+reload */
        }
    } else if (menuId == IDM_ACTION_SEQUENCE) {
        /* Record up to 4 combos */
        char result[MAX_ACTION_STR] = {0};
        for (int step = 0; step < MAX_COMBO_STEPS; step++) {
            char buf[64] = {0};
            char title[64];
            snprintf(title, sizeof(title), "Press key combo #%d (Escape to finish)", step + 1);
            /* Simple: just record multiple combos */
            if (!RecordKeystroke(hwnd, buf, sizeof(buf)) || !buf[0])
                break;
            if (result[0]) strncat(result, ",", sizeof(result) - strlen(result) - 1);
            strncat(result, buf, sizeof(result) - strlen(result) - 1);
        }
        if (result[0]) {
            memset(target, 0, sizeof(*target));
            strncpy(target->configStr, result, MAX_ACTION_STR - 1);
            target->type = ACTION_KEYS;
            target->numSteps = 1;
        }
    } else if (menuId == IDM_ACTION_RUN) {
        OPENFILENAMEA ofn;
        char path[MAX_PATH] = {0};
        ZeroMemory(&ofn, sizeof(ofn));
        ofn.lStructSize = sizeof(ofn);
        ofn.hwndOwner   = hwnd;
        ofn.lpstrFilter  = "Executables\0*.exe\0All Files\0*.*\0";
        ofn.lpstrFile    = path;
        ofn.nMaxFile     = MAX_PATH;
        ofn.Flags        = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
        if (GetOpenFileNameA(&ofn)) {
            memset(target, 0, sizeof(*target));
            target->type = ACTION_RUN;
            snprintf(target->configStr, MAX_ACTION_STR, "run:%s", path);
            strncpy(target->runTarget, path, MAX_RUN_TARGET - 1);
        }
    } else if (menuId >= IDM_ACTION_BASE && menuId < IDM_ACTION_BASE + 100) {
        int idx = menuId - IDM_ACTION_BASE;
        int count;
        const BuiltinAction *all = action_get_all(&count);
        if (idx < count) {
            memset(target, 0, sizeof(*target));
            strncpy(target->configStr, all[idx].id, MAX_ACTION_STR - 1);
            target->type = ACTION_KEYS;
            target->numSteps = 1;
        }
    }

    MarkDirty(hwnd);
}

static void ShowActionMenu(HWND hwnd, int direction, BOOL isApp) {
    s_activeDirection = direction;
    s_activeIsApp = isApp;

    HMENU menu = CreatePopupMenu();
    BuildActionMenu(menu);

    /* Position at the button */
    RECT rc;
    int btnId = isApp ? (IDC_APP_BTN_UP + direction) : (IDC_GESTURE_BTN_UP + direction);
    HWND btn = GetDlgItem(hwnd, btnId);
    if (btn) {
        GetWindowRect(btn, &rc);
    } else {
        POINT pt;
        GetCursorPos(&pt);
        rc.left = pt.x; rc.bottom = pt.y;
    }

    int cmd = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_LEFTALIGN | TPM_TOPALIGN,
                             rc.left, rc.bottom, 0, hwnd, NULL);
    DestroyMenu(menu);

    if (cmd > 0)
        SetActionFromMenu(hwnd, cmd, direction, isApp);
}

/* ── Helper: Update gesture button text ────────────────────────── */

static void UpdateGestureButton(HWND hwnd, int btnId, const KeyAction *action) {
    const char *name = GetActionDisplayName(action);
    SetDlgItemTextA(hwnd, btnId, name);
}

/* ── General Page ──────────────────────────────────────────────── */

static void InitGeneralPage(HWND hwnd) {
    /* Trigger key combo box */
    HWND combo = GetDlgItem(hwnd, IDC_TRIGGER_COMBO);
    const char *triggerKeys[] = {"F13","F14","F15","F16","F17","F18","F19","F20","F21","F22","F23","F24"};
    for (int i = 0; i < 12; i++) {
        SendMessageA(combo, CB_ADDSTRING, 0, (LPARAM)triggerKeys[i]);
    }
    const char *curTrig = VkDisplayName(s_cfg.trigger_vk);
    int idx = (int)SendMessageA(combo, CB_FINDSTRINGEXACT, -1, (LPARAM)curTrig);
    SendMessage(combo, CB_SETCURSEL, idx >= 0 ? idx : 11, 0);

    /* Tap action */
    SetDlgItemTextA(hwnd, IDC_TAP_EDIT, GetActionDisplayName(&s_cfg.tap_action));

    /* Spin edits are initialized during creation in WM_INITDIALOG */

    /* Checkboxes */
    CheckDlgButton(hwnd, IDC_SCROLL_CHECK, s_cfg.scroll_enabled ? BST_CHECKED : BST_UNCHECKED);
    CheckDlgButton(hwnd, IDC_REVERSE_CHECK, s_cfg.reverse_scroll ? BST_CHECKED : BST_UNCHECKED);
    CheckDlgButton(hwnd, IDC_STARTUP_CHECK, s_cfg.launch_on_startup ? BST_CHECKED : BST_UNCHECKED);
    UpdateScrollControls(s_cfg.scroll_enabled);
}

static void ReadGeneralPage(HWND hwnd) {
    /* Trigger key */
    char buf[32];
    GetDlgItemTextA(hwnd, IDC_TRIGGER_COMBO, buf, sizeof(buf));
    WORD vk = lookup_key(buf);
    if (vk) s_cfg.trigger_vk = vk;

    /* Spin edits — read via up-down control positions */
    s_cfg.gesture_threshold = (int)SendDlgItemMessage(hwnd, IDC_THRESHOLD_SLIDER + 500, UDM_GETPOS32, 0, 0);
    s_cfg.dead_zone = (int)SendDlgItemMessage(hwnd, IDC_DEADZONE_SLIDER + 500, UDM_GETPOS32, 0, 0);
    s_cfg.post_release_lock_ms = (int)SendDlgItemMessage(hwnd, IDC_LOCK_SLIDER + 500, UDM_GETPOS32, 0, 0);
    s_cfg.scroll_multiplier = (int)SendDlgItemMessage(hwnd, IDC_SCROLL_SLIDER + 500, UDM_GETPOS32, 0, 0);

    /* Checkboxes */
    s_cfg.scroll_enabled = IsDlgButtonChecked(hwnd, IDC_SCROLL_CHECK) == BST_CHECKED;
    s_cfg.reverse_scroll = IsDlgButtonChecked(hwnd, IDC_REVERSE_CHECK) == BST_CHECKED;
    s_cfg.launch_on_startup = IsDlgButtonChecked(hwnd, IDC_STARTUP_CHECK) == BST_CHECKED;
}

/* ── Dialog page procedures (code-created controls) ────────────── */

/* Apply the UI font to a control */
static void ApplyFont(HWND hw) {
    SendMessage(hw, WM_SETFONT, (WPARAM)(s_hFont ? s_hFont : GetStockObject(DEFAULT_GUI_FONT)), TRUE);
}

/* Helper: create a static label (wide string for Unicode arrows) */
static HWND CreateLabelW(HWND parent, const WCHAR *text, int x, int y, int w, int h, int id) {
    HWND hw = CreateWindowW(L"STATIC", text, WS_CHILD | WS_VISIBLE | SS_LEFT,
                            x, y, w, h, parent, (HMENU)(INT_PTR)id, s_hInst, NULL);
    ApplyFont(hw);
    return hw;
}

static HWND CreateLabel(HWND parent, const char *text, int x, int y, int w, int h, int id) {
    HWND hw = CreateWindowA("STATIC", text, WS_CHILD | WS_VISIBLE | SS_LEFT,
                            x, y, w, h, parent, (HMENU)(INT_PTR)id, s_hInst, NULL);
    ApplyFont(hw);
    return hw;
}

/* Helper: create a button */
static HWND CreateBtn(HWND parent, const char *text, int x, int y, int w, int h, int id) {
    HWND hw = CreateWindowA("BUTTON", text, WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                            x, y, w, h, parent, (HMENU)(INT_PTR)id, s_hInst, NULL);
    ApplyFont(hw);
    return hw;
}

/* Helper: create a spin edit (number input with up/down arrows) */
static HWND CreateSpinEdit(HWND parent, int x, int y, int w, int h, int id,
                           int minVal, int maxVal, int curVal, const char *suffix) {
    /* Edit box */
    HWND edit = CreateWindowA("EDIT", "",
        WS_CHILD | WS_VISIBLE | WS_BORDER | ES_NUMBER | ES_RIGHT,
        x, y, w - 18, h, parent, (HMENU)(INT_PTR)id, s_hInst, NULL);
    ApplyFont(edit);

    /* Up-down control (spinner) */
    HWND updown = CreateWindowA(UPDOWN_CLASSA, "",
        WS_CHILD | WS_VISIBLE | UDS_SETBUDDYINT | UDS_ALIGNRIGHT | UDS_ARROWKEYS | UDS_NOTHOUSANDS,
        0, 0, 0, 0, parent, (HMENU)(INT_PTR)(id + 500), s_hInst, NULL);
    SendMessage(updown, UDM_SETBUDDY, (WPARAM)edit, 0);
    SendMessage(updown, UDM_SETRANGE32, (WPARAM)minVal, (LPARAM)maxVal);
    SendMessage(updown, UDM_SETPOS32, 0, (LPARAM)curVal);

    /* Suffix label */
    if (suffix && *suffix) {
        CreateLabel(parent, suffix, x + w + 4, y + 3, 30, h, -1);
    }

    return edit;
}

/* Helper: create a checkbox */
static HWND CreateCheck(HWND parent, const char *text, int x, int y, int w, int h, int id) {
    HWND hw = CreateWindowA("BUTTON", text, WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
                            x, y, w, h, parent, (HMENU)(INT_PTR)id, s_hInst, NULL);
    ApplyFont(hw);
    return hw;
}

/* Center a window on its monitor's work area (DPI-safe) */
static void CenterOnScreen(HWND hwnd) {
    RECT rc;
    GetWindowRect(hwnd, &rc);
    int w = rc.right - rc.left;
    int h = rc.bottom - rc.top;
    HMONITOR mon = MonitorFromWindow(hwnd, MONITOR_DEFAULTTOPRIMARY);
    MONITORINFO mi;
    ZeroMemory(&mi, sizeof(mi));
    mi.cbSize = sizeof(mi);
    if (GetMonitorInfo(mon, &mi)) {
        int cx = mi.rcWork.left + (mi.rcWork.right - mi.rcWork.left - w) / 2;
        int cy = mi.rcWork.top  + (mi.rcWork.bottom - mi.rcWork.top - h) / 2;
        SetWindowPos(hwnd, NULL, cx, cy, 0, 0, SWP_NOSIZE | SWP_NOZORDER);
    }
}

/* Enable/disable scroll-dependent controls based on scroll_enabled state */
static void UpdateScrollControls(BOOL enabled) {
    EnableWindow(s_scrollLabel,  enabled);
    EnableWindow(s_scrollEdit,   enabled);
    EnableWindow(s_scrollUpDown, enabled);
    EnableWindow(s_reverseCheck, enabled);
}

static INT_PTR CALLBACK GeneralPageProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_INITDIALOG: {
        int x = 16, y = 12, lw = 130, ew = 70, sh = 22, gap = 30;

        /* ── Trigger ── */
        CreateLabel(hwnd, "Trigger Key", x, y + 3, lw, sh, -1);
        HWND combo = CreateWindowA("COMBOBOX", "", WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | WS_VSCROLL,
                                    x + lw, y, 100, 200, hwnd, (HMENU)(INT_PTR)IDC_TRIGGER_COMBO, s_hInst, NULL);
        ApplyFont(combo);
        y += gap;

        CreateLabel(hwnd, "Tap Action", x, y + 3, lw, sh, -1);
        HWND tapEdit = CreateWindowA("STATIC", "", WS_CHILD | WS_VISIBLE | SS_LEFT | SS_SUNKEN,
                                      x + lw, y, 160, sh, hwnd, (HMENU)(INT_PTR)IDC_TAP_EDIT, s_hInst, NULL);
        ApplyFont(tapEdit);
        CreateBtn(hwnd, "Record", x + lw + 165, y, 60, sh, IDC_TAP_RECORD);
        y += gap + 8;

        /* ── Gesture Tuning ── */
        CreateLabel(hwnd, "Gesture Threshold", x, y + 3, lw, sh, -1);
        CreateSpinEdit(hwnd, x + lw, y, ew, sh, IDC_THRESHOLD_SLIDER, 1, 500, s_cfg.gesture_threshold, "px");
        y += gap;

        CreateLabel(hwnd, "Dead Zone", x, y + 3, lw, sh, -1);
        CreateSpinEdit(hwnd, x + lw, y, ew, sh, IDC_DEADZONE_SLIDER, 0, 200, s_cfg.dead_zone, "px");
        y += gap;

        CreateLabel(hwnd, "Post-Release Lock", x, y + 3, lw, sh, -1);
        CreateSpinEdit(hwnd, x + lw, y, ew, sh, IDC_LOCK_SLIDER, 0, 2000, s_cfg.post_release_lock_ms, "ms");
        y += gap + 8;

        /* ── Scrolling ── */
        CreateCheck(hwnd, "Enable Horizontal Scrolling", x, y, 260, sh, IDC_SCROLL_CHECK);
        y += gap;

        s_scrollLabel = CreateLabel(hwnd, "Scroll Multiplier", x, y + 3, lw, sh, -1);
        s_scrollEdit = CreateSpinEdit(hwnd, x + lw, y, ew, sh, IDC_SCROLL_SLIDER, 1, 20, s_cfg.scroll_multiplier, "x");
        s_scrollUpDown = GetDlgItem(hwnd, IDC_SCROLL_SLIDER + 500);
        y += gap;

        s_reverseCheck = CreateCheck(hwnd, "Reverse Scroll Direction", x, y, 260, sh, IDC_REVERSE_CHECK);
        y += gap + 4;

        /* ── Startup ── */
        CreateCheck(hwnd, "Launch on Windows startup", x, y, 260, sh, IDC_STARTUP_CHECK);

        InitGeneralPage(hwnd);
        s_generalInitDone = TRUE;

        /* Fallback: re-center the PropertySheet after pages are created */
        CenterOnScreen(GetParent(hwnd));
        return TRUE;
    }

    case WM_NOTIFY: {
        NMHDR *nm = (NMHDR *)lParam;
        /* Spin control value changed */
        if (nm->code == UDN_DELTAPOS) {
            MarkDirty(hwnd);
        }
        if (nm->code == PSN_APPLY) {
            ReadGeneralPage(hwnd);
            SaveAndReload();
            s_dirty = FALSE;
            SetWindowLongPtr(hwnd, DWLP_MSGRESULT, PSNRET_NOERROR);
            return TRUE;
        }
        break;
    }

    case WM_COMMAND:
        if (LOWORD(wParam) == IDC_SCROLL_CHECK) {
            BOOL enabled = IsDlgButtonChecked(hwnd, IDC_SCROLL_CHECK) == BST_CHECKED;
            UpdateScrollControls(enabled);
            MarkDirty(hwnd);
        }
        if (LOWORD(wParam) == IDC_TAP_RECORD) {
            char buf[MAX_ACTION_STR] = {0};
            if (RecordKeystroke(hwnd, buf, sizeof(buf)) && buf[0]) {
                strncpy(s_cfg.tap_action.configStr, buf, MAX_ACTION_STR - 1);
                s_cfg.tap_action.type = ACTION_KEYS;
                SetDlgItemTextA(hwnd, IDC_TAP_EDIT, buf);
                MarkDirty(hwnd);
            }
        }
        if (LOWORD(wParam) == IDC_TRIGGER_COMBO && HIWORD(wParam) == CBN_SELCHANGE) {
            MarkDirty(hwnd);
        }
        if (LOWORD(wParam) == IDC_REVERSE_CHECK || LOWORD(wParam) == IDC_STARTUP_CHECK) {
            MarkDirty(hwnd);
        }
        /* Direct typing in spin edit controls */
        if (HIWORD(wParam) == EN_CHANGE && s_generalInitDone) {
            int id = LOWORD(wParam);
            if (id == IDC_THRESHOLD_SLIDER || id == IDC_DEADZONE_SLIDER ||
                id == IDC_LOCK_SLIDER || id == IDC_SCROLL_SLIDER) {
                MarkDirty(hwnd);
            }
        }
        return TRUE;
    }
    return FALSE;
}

/* ── Default Gestures Page ─────────────────────────────────────── */

static INT_PTR CALLBACK GesturesPageProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_INITDIALOG: {
        int x = 16, y = 14, bw = 260, bh = 28;
        const WCHAR *arrows[] = {L"\x2191 Up", L"\x2193 Down", L"\x2190 Left", L"\x2192 Right"};

        CreateLabel(hwnd, "Click a button to change the gesture action.", x, y, 380, 18, -1);
        y += 30;

        for (int i = 0; i < DIR_COUNT; i++) {
            CreateLabelW(hwnd, arrows[i], x, y + 5, 55, bh, -1);
            CreateBtn(hwnd, GetActionDisplayName(&s_cfg.defaults[i]),
                      x + 60, y, bw, bh, IDC_GESTURE_BTN_UP + i);
            y += bh + 8;
        }

        y += 12;
        CreateLabel(hwnd, "Tip: Custom Keystroke records your key combo.", x, y, 380, 16, -1);
        y += 18;
        CreateLabel(hwnd, "Key Sequence records multi-step combos (e.g., Ctrl+K, Ctrl+C).", x, y, 380, 16, -1);
        y += 18;
        CreateLabel(hwnd, "Type \"run:program.exe\" for launch actions.", x, y, 380, 16, -1);
        return TRUE;
    }

    case WM_COMMAND: {
        int id = LOWORD(wParam);
        if (id >= IDC_GESTURE_BTN_UP && id <= IDC_GESTURE_BTN_RIGHT) {
            int dir = id - IDC_GESTURE_BTN_UP;
            ShowActionMenu(hwnd, dir, FALSE);
            /* Update button text */
            UpdateGestureButton(hwnd, id, &s_cfg.defaults[dir]);
        }
        return TRUE;
    }

    case WM_NOTIFY: {
        NMHDR *nm = (NMHDR *)lParam;
        if (nm->code == PSN_APPLY) {
            SaveAndReload();
            s_dirty = FALSE;
            SetWindowLongPtr(hwnd, DWLP_MSGRESULT, PSNRET_NOERROR);
            return TRUE;
        }
        break;
    }
    }
    return FALSE;
}

/* ── App Overrides Page ────────────────────────────────────────── */

static int s_selectedApp = -1;

static void RefreshAppList(HWND hwnd) {
    HWND list = GetDlgItem(hwnd, IDC_APP_LIST);
    SendMessage(list, LB_RESETCONTENT, 0, 0);
    for (int i = 0; i < s_cfg.numApps; i++) {
        SendMessageA(list, LB_ADDSTRING, 0, (LPARAM)s_cfg.apps[i].name);
    }
    s_selectedApp = -1;
}

static void UpdateAppOverridePanel(HWND hwnd) {
    BOOL show = (s_selectedApp >= 0 && s_selectedApp < s_cfg.numApps);
    const AppOverride *ov = show ? &s_cfg.apps[s_selectedApp] : NULL;

    for (int d = 0; d < DIR_COUNT; d++) {
        EnableWindow(GetDlgItem(hwnd, IDC_APP_CHECK_UP + d), show);
        EnableWindow(GetDlgItem(hwnd, IDC_APP_BTN_UP + d), show);

        if (show && ov) {
            CheckDlgButton(hwnd, IDC_APP_CHECK_UP + d,
                           ov->hasAction[d] ? BST_CHECKED : BST_UNCHECKED);
            const char *name = ov->hasAction[d]
                ? GetActionDisplayName(&ov->actions[d])
                : "(default)";
            SetDlgItemTextA(hwnd, IDC_APP_BTN_UP + d, name);
        } else {
            CheckDlgButton(hwnd, IDC_APP_CHECK_UP + d, BST_UNCHECKED);
            SetDlgItemTextA(hwnd, IDC_APP_BTN_UP + d, "");
        }
    }

    if (show && ov)
        SetDlgItemTextA(hwnd, IDC_APP_NAME_LABEL, ov->name);
    else
        SetDlgItemTextA(hwnd, IDC_APP_NAME_LABEL, "");
}

/* ── App Picker Dialog ─────────────────────────────────────────── */

typedef struct {
    char exeName[MAX_PATH];
    char exePath[MAX_PATH];
    char description[256];
} ProcessInfo;

static ProcessInfo *s_processes = NULL;  /* heap-allocated when picker opens */
static int s_numProcesses = 0;
#define MAX_PROCESSES 256

static int CompareProcessInfo(const void *a, const void *b) {
    return _stricmp(((const ProcessInfo *)a)->exeName,
                    ((const ProcessInfo *)b)->exeName);
}

static void EnumerateProcesses(void) {
    s_numProcesses = 0;
    if (!s_processes) {
        s_processes = (ProcessInfo *)malloc(MAX_PROCESSES * sizeof(ProcessInfo));
        if (!s_processes) return;
    }

    DWORD pids[1024];
    DWORD needed;
    if (!EnumProcesses(pids, sizeof(pids), &needed)) return;
    int count = needed / sizeof(DWORD);

    for (int i = 0; i < count && s_numProcesses < MAX_PROCESSES; i++) {
        if (pids[i] == 0 || pids[i] == 4) continue; /* skip System */

        HANDLE proc = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pids[i]);
        if (!proc) continue;

        char path[MAX_PATH] = {0};
        DWORD pathSize = MAX_PATH;
        if (QueryFullProcessImageNameA(proc, 0, path, &pathSize)) {
            /* Extract filename */
            char *slash = strrchr(path, '\\');
            const char *name = slash ? slash + 1 : path;

            /* Deduplicate */
            BOOL dup = FALSE;
            for (int j = 0; j < s_numProcesses; j++) {
                if (_stricmp(s_processes[j].exeName, name) == 0) { dup = TRUE; break; }
            }
            if (!dup) {
                ProcessInfo *pi = &s_processes[s_numProcesses];
                strncpy(pi->exeName, name, MAX_PATH - 1);
                strncpy(pi->exePath, path, MAX_PATH - 1);
                pi->description[0] = '\0';

                /* Get version info description */
                DWORD verHandle;
                DWORD verSize = GetFileVersionInfoSizeA(path, &verHandle);
                if (verSize) {
                    void *verData = malloc(verSize);
                    if (verData && GetFileVersionInfoA(path, verHandle, verSize, verData)) {
                        struct { WORD lang; WORD codepage; } *trans;
                        UINT transLen;
                        if (VerQueryValueA(verData, "\\VarFileInfo\\Translation", (void**)&trans, &transLen) && transLen >= 4) {
                            char subBlock[128];
                            snprintf(subBlock, sizeof(subBlock),
                                     "\\StringFileInfo\\%04x%04x\\FileDescription",
                                     trans[0].lang, trans[0].codepage);
                            char *desc = NULL;
                            UINT descLen;
                            if (VerQueryValueA(verData, subBlock, (void**)&desc, &descLen) && desc)
                                strncpy(pi->description, desc, 255);
                        }
                    }
                    free(verData);
                }

                s_numProcesses++;
            }
        }
        CloseHandle(proc);
    }

    qsort(s_processes, s_numProcesses, sizeof(ProcessInfo), CompareProcessInfo);
}

static void FreeProcessList(void) {
    free(s_processes);
    s_processes = NULL;
    s_numProcesses = 0;
}

static char s_pickerResult[MAX_APP_NAME] = {0};

static INT_PTR CALLBACK AppPickerDlgProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    (void)lParam;
    switch (msg) {
    case WM_INITDIALOG: {
        /* Force exact pixel size — child controls use hardcoded pixel coords */
        SetWindowPos(hwnd, NULL, 0, 0, 396, 320, SWP_NOMOVE | SWP_NOZORDER);

        /* Filter edit */
        HWND filter = CreateWindowA("EDIT", "", WS_CHILD | WS_VISIBLE | WS_BORDER | ES_AUTOHSCROLL,
                                     10, 10, 360, 22, hwnd, (HMENU)(INT_PTR)IDC_PICKER_FILTER, s_hInst, NULL);
        SendMessage(filter, WM_SETFONT, (WPARAM)GetStockObject(DEFAULT_GUI_FONT), TRUE);
        SendMessageW(filter, EM_SETCUEBANNER, 0, (LPARAM)L"Filter...");

        /* Process list */
        HWND list = CreateWindowA("LISTBOX", "", WS_CHILD | WS_VISIBLE | WS_BORDER | WS_VSCROLL | LBS_NOTIFY,
                                   10, 38, 360, 220, hwnd, (HMENU)(INT_PTR)IDC_PICKER_LIST, s_hInst, NULL);
        SendMessage(list, WM_SETFONT, (WPARAM)GetStockObject(DEFAULT_GUI_FONT), TRUE);

        /* Buttons */
        CreateBtn(hwnd, "Browse...", 10, 265, 80, 25, IDC_PICKER_BROWSE);
        CreateBtn(hwnd, "OK", 210, 265, 75, 25, IDOK);
        CreateBtn(hwnd, "Cancel", 295, 265, 75, 25, IDCANCEL);

        /* Populate */
        EnumerateProcesses();
        SendMessage(list, WM_SETREDRAW, FALSE, 0);
        for (int i = 0; i < s_numProcesses; i++) {
            char display[512];
            if (s_processes[i].description[0])
                snprintf(display, sizeof(display), "%s  -  %s", s_processes[i].exeName, s_processes[i].description);
            else
                snprintf(display, sizeof(display), "%s", s_processes[i].exeName);
            SendMessageA(list, LB_ADDSTRING, 0, (LPARAM)display);
        }
        SendMessage(list, WM_SETREDRAW, TRUE, 0);

        SetFocus(filter);
        return FALSE;
    }

    case WM_COMMAND:
        if (LOWORD(wParam) == IDC_PICKER_FILTER && HIWORD(wParam) == EN_CHANGE) {
            char filter[64];
            GetDlgItemTextA(hwnd, IDC_PICKER_FILTER, filter, sizeof(filter));
            HWND list = GetDlgItem(hwnd, IDC_PICKER_LIST);
            SendMessage(list, WM_SETREDRAW, FALSE, 0);
            SendMessage(list, LB_RESETCONTENT, 0, 0);
            for (int i = 0; i < s_numProcesses; i++) {
                /* Case-insensitive substring match on name + description */
                char combined[512];
                snprintf(combined, sizeof(combined), "%s %s", s_processes[i].exeName, s_processes[i].description);
                /* Simple case-insensitive contains */
                BOOL match = (filter[0] == '\0');
                if (!match) {
                    for (char *p = combined; *p; p++) {
                        if (_strnicmp(p, filter, strlen(filter)) == 0) { match = TRUE; break; }
                    }
                }
                if (match) {
                    char display[512];
                    if (s_processes[i].description[0])
                        snprintf(display, sizeof(display), "%s  -  %s", s_processes[i].exeName, s_processes[i].description);
                    else
                        snprintf(display, sizeof(display), "%s", s_processes[i].exeName);
                    SendMessageA(list, LB_ADDSTRING, 0, (LPARAM)display);
                }
            }
            SendMessage(list, WM_SETREDRAW, TRUE, 0);
            InvalidateRect(list, NULL, TRUE);
        }
        if (LOWORD(wParam) == IDOK) {
            HWND list = GetDlgItem(hwnd, IDC_PICKER_LIST);
            int sel = (int)SendMessage(list, LB_GETCURSEL, 0, 0);
            if (sel >= 0) {
                char text[512];
                SendMessageA(list, LB_GETTEXT, sel, (LPARAM)text);
                /* Extract exe name (before " - ") */
                char *dash = strstr(text, "  -  ");
                if (dash) *dash = '\0';
                /* Trim */
                char *end = text + strlen(text) - 1;
                while (end > text && *end == ' ') *end-- = '\0';
                strncpy(s_pickerResult, text, MAX_APP_NAME - 1);
                EndDialog(hwnd, IDOK);
            }
        }
        if (LOWORD(wParam) == IDCANCEL) {
            s_pickerResult[0] = '\0';
            EndDialog(hwnd, IDCANCEL);
        }
        if (LOWORD(wParam) == IDC_PICKER_BROWSE) {
            OPENFILENAMEA ofn;
            char path[MAX_PATH] = {0};
            ZeroMemory(&ofn, sizeof(ofn));
            ofn.lStructSize = sizeof(ofn);
            ofn.hwndOwner   = hwnd;
            ofn.lpstrFilter  = "Executables\0*.exe\0";
            ofn.lpstrFile    = path;
            ofn.nMaxFile     = MAX_PATH;
            ofn.Flags        = OFN_FILEMUSTEXIST;
            if (GetOpenFileNameA(&ofn)) {
                char *slash = strrchr(path, '\\');
                strncpy(s_pickerResult, slash ? slash + 1 : path, MAX_APP_NAME - 1);
                EndDialog(hwnd, IDOK);
            }
        }
        /* Double-click on list = OK */
        if (LOWORD(wParam) == IDC_PICKER_LIST && HIWORD(wParam) == LBN_DBLCLK) {
            SendMessage(hwnd, WM_COMMAND, MAKEWPARAM(IDOK, 0), 0);
        }
        return TRUE;
    }
    return FALSE;
}

static BOOL ShowAppPickerDialog(HWND parent) {
    s_pickerResult[0] = '\0';

    /* In-memory DLGTEMPLATE — shell only, child controls created in WM_INITDIALOG */
    #pragma pack(push, 4)
    struct {
        DLGTEMPLATE tmpl;
        WORD menu;
        WORD cls;
        WCHAR title[20];
    } dlg;
    #pragma pack(pop)

    ZeroMemory(&dlg, sizeof(dlg));
    dlg.tmpl.style = WS_POPUP | WS_CAPTION | WS_SYSMENU | DS_MODALFRAME | DS_CENTER;
    dlg.tmpl.dwExtendedStyle = WS_EX_TOPMOST;
    dlg.tmpl.cdit  = 0;
    dlg.tmpl.cx    = 264;   /* dialog units, approx 396px */
    dlg.tmpl.cy    = 213;   /* dialog units, approx 320px */
    dlg.menu = 0;
    dlg.cls  = 0;
    wcscpy(dlg.title, L"Select Application");

    DialogBoxIndirectParam(s_hInst, &dlg.tmpl, parent, AppPickerDlgProc, 0);
    FreeProcessList();

    return (s_pickerResult[0] != '\0');
}

static INT_PTR CALLBACK AppsPageProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_INITDIALOG: {
        int lx = 10, ly = 10, lw = 150;  /* left panel */
        int rx = 170, ry = 10, rw = 220;  /* right panel */
        int sh = 22;

        /* Left: app list */
        CreateLabel(hwnd, "Applications", lx, ly, lw, 16, -1);
        ly += 20;

        HWND list = CreateWindowA("LISTBOX", "",
            WS_CHILD | WS_VISIBLE | WS_BORDER | WS_VSCROLL | LBS_NOTIFY,
            lx, ly, lw, 180, hwnd, (HMENU)(INT_PTR)IDC_APP_LIST, s_hInst, NULL);
        SendMessage(list, WM_SETFONT, (WPARAM)GetStockObject(DEFAULT_GUI_FONT), TRUE);
        ly += 185;

        CreateBtn(hwnd, "+ Add", lx, ly, 70, sh, IDC_APP_ADD);
        CreateBtn(hwnd, "- Remove", lx + 75, ly, 70, sh, IDC_APP_REMOVE);

        /* Right: override panel */
        CreateLabel(hwnd, "", rx, ry, rw, 16, IDC_APP_NAME_LABEL);
        ry += 24;

        const WCHAR *arrows[] = {L"\x2191 Up", L"\x2193 Down", L"\x2190 Left", L"\x2192 Right"};
        for (int d = 0; d < DIR_COUNT; d++) {
            HWND chk = CreateWindowW(L"BUTTON", arrows[d], WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
                                      rx, ry + 3, 60, sh, hwnd, (HMENU)(INT_PTR)(IDC_APP_CHECK_UP + d), s_hInst, NULL);
            ApplyFont(chk);
            CreateBtn(hwnd, "(default)", rx + 65, ry, 150, sh, IDC_APP_BTN_UP + d);
            ry += sh + 6;
        }

        ry += 8;
        CreateLabel(hwnd, "Unchecked = uses default gesture", rx, ry, rw, 16, -1);

        RefreshAppList(hwnd);
        UpdateAppOverridePanel(hwnd);
        return TRUE;
    }

    case WM_COMMAND: {
        int id = LOWORD(wParam);

        /* App list selection changed */
        if (id == IDC_APP_LIST && HIWORD(wParam) == LBN_SELCHANGE) {
            s_selectedApp = (int)SendDlgItemMessage(hwnd, IDC_APP_LIST, LB_GETCURSEL, 0, 0);
            UpdateAppOverridePanel(hwnd);
        }

        /* Add app */
        if (id == IDC_APP_ADD) {
            if (s_cfg.numApps >= MAX_APPS) {
                MessageBoxA(hwnd, "Maximum number of app overrides reached.", "Gesture Engine", MB_OK);
                return TRUE;
            }
            if (ShowAppPickerDialog(hwnd)) {
                AppOverride *ov = &s_cfg.apps[s_cfg.numApps];
                memset(ov, 0, sizeof(*ov));
                strncpy(ov->name, s_pickerResult, MAX_APP_NAME - 1);
                for (char *p = ov->name; *p; p++) *p = (char)tolower((unsigned char)*p);
                s_cfg.numApps++;
                RefreshAppList(hwnd);
                /* Select the new app */
                SendDlgItemMessage(hwnd, IDC_APP_LIST, LB_SETCURSEL, s_cfg.numApps - 1, 0);
                s_selectedApp = s_cfg.numApps - 1;
                UpdateAppOverridePanel(hwnd);
                MarkDirty(hwnd);
            }
        }

        /* Remove app */
        if (id == IDC_APP_REMOVE && s_selectedApp >= 0 && s_selectedApp < s_cfg.numApps) {
            /* Shift remaining apps down */
            for (int i = s_selectedApp; i < s_cfg.numApps - 1; i++)
                s_cfg.apps[i] = s_cfg.apps[i + 1];
            s_cfg.numApps--;
            RefreshAppList(hwnd);
            s_selectedApp = -1;
            UpdateAppOverridePanel(hwnd);
            MarkDirty(hwnd);
        }

        /* App gesture buttons */
        if (id >= IDC_APP_BTN_UP && id <= IDC_APP_BTN_RIGHT) {
            int dir = id - IDC_APP_BTN_UP;
            if (s_selectedApp >= 0) {
                ShowActionMenu(hwnd, dir, TRUE);
                UpdateAppOverridePanel(hwnd);
            }
        }

        /* App override checkboxes */
        if (id >= IDC_APP_CHECK_UP && id <= IDC_APP_CHECK_RIGHT) {
            int dir = id - IDC_APP_CHECK_UP;
            if (s_selectedApp >= 0 && s_selectedApp < s_cfg.numApps) {
                BOOL checked = IsDlgButtonChecked(hwnd, id) == BST_CHECKED;
                s_cfg.apps[s_selectedApp].hasAction[dir] = checked;
                if (!checked) {
                    memset(&s_cfg.apps[s_selectedApp].actions[dir], 0, sizeof(KeyAction));
                }
                UpdateAppOverridePanel(hwnd);
                MarkDirty(hwnd);
            }
        }
        return TRUE;
    }

    case WM_NOTIFY: {
        NMHDR *nm = (NMHDR *)lParam;
        if (nm->code == PSN_APPLY) {
            SaveAndReload();
            s_dirty = FALSE;
            SetWindowLongPtr(hwnd, DWLP_MSGRESULT, PSNRET_NOERROR);
            return TRUE;
        }
        break;
    }
    }
    return FALSE;
}

/* ── PropertySheet Setup ───────────────────────────────────────── */

/* Create in-memory dialog templates for PropertySheet pages */
static DLGTEMPLATE* CreatePageTemplate(int width, int height) {
    /* Minimal DLGTEMPLATE with no controls */
    #pragma pack(push, 4)
    static struct {
        DLGTEMPLATE tmpl;
        WORD menu, cls, title;
    } tmpl;
    #pragma pack(pop)

    ZeroMemory(&tmpl, sizeof(tmpl));
    tmpl.tmpl.style = DS_SETFONT | WS_CHILD | WS_VISIBLE;
    tmpl.tmpl.cx = (short)width;
    tmpl.tmpl.cy = (short)height;

    return &tmpl.tmpl;
}

/* Center the PropertySheet after it reaches final size */
static int CALLBACK PropSheetCallback(HWND hwndDlg, UINT uMsg, LPARAM lParam) {
    (void)lParam;
    if (uMsg == PSCB_INITIALIZED)
        CenterOnScreen(hwndDlg);
    return 0;
}

void ShowSettingsDialog(HWND parent, const char *configPath) {
    if (s_dialogOpen) return;
    s_dialogOpen = TRUE;

    s_hInst = GetModuleHandle(NULL);
    s_engineHwnd = parent;
    strncpy(s_configPath, configPath, MAX_PATH - 1);

    /* Load config into working copy */
    if (config_load(&s_cfg, configPath) != 0) {
        MessageBoxA(parent, "Failed to load config.", "Gesture Engine", MB_ICONERROR);
        return;
    }
    s_dirty = FALSE;
    s_generalInitDone = FALSE;

    /* Create modern UI font (Segoe UI) */
    if (!s_hFont) {
        s_hFont = CreateFontW(-13, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                               DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY,
                               DEFAULT_PITCH | FF_SWISS, L"Segoe UI");
    }

    /* Init common controls */
    INITCOMMONCONTROLSEX icc = { sizeof(icc), ICC_BAR_CLASSES | ICC_LISTVIEW_CLASSES | ICC_UPDOWN_CLASS };
    InitCommonControlsEx(&icc);

    /* Create page templates (in dialog units, ~400x280 pixels at 96dpi) */
    DLGTEMPLATE *pageTmpl = CreatePageTemplate(280, 200);

    PROPSHEETPAGEW pages[3];
    ZeroMemory(pages, sizeof(pages));

    pages[0].dwSize      = sizeof(PROPSHEETPAGEW);
    pages[0].dwFlags     = PSP_DLGINDIRECT;
    pages[0].pResource   = pageTmpl;
    pages[0].pfnDlgProc  = GeneralPageProc;
    pages[0].pszTitle     = L"General";
    pages[0].dwFlags     |= PSP_USETITLE;

    pages[1].dwSize      = sizeof(PROPSHEETPAGEW);
    pages[1].dwFlags     = PSP_DLGINDIRECT | PSP_USETITLE;
    pages[1].pResource   = pageTmpl;
    pages[1].pfnDlgProc  = GesturesPageProc;
    pages[1].pszTitle     = L"Gestures";

    pages[2].dwSize      = sizeof(PROPSHEETPAGEW);
    pages[2].dwFlags     = PSP_DLGINDIRECT | PSP_USETITLE;
    pages[2].pResource   = pageTmpl;
    pages[2].pfnDlgProc  = AppsPageProc;
    pages[2].pszTitle     = L"Apps";

    PROPSHEETHEADERW psh;
    ZeroMemory(&psh, sizeof(psh));
    psh.dwSize      = sizeof(psh);
    psh.dwFlags     = PSH_PROPSHEETPAGE | PSH_USECALLBACK;
    psh.hwndParent  = NULL;         /* NULL — message-only parent causes top-left positioning */
    psh.hInstance    = s_hInst;
    psh.pszCaption   = L"Gesture Engine Settings";
    psh.pfnCallback  = PropSheetCallback;
    psh.nPages       = 3;
    psh.ppsp         = pages;

    PropertySheetW(&psh);
    s_dialogOpen = FALSE;
    /* Save/reload happens in PSN_APPLY handlers via SaveAndReload() */
}
