#ifndef ACTIONS_H
#define ACTIONS_H

/* Built-in action definition */
typedef struct {
    const char *id;          /* config string: "task_view" */
    const char *displayName; /* UI label: "Task View" */
    const char *category;    /* group: "Windows" */
    const char *keys;        /* key string: "win+tab" */
} BuiltinAction;

/* Returns the key string for a builtin action ID, or NULL if not found */
const char* action_lookup(const char *id);

/* Get the full builtin action table. Sets *count to number of entries. */
const BuiltinAction* action_get_all(int *count);

/* Category list (NULL-terminated) */
extern const char *ACTION_CATEGORIES[];

#endif /* ACTIONS_H */
