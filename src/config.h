#ifndef CONFIG_H
#define CONFIG_H

#include <windows.h>

#define MAX_MODIFIERS    4
#define MAX_COMBO_STEPS  4
#define MAX_APPS         32
#define MAX_APP_NAME     64
#define MAX_RUN_TARGET   260
#define MAX_ACTION_STR   256
#define DIR_COUNT        4

/* Directions */
enum { DIR_UP = 0, DIR_DOWN, DIR_LEFT, DIR_RIGHT };

/* Action types */
typedef enum {
    ACTION_NONE = 0,
    ACTION_KEYS,        /* keystroke combo(s): single or multi-step sequence */
    ACTION_RUN          /* launch a program */
} ActionType;

/* A single key combination (e.g., Ctrl+Shift+Tab) */
typedef struct {
    WORD modifiers[MAX_MODIFIERS];  /* VK codes for modifier keys */
    int  numModifiers;
    WORD key;                       /* VK code for the main key */
    BOOL isMouse;                   /* TRUE if key is a mouse button VK */
} KeyCombo;

/* A gesture action: one or more key combos, or a run command */
typedef struct {
    ActionType type;
    KeyCombo steps[MAX_COMBO_STEPS];
    int      numSteps;              /* 1 = single combo, >1 = sequence */
    char     runTarget[MAX_RUN_TARGET];
    char     configStr[MAX_ACTION_STR]; /* original config string for UI display */
} KeyAction;

/* Per-app gesture override */
typedef struct {
    char      name[MAX_APP_NAME];       /* lowercase process name */
    KeyAction actions[DIR_COUNT];       /* indexed by DIR_UP..DIR_RIGHT */
    BOOL      hasAction[DIR_COUNT];     /* which directions are overridden */
} AppOverride;

/* Full configuration */
typedef struct {
    /* Tuning */
    int  gesture_threshold;
    int  dead_zone;
    BOOL scroll_enabled;
    int  scroll_multiplier;
    BOOL reverse_scroll;
    int  post_release_lock_ms;

    /* Trigger */
    WORD trigger_vk;                /* VK code for gesture trigger (default: VK_F24) */
    KeyAction tap_action;           /* action on tap (default: ctrl+lclick) */

    /* Startup */
    BOOL launch_on_startup;

    /* Default gestures */
    KeyAction defaults[DIR_COUNT];

    /* Per-app overrides */
    AppOverride apps[MAX_APPS];
    int numApps;
} GestureConfig;

/* Lookup a key name string ("ctrl", "f24", "a") → VK code. Returns 0 if not found. */
WORD lookup_key(const char *name);

/* Reverse lookup: VK code → key name string. Returns "?" if not found. */
const char* vk_to_name(WORD vk);

/* Load config from JSON file. Returns 0 on success. */
int config_load(GestureConfig *cfg, const char *path);

/* Save config to JSON file. Returns 0 on success. */
int config_save(const GestureConfig *cfg, const char *path);

/* Get the action for a direction, with app fallthrough to defaults. */
const KeyAction* config_get_action(const GestureConfig *cfg,
                                   const char *processName, int direction);

#endif /* CONFIG_H */
