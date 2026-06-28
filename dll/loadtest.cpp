#include <windows.h>
#include <cstdio>
int main() {
    SetCurrentDirectoryW(L"J:\\SteamLibrary\\steamapps\\common\\Vagante");
    HMODULE h = LoadLibraryW(L"openal32.dll");
    if (!h) { printf("FAIL LoadLibrary err=%lu\n", GetLastError()); return 1; }
    printf("OK loaded openal32.dll at %p\n", (void*)h);
    // confirm a forwarded export resolves
    FARPROC p = GetProcAddress(h, "alGetString");
    printf("alGetString = %p (forward %s)\n", (void*)p, p ? "resolved" : "MISSING");
    FreeLibrary(h);
    return 0;
}
