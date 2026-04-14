#include "config.h"
#include "actions.h"
#include "cJSON.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

/* ── Key name → VK code mapping ─────────────────────────────────── */

typedef struct { const char *name; WORD vk; BOOL isMouse; } KeyEntry;

static const KeyEntry KEY_TABLE[] = {
    /* Modifiers */
    {"ctrl",VK_LCONTROL,0}, {"shift",VK_LSHIFT,0}, {"alt",VK_LMENU,0}, {"win",VK_LWIN,0},

    /* Navigation */
    {"tab",VK_TAB,0}, {"enter",VK_RETURN,0}, {"return",VK_RETURN,0},
    {"escape",VK_ESCAPE,0}, {"esc",VK_ESCAPE,0}, {"space",VK_SPACE,0},
    {"backspace",VK_BACK,0}, {"delete",VK_DELETE,0}, {"del",VK_DELETE,0},
    {"insert",VK_INSERT,0}, {"ins",VK_INSERT,0},
    {"home",VK_HOME,0}, {"end",VK_END,0},
    {"pageup",VK_PRIOR,0}, {"pgup",VK_PRIOR,0},
    {"pagedown",VK_NEXT,0}, {"pgdn",VK_NEXT,0},

    /* Arrows */
    {"up",VK_UP,0}, {"down",VK_DOWN,0}, {"left",VK_LEFT,0}, {"right",VK_RIGHT,0},

    /* Punctuation / symbols */
    {"minus",VK_OEM_MINUS,0}, {"plus",VK_OEM_PLUS,0}, {"equal",VK_OEM_PLUS,0},
    {"comma",VK_OEM_COMMA,0}, {"period",VK_OEM_PERIOD,0},
    {"semicolon",VK_OEM_1,0}, {"slash",VK_OEM_2,0}, {"backslash",VK_OEM_5,0},
    {"lbracket",VK_OEM_4,0}, {"rbracket",VK_OEM_6,0},

    /* F-keys */
    {"f1",VK_F1,0},{"f2",VK_F2,0},{"f3",VK_F3,0},{"f4",VK_F4,0},{"f5",VK_F5,0},
    {"f6",VK_F6,0},{"f7",VK_F7,0},{"f8",VK_F8,0},{"f9",VK_F9,0},{"f10",VK_F10,0},
    {"f11",VK_F11,0},{"f12",VK_F12,0},{"f13",VK_F13,0},{"f14",VK_F14,0},
    {"f15",VK_F15,0},{"f16",VK_F16,0},{"f17",VK_F17,0},{"f18",VK_F18,0},
    {"f19",VK_F19,0},{"f20",VK_F20,0},{"f21",VK_F21,0},{"f22",VK_F22,0},
    {"f23",VK_F23,0},{"f24",VK_F24,0},

    /* Mouse buttons */
    {"lclick",VK_LBUTTON,1}, {"rclick",VK_RBUTTON,1}, {"mclick",VK_MBUTTON,1},

    /* Media keys */
    {"volumeup",VK_VOLUME_UP,0}, {"volumedown",VK_VOLUME_DOWN,0},
    {"volumemute",VK_VOLUME_MUTE,0},
    {"mediaplaypause",VK_MEDIA_PLAY_PAUSE,0},
    {"medianexttrack",VK_MEDIA_NEXT_TRACK,0},
    {"mediaprevtrack",VK_MEDIA_PREV_TRACK,0},

    {NULL, 0, 0}
};

static BOOL is_modifier(WORD vk) {
    return vk == VK_LCONTROL || vk == VK_RCONTROL ||
           vk == VK_LSHIFT   || vk == VK_RSHIFT   ||
           vk == VK_LMENU    || vk == VK_RMENU     ||
           vk == VK_LWIN     || vk == VK_RWIN;
}

static BOOL is_mouse_vk(WORD vk) {
    return vk == VK_LBUTTON || vk == VK_RBUTTON || vk == VK_MBUTTON;
}

/* Lookup a key name, returning VK code (0 if not found) */
WORD lookup_key(const char *name) {
    for (const KeyEntry *e = KEY_TABLE; e->name; e++) {
        if (_stricmp(name, e->name) == 0)
            return e->vk;
    }
    /* Single letter a-z */
    if (strlen(name) == 1 && isalpha((unsigned char)name[0]))
        return (WORD)toupper((unsigned char)name[0]);
    /* Single digit 0-9 */
    if (strlen(name) == 1 && isdigit((unsigned char)name[0]))
        return (WORD)name[0];
    return 0;
}

/* ── Parse a single key combo (e.g., "ctrl+shift+tab") into KeyCombo ── */

static int parse_combo(const char *str, KeyCombo *out) {
    memset(out, 0, sizeof(*out));

    char buf[256];
    strncpy(buf, str, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';

    WORD keys[MAX_MODIFIERS + 1];
    BOOL mouseFlags[MAX_MODIFIERS + 1];
    int numKeys = 0;

    char *saveptr = NULL;
    char *token = strtok_r(buf, "+", &saveptr);
    while (token && numKeys < MAX_MODIFIERS + 1) {
        /* Trim whitespace */
        while (*token == ' ') token++;
        char *end = token + strlen(token) - 1;
        while (end > token && *end == ' ') *end-- = '\0';

        WORD vk = lookup_key(token);
        if (vk == 0) {
            fprintf(stderr, "config: unknown key '%s'\n", token);
            return -1;
        }
        keys[numKeys] = vk;
        mouseFlags[numKeys] = is_mouse_vk(vk);
        numKeys++;
        token = strtok_r(NULL, "+", &saveptr);
    }

    if (numKeys == 0)
        return -1;

    /* Last key is the main key */
    out->key = keys[numKeys - 1];
    out->isMouse = mouseFlags[numKeys - 1];
    for (int i = 0; i < numKeys - 1; i++) {
        if (out->numModifiers < MAX_MODIFIERS)
            out->modifiers[out->numModifiers++] = keys[i];
    }

    /* If the only key IS a modifier, treat it as main key */
    if (numKeys == 1 && is_modifier(keys[0])) {
        out->key = keys[0];
        out->numModifiers = 0;
    }

    return 0;
}

/* ── Parse action string ────────────────────────────────────────── */
/* Supports: "ctrl+shift+tab", "ctrl+k,ctrl+c", "run:notepad", "task_view" */

static int parse_action(const char *str, KeyAction *out) {
    memset(out, 0, sizeof(*out));

    if (!str || !*str)
        return -1;

    /* Save original config string for UI display */
    strncpy(out->configStr, str, MAX_ACTION_STR - 1);

    /* "run:" prefix */
    if (_strnicmp(str, "run:", 4) == 0) {
        out->type = ACTION_RUN;
        strncpy(out->runTarget, str + 4, MAX_RUN_TARGET - 1);
        return 0;
    }

    /* Check if it's a builtin action ID */
    const char *resolved = action_lookup(str);
    if (resolved) {
        /* Recursively parse the resolved key string */
        /* But keep the original configStr */
        char saved[MAX_ACTION_STR];
        strncpy(saved, out->configStr, MAX_ACTION_STR);
        int rc = parse_action(resolved, out);
        strncpy(out->configStr, saved, MAX_ACTION_STR);
        return rc;
    }

    out->type = ACTION_KEYS;

    /* Split on ',' for multi-step sequences */
    char buf[MAX_ACTION_STR];
    strncpy(buf, str, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';

    char *saveptr = NULL;
    char *step = strtok_r(buf, ",", &saveptr);
    while (step && out->numSteps < MAX_COMBO_STEPS) {
        /* Trim whitespace */
        while (*step == ' ') step++;
        char *end = step + strlen(step) - 1;
        while (end > step && *end == ' ') *end-- = '\0';

        if (parse_combo(step, &out->steps[out->numSteps]) == 0)
            out->numSteps++;
        else
            return -1;

        step = strtok_r(NULL, ",", &saveptr);
    }

    return (out->numSteps > 0) ? 0 : -1;
}

/* ── Load JSON config ───────────────────────────────────────────── */

int config_load(GestureConfig *cfg, const char *path) {
    /* Defaults */
    cfg->gesture_threshold    = 30;
    cfg->dead_zone            = 15;
    cfg->scroll_enabled       = TRUE;
    cfg->scroll_multiplier    = 3;
    cfg->reverse_scroll       = FALSE;
    cfg->post_release_lock_ms = 150;
    cfg->trigger_vk           = VK_F24;
    cfg->launch_on_startup    = FALSE;
    cfg->numApps              = 0;
    memset(&cfg->tap_action, 0, sizeof(cfg->tap_action));
    memset(cfg->defaults, 0, sizeof(cfg->defaults));
    memset(cfg->apps, 0, sizeof(cfg->apps));

    /* Default tap action: ctrl+lclick */
    parse_action("ctrl+lclick", &cfg->tap_action);

    /* Read file */
    FILE *f = fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "config: cannot open %s\n", path);
        return -1;
    }
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    if (len < 0) { fclose(f); return -1; }
    fseek(f, 0, SEEK_SET);
    char *data = (char *)malloc((size_t)len + 1);
    if (!data) { fclose(f); return -1; }
    size_t nread = fread(data, 1, (size_t)len, f);
    data[nread] = '\0';
    fclose(f);

    cJSON *root = cJSON_Parse(data);
    free(data);
    if (!root) {
        fprintf(stderr, "config: JSON parse error near: %.40s\n",
                cJSON_GetErrorPtr() ? cJSON_GetErrorPtr() : "unknown");
        return -1;
    }

    /* Settings */
    cJSON *settings = cJSON_GetObjectItem(root, "settings");
    if (settings) {
        cJSON *v;
        if ((v = cJSON_GetObjectItem(settings, "gesture_threshold")) && cJSON_IsNumber(v))
            cfg->gesture_threshold = v->valueint;
        if ((v = cJSON_GetObjectItem(settings, "dead_zone")) && cJSON_IsNumber(v))
            cfg->dead_zone = v->valueint;
        if ((v = cJSON_GetObjectItem(settings, "scroll_enabled")) && cJSON_IsBool(v))
            cfg->scroll_enabled = cJSON_IsTrue(v);
        if ((v = cJSON_GetObjectItem(settings, "scroll_multiplier")) && cJSON_IsNumber(v))
            cfg->scroll_multiplier = v->valueint;
        if ((v = cJSON_GetObjectItem(settings, "reverse_scroll")) && cJSON_IsBool(v))
            cfg->reverse_scroll = cJSON_IsTrue(v);
        if ((v = cJSON_GetObjectItem(settings, "post_release_lock_ms")) && cJSON_IsNumber(v))
            cfg->post_release_lock_ms = v->valueint;
        if ((v = cJSON_GetObjectItem(settings, "trigger_key")) && cJSON_IsString(v)) {
            WORD vk = lookup_key(v->valuestring);
            if (vk) cfg->trigger_vk = vk;
        }
        if ((v = cJSON_GetObjectItem(settings, "tap_action")) && cJSON_IsString(v))
            parse_action(v->valuestring, &cfg->tap_action);
        if ((v = cJSON_GetObjectItem(settings, "launch_on_startup")) && cJSON_IsBool(v))
            cfg->launch_on_startup = cJSON_IsTrue(v);
    }

    /* Clamp settings to sane ranges */
    if (cfg->gesture_threshold < 1)   cfg->gesture_threshold = 1;
    if (cfg->gesture_threshold > 500) cfg->gesture_threshold = 500;
    if (cfg->dead_zone < 0)           cfg->dead_zone = 0;
    if (cfg->dead_zone > 200)         cfg->dead_zone = 200;
    if (cfg->scroll_multiplier < 1)   cfg->scroll_multiplier = 1;
    if (cfg->scroll_multiplier > 20)  cfg->scroll_multiplier = 20;
    if (cfg->post_release_lock_ms < 0)    cfg->post_release_lock_ms = 0;
    if (cfg->post_release_lock_ms > 2000) cfg->post_release_lock_ms = 2000;

    /* Default gestures */
    const char *dirs[] = {"up", "down", "left", "right"};
    cJSON *defs = cJSON_GetObjectItem(root, "defaults");
    if (defs) {
        for (int i = 0; i < DIR_COUNT; i++) {
            cJSON *v = cJSON_GetObjectItem(defs, dirs[i]);
            if (v && cJSON_IsString(v))
                parse_action(v->valuestring, &cfg->defaults[i]);
        }
    }

    /* Per-app overrides */
    cJSON *apps = cJSON_GetObjectItem(root, "apps");
    if (apps) {
        cJSON *app = NULL;
        cJSON_ArrayForEach(app, apps) {
            if (cfg->numApps >= MAX_APPS) break;
            AppOverride *ov = &cfg->apps[cfg->numApps];

            /* Copy app name as lowercase */
            strncpy(ov->name, app->string, MAX_APP_NAME - 1);
            for (char *p = ov->name; *p; p++) *p = (char)tolower((unsigned char)*p);

            for (int i = 0; i < DIR_COUNT; i++) {
                cJSON *v = cJSON_GetObjectItem(app, dirs[i]);
                if (v && cJSON_IsString(v)) {
                    if (parse_action(v->valuestring, &ov->actions[i]) == 0)
                        ov->hasAction[i] = TRUE;
                }
            }
            cfg->numApps++;
        }
    }

    cJSON_Delete(root);
    return 0;
}

/* ── Reverse-lookup VK code to key name ────────────────────────── */

const char* vk_to_name(WORD vk) {
    for (const KeyEntry *e = KEY_TABLE; e->name; e++) {
        if (e->vk == vk) return e->name;
    }
    /* Single letter */
    if (vk >= 'A' && vk <= 'Z') {
        static char letter[2];
        letter[0] = (char)tolower((unsigned char)vk);
        letter[1] = '\0';
        return letter;
    }
    /* Single digit */
    if (vk >= '0' && vk <= '9') {
        static char digit[2];
        digit[0] = (char)vk;
        digit[1] = '\0';
        return digit;
    }
    return "?";
}

/* ── Save config to JSON ───────────────────────────────────────── */

int config_save(const GestureConfig *cfg, const char *path) {
    cJSON *root = cJSON_CreateObject();
    if (!root) return -1;

    /* Settings */
    cJSON *settings = cJSON_AddObjectToObject(root, "settings");
    if (settings) {
        const char *trigName = vk_to_name(cfg->trigger_vk);
        cJSON_AddStringToObject(settings, "trigger_key", trigName ? trigName : "f24");
        if (cfg->tap_action.configStr[0])
            cJSON_AddStringToObject(settings, "tap_action", cfg->tap_action.configStr);
        else
            cJSON_AddStringToObject(settings, "tap_action", "ctrl+lclick");
        cJSON_AddNumberToObject(settings, "gesture_threshold", cfg->gesture_threshold);
        cJSON_AddNumberToObject(settings, "dead_zone", cfg->dead_zone);
        cJSON_AddBoolToObject(settings, "scroll_enabled", cfg->scroll_enabled);
        cJSON_AddNumberToObject(settings, "scroll_multiplier", cfg->scroll_multiplier);
        cJSON_AddBoolToObject(settings, "reverse_scroll", cfg->reverse_scroll);
        cJSON_AddNumberToObject(settings, "post_release_lock_ms", cfg->post_release_lock_ms);
        cJSON_AddBoolToObject(settings, "launch_on_startup", cfg->launch_on_startup);
    }

    /* Defaults */
    const char *dirs[] = {"up", "down", "left", "right"};
    cJSON *defs = cJSON_AddObjectToObject(root, "defaults");
    if (defs) {
        for (int i = 0; i < DIR_COUNT; i++) {
            if (cfg->defaults[i].type != ACTION_NONE && cfg->defaults[i].configStr[0])
                cJSON_AddStringToObject(defs, dirs[i], cfg->defaults[i].configStr);
        }
    }

    /* Apps */
    cJSON *apps = cJSON_AddObjectToObject(root, "apps");
    if (apps) {
        for (int a = 0; a < cfg->numApps; a++) {
            const AppOverride *ov = &cfg->apps[a];
            cJSON *app = cJSON_AddObjectToObject(apps, ov->name);
            if (!app) continue;
            for (int i = 0; i < DIR_COUNT; i++) {
                if (ov->hasAction[i] && ov->actions[i].configStr[0])
                    cJSON_AddStringToObject(app, dirs[i], ov->actions[i].configStr);
            }
        }
    }

    char *json = cJSON_Print(root);
    cJSON_Delete(root);
    if (!json) return -1;

    FILE *f = fopen(path, "wb");
    if (!f) { cJSON_free(json); return -1; }
    fputs(json, f);
    fclose(f);
    cJSON_free(json);
    return 0;
}

/* ── Action lookup with fallthrough ─────────────────────────────── */

const KeyAction* config_get_action(const GestureConfig *cfg,
                                   const char *processName, int direction)
{
    if (direction < 0 || direction >= DIR_COUNT)
        return NULL;

    /* Check app overrides */
    if (processName && *processName) {
        for (int i = 0; i < cfg->numApps; i++) {
            if (_stricmp(cfg->apps[i].name, processName) == 0) {
                if (cfg->apps[i].hasAction[direction])
                    return &cfg->apps[i].actions[direction];
                break;
            }
        }
    }

    /* Fall through to defaults */
    if (cfg->defaults[direction].type != ACTION_NONE)
        return &cfg->defaults[direction];

    return NULL;
}
