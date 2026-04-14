#ifndef SETTINGS_UI_H
#define SETTINGS_UI_H

#include <windows.h>

/* Show the settings dialog. Blocks until closed.
 * Reads config from configPath, writes back on OK/Apply.
 * Posts (WM_USER+5) to parent to trigger config reload on Apply/OK.
 * parent: owner window (engine's hidden HWND).
 */
void ShowSettingsDialog(HWND parent, const char *configPath);

#endif /* SETTINGS_UI_H */
