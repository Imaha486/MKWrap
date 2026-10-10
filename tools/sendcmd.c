#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
static HWND found; static const char *pat;
static int g_nomenu;
static BOOL CALLBACK cb(HWND h, LPARAM l)
{
    char t[256]; DWORD pid = 0; GetWindowTextA(h, t, 256); (void)l;
    GetWindowThreadProcessId(h, &pid);
    if (pid == GetCurrentProcessId() || !IsWindowVisible(h) || !strstr(t, pat)) return TRUE;
    if (g_nomenu ? (t[0] != 0) : (GetMenu(h) != NULL)) { found = h; return FALSE; }
    return TRUE;
}
int main(int argc, char **argv)
{
    /* sendcmd <title-substr|*> cmd <id> | key <vk> | sys <vk> */
    pat = argc > 1 && strcmp(argv[1], "*") ? argv[1] : "";
    EnumWindows(cb, 0);
    if (!found) { g_nomenu = 1; EnumWindows(cb, 0); }
    if (!found) { printf("not found\n"); return 1; }
    if (!strcmp(argv[2], "menu")) {
        HMENU m = GetMenu(found); int i, j, n = GetMenuItemCount(m);
        for (i = 0; i < n; i++) {
            char t[128]; HMENU sub = GetSubMenu(m, i); int k;
            GetMenuStringA(m, i, t, 128, MF_BYPOSITION); printf("%s\n", t);
            k = sub ? GetMenuItemCount(sub) : 0;
            for (j = 0; j < k; j++) { GetMenuStringA(sub, j, t, 128, MF_BYPOSITION); printf("  %u %s\n", GetMenuItemID(sub, j), t); }
        }
        return 0;
    }
    if (!strcmp(argv[2], "cmd")) PostMessageA(found, WM_COMMAND, atoi(argv[3]), 0);
    else if (!strcmp(argv[2], "key")) { int vk = strtol(argv[3], 0, 0); PostMessageA(found, WM_KEYDOWN, vk, 1); Sleep(80); PostMessageA(found, WM_KEYUP, vk, 0xC0000001); }
    else if (!strcmp(argv[2], "sys")) { int vk = strtol(argv[3], 0, 0); PostMessageA(found, WM_SYSKEYDOWN, vk, 1 | (1 << 29)); Sleep(50); PostMessageA(found, WM_SYSKEYUP, vk, 0xE0000001); }
    printf("ok %p\n", (void *)found);
    return 0;
}
