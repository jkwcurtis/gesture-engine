#include "actions.h"
#include <string.h>

#ifdef _MSC_VER
#define strcasecmp _stricmp
#else
#include <strings.h>
#endif

/* ── Category list ─────────────────────────────────────────────── */

const char *ACTION_CATEGORIES[] = {
    "Navigation", "Windows", "Media", "System", "Clipboard", "Mouse", NULL
};

/* ── Built-in action registry ──────────────────────────────────── */

static const BuiltinAction BUILTIN_ACTIONS[] = {
    /* Navigation */
    {"back",        "Back",               "Navigation", "alt+left"},
    {"forward",     "Forward",            "Navigation", "alt+right"},
    {"new_tab",     "New Tab",            "Navigation", "ctrl+t"},
    {"close_tab",   "Close Tab",          "Navigation", "ctrl+w"},
    {"reopen_tab",  "Reopen Closed Tab",  "Navigation", "ctrl+shift+t"},
    {"next_tab",    "Next Tab",           "Navigation", "ctrl+pagedown"},
    {"prev_tab",    "Previous Tab",       "Navigation", "ctrl+pageup"},
    {"switch_app",  "Switch Application", "Navigation", "alt+tab"},

    /* Windows */
    {"show_desktop",  "Show Desktop",          "Windows", "win+d"},
    {"task_view",     "Task View",             "Windows", "win+tab"},
    {"minimize",      "Minimize Window",       "Windows", "win+down"},
    {"maximize",      "Maximize Window",       "Windows", "win+up"},
    {"snap_left",     "Snap Left",             "Windows", "win+left"},
    {"snap_right",    "Snap Right",            "Windows", "win+right"},
    {"vdesk_left",    "Virtual Desktop Left",  "Windows", "win+ctrl+left"},
    {"vdesk_right",   "Virtual Desktop Right", "Windows", "win+ctrl+right"},
    {"close_window",  "Close Window",          "Windows", "alt+f4"},

    /* Media */
    {"vol_up",      "Volume Up",       "Media", "volumeup"},
    {"vol_down",    "Volume Down",     "Media", "volumedown"},
    {"mute",        "Mute",            "Media", "volumemute"},
    {"play_pause",  "Play / Pause",    "Media", "mediaplaypause"},
    {"next_track",  "Next Track",      "Media", "medianexttrack"},
    {"prev_track",  "Previous Track",  "Media", "mediaprevtrack"},

    /* System */
    {"lock_screen",  "Lock Screen",     "System", "win+l"},
    {"screenshot",   "Screenshot",      "System", "win+shift+s"},
    {"emoji",        "Emoji Menu",      "System", "win+period"},
    {"settings",     "Settings",        "System", "win+i"},
    {"calculator",   "Calculator",      "System", "run:calc"},

    /* Clipboard */
    {"copy",          "Copy",              "Clipboard", "ctrl+c"},
    {"cut",           "Cut",               "Clipboard", "ctrl+x"},
    {"paste",         "Paste",             "Clipboard", "ctrl+v"},
    {"undo",          "Undo",              "Clipboard", "ctrl+z"},
    {"redo",          "Redo",              "Clipboard", "ctrl+y"},
    {"clip_history",  "Clipboard History", "Clipboard", "win+v"},

    /* Mouse */
    {"middle_click",  "Middle Click",  "Mouse", "mclick"},
    {"double_click",  "Double Click",  "Mouse", "lclick,lclick"},
};

#define BUILTIN_COUNT (sizeof(BUILTIN_ACTIONS) / sizeof(BUILTIN_ACTIONS[0]))

const char* action_lookup(const char *id) {
    if (!id || !*id) return NULL;
    for (int i = 0; i < (int)BUILTIN_COUNT; i++) {
        if (strcasecmp(id, BUILTIN_ACTIONS[i].id) == 0)
            return BUILTIN_ACTIONS[i].keys;
    }
    return NULL;
}

const BuiltinAction* action_get_all(int *count) {
    if (count) *count = (int)BUILTIN_COUNT;
    return BUILTIN_ACTIONS;
}
