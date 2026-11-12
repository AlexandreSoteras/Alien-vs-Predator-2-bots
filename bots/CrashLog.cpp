// CrashLog: a tiny crash reporter for the AvP2 server game DLL (object.lto).
//
// Installed before any other static initializer of the DLL (init_seg(lib)), it registers a vectored
// exception handler that appends to "avp2bots_crash.txt" in the server's working directory:
// the exception code and address as module+offset, the registers, an EBP-chain walk and a raw scan
// of the stack for return addresses inside loaded modules. Resolve object.lto offsets with
// navgen/resolve_crash.py and server/out/object.map.
//
// It also writes one "loaded" line when the DLL initialises, which proves our code ran at all.

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#pragma warning(disable : 4073)  // initializers put in library initialization area
#pragma init_seg(lib)

namespace {

const char* kLogName = "avp2bots_crash.txt";
volatile LONG g_reports = 0;

void Describe(FILE* f, DWORD addr)
{
    HMODULE mod = 0;
    char path[MAX_PATH] = "?";
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           (LPCSTR)(ULONG_PTR)addr, &mod) && mod)
    {
        GetModuleFileNameA(mod, path, sizeof(path));
        const char* base = strrchr(path, '\\');
        fprintf(f, "%08lX  %s+0x%lX (base %08lX)\n", addr, base ? base + 1 : path,
                (unsigned long)(addr - (DWORD)(ULONG_PTR)mod), (unsigned long)(ULONG_PTR)mod);
    }
    else
    {
        fprintf(f, "%08lX  (no module)\n", addr);
    }
}

bool InModule(DWORD addr)
{
    HMODULE mod = 0;
    return addr > 0x10000 &&
           GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                              (LPCSTR)(ULONG_PTR)addr, &mod) && mod;
}

bool Readable(const void* p, size_t n)
{
    MEMORY_BASIC_INFORMATION mbi;
    if (!VirtualQuery(p, &mbi, sizeof(mbi)) || mbi.State != MEM_COMMIT)
        return false;
    if (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD))
        return false;
    return (const char*)p + n <= (const char*)mbi.BaseAddress + mbi.RegionSize;
}

LONG CALLBACK Handler(PEXCEPTION_POINTERS info)
{
    DWORD code = info->ExceptionRecord->ExceptionCode;
    // only real faults; C++ exceptions, breakpoints, debug strings and thread naming are normal traffic
    if (code != EXCEPTION_ACCESS_VIOLATION && code != EXCEPTION_ILLEGAL_INSTRUCTION &&
        code != EXCEPTION_INT_DIVIDE_BY_ZERO && code != EXCEPTION_STACK_OVERFLOW &&
        code != EXCEPTION_PRIV_INSTRUCTION && code != EXCEPTION_ARRAY_BOUNDS_EXCEEDED)
        return EXCEPTION_CONTINUE_SEARCH;
    if (InterlockedIncrement(&g_reports) > 5)
        return EXCEPTION_CONTINUE_SEARCH;

    FILE* f = fopen(kLogName, "a");
    if (!f)
        return EXCEPTION_CONTINUE_SEARCH;
    CONTEXT* c = info->ContextRecord;
    time_t now = time(0);
    fprintf(f, "\n=== exception %08lX at %s", code, ctime(&now));
    fprintf(f, "fault  ");
    Describe(f, (DWORD)(ULONG_PTR)info->ExceptionRecord->ExceptionAddress);
    if (code == EXCEPTION_ACCESS_VIOLATION && info->ExceptionRecord->NumberParameters >= 2)
        fprintf(f, "access %s %08lX\n", info->ExceptionRecord->ExceptionInformation[0] ? "write" : "read",
                (unsigned long)info->ExceptionRecord->ExceptionInformation[1]);
    fprintf(f, "EAX=%08lX EBX=%08lX ECX=%08lX EDX=%08lX ESI=%08lX EDI=%08lX\nEBP=%08lX ESP=%08lX EIP=%08lX\n",
            c->Eax, c->Ebx, c->Ecx, c->Edx, c->Esi, c->Edi, c->Ebp, c->Esp, c->Eip);

    fprintf(f, "-- ebp chain\n");
    DWORD ebp = c->Ebp;
    for (int i = 0; i < 32 && ebp && Readable((void*)(ULONG_PTR)ebp, 8); ++i)
    {
        DWORD* frame = (DWORD*)(ULONG_PTR)ebp;
        DWORD ret = frame[1];
        if (!InModule(ret))
            break;
        Describe(f, ret);
        if (frame[0] <= ebp)
            break;
        ebp = frame[0];
    }

    fprintf(f, "-- stack scan (values that point into modules)\n");
    DWORD* sp = (DWORD*)(ULONG_PTR)c->Esp;
    int shown = 0;
    for (int i = 0; i < 1024 && shown < 40; ++i)
    {
        if (!Readable(sp + i, 4))
            break;
        DWORD v = sp[i];
        if (InModule(v))
        {
            fprintf(f, "[esp+%03X] ", i * 4);
            Describe(f, v);
            ++shown;
        }
    }
    fclose(f);
    return EXCEPTION_CONTINUE_SEARCH;
}

// The CRT's invalid-parameter check (e.g. fgets on a NULL FILE*) ends the process with a fast fail that
// no exception handler sees. Log it with a backtrace, then let the CRT function return its error code
// instead of killing the server.
void __cdecl InvalidParameter(const wchar_t*, const wchar_t*, const wchar_t*, unsigned int, uintptr_t)
{
    if (InterlockedIncrement(&g_reports) > 20)
        return;
    FILE* f = fopen(kLogName, "a");
    if (!f)
        return;
    time_t now = time(0);
    fprintf(f, "\n=== invalid parameter (CRT) at %s-- backtrace\n", ctime(&now));
    void* frames[32];
    USHORT n = CaptureStackBackTrace(0, 32, frames, 0);
    for (USHORT i = 0; i < n; ++i)
        Describe(f, (DWORD)(ULONG_PTR)frames[i]);
    fclose(f);
}

struct Installer
{
    Installer()
    {
        AddVectoredExceptionHandler(1, Handler);
        _set_invalid_parameter_handler(InvalidParameter);
        FILE* f = fopen(kLogName, "a");
        if (f)
        {
            time_t now = time(0);
            HMODULE self = 0;
            GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                               (LPCSTR)&kLogName, &self);
            fprintf(f, "object.lto (avp2bots build) loaded at %08lX, %s", (unsigned long)(ULONG_PTR)self, ctime(&now));
            fclose(f);
        }
    }
};

Installer g_installer;

} // namespace
