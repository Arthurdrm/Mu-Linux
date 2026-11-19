#include <cstdint>
#pragma once
#include "Platform/IDiagnostics.h"
#include "Platform/IWindow.h"
#include "Debug/MiniDump.h"
#include <windows.h>

// SEH: cada paso del filtro de crash va protegido para que un fallo al
// loguear no tape el volcado. GCC (MinGW) no tiene __try/__except: ahí los
// pasos corren sin protección, igual que en Linux.
#if defined(_MSC_VER)
#define MU_SEH_TRY __try
#define MU_SEH_EXCEPT __except (EXCEPTION_EXECUTE_HANDLER)
#else
#define MU_SEH_TRY if (true)
#define MU_SEH_EXCEPT else
#endif
#include <cstdio>
#include <intrin.h>
#include "Platform/ITimer.h"
#include <exception>
#include <typeinfo>
namespace Platform {
namespace Win32DiagnosticsDetail {
inline DiagnosticLog logger = nullptr;
inline void CrashLog(const char* text) { if (logger) logger(text); }

inline void FormatAddressRVA(char* out, size_t outSize, uintptr_t addr)
{
    HMODULE hMod = NULL;
    if (addr && GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                  GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                  (LPCSTR)addr, &hMod) && hMod != NULL) {
        char modPath[MAX_PATH] = {};
        GetModuleFileNameA(hMod, modPath, MAX_PATH);
        const char* baseName = strrchr(modPath, '\\');
        if (!baseName) baseName = strrchr(modPath, '/');
        baseName = baseName ? baseName + 1 : modPath;
        uintptr_t rva = addr - (uintptr_t)hMod;
        _snprintf_s(out, outSize, _TRUNCATE, "%s+0x%llX (0x%p)", baseName, (unsigned long long)rva, (void*)addr);
    } else {
        _snprintf_s(out, outSize, _TRUNCATE, "0x%p", (void*)addr);
    }
}

inline LONG WINAPI DbgUnhandledException(EXCEPTION_POINTERS* ep)
{
    // Anti-recursion guard: Si DbgLog crashea (e.g., WriteFile AV),
    // el filter se reentra → recursion infinita → otra AV en KernelBase.
    static __declspec(thread) int s_handlerDepth = 0;
    if (s_handlerDepth > 0) return EXCEPTION_CONTINUE_SEARCH;
    s_handlerDepth++;

    // NOTA VEH vs SetUnhandledExceptionFilter:
    // Se este handler fosse registrado via AddVectoredExceptionHandler (VEH), ele
    // capturaria exceções em first-chance ANTES de qualquer bloco try/catch C++,
    // abortando o client indevidamente em exceções normais tratadas (0xE06D7363).
    // O client utiliza SetUnhandledExceptionFilter, sendo chamado apenas para
    // exceções de fato não tratadas. Caso seja invocado por VEH, deve retornar
    // EXCEPTION_CONTINUE_SEARCH em 0xE06D7363 para permitir o catch.

    // El .dmp se escribe PRIMERO, antes de loguear nada.
    char dumpPath[MAX_PATH] = {};
    bool dumpOk = false;
    MU_SEH_TRY {
        dumpOk = CMiniDump::Write(ep, dumpPath, sizeof(dumpPath));
    } MU_SEH_EXCEPT { dumpOk = false; }

    char line1[256], line2[256], faultRva[256], full[800];
    char cppExceptionWhat[512] = {};
    char cppExceptionType[128] = {};
    faultRva[0] = 0;

    MU_SEH_TRY {
        FormatAddressRVA(faultRva, sizeof(faultRva), (uintptr_t)ep->ExceptionRecord->ExceptionAddress);
        _snprintf_s(line1, sizeof(line1), _TRUNCATE,
            "CRASH: code=0x%08X  addr=%s",
            ep->ExceptionRecord->ExceptionCode,
            faultRva);
    } MU_SEH_EXCEPT {
        line1[0] = 0;
    }

    line2[0] = 0;
    MU_SEH_TRY {
        if (ep && ep->ExceptionRecord && ep->ExceptionRecord->NumberParameters >= 2) {
            _snprintf_s(line2, sizeof(line2), _TRUNCATE,
                "param0=%u  param1=0x%p",
                (unsigned)ep->ExceptionRecord->ExceptionInformation[0],
                (void*)ep->ExceptionRecord->ExceptionInformation[1]);
        }
    } MU_SEH_EXCEPT { line2[0] = 0; }

    // ── Extração de std::exception::what() para MSVC C++ Exception (0xE06D7363) ──
    if (ep && ep->ExceptionRecord && ep->ExceptionRecord->ExceptionCode == 0xE06D7363) {
        if (ep->ExceptionRecord->NumberParameters >= 2) {
            void* obj = (void*)ep->ExceptionRecord->ExceptionInformation[1];
            if (obj) {
                MU_SEH_TRY {
                    const std::exception* ex = reinterpret_cast<const std::exception*>(obj);
                    const char* w = ex->what();
                    if (w) {
                        int j = 0;
                        for (; j < (int)sizeof(cppExceptionWhat) - 1 && w[j] != 0; ++j) {
                            char c = w[j];
                            cppExceptionWhat[j] = (c >= 32 && c <= 126) ? c : ' ';
                        }
                        cppExceptionWhat[j] = 0;
                    }
                } MU_SEH_EXCEPT {}
            }
        }
#if !defined(_M_X64)
        if (ep->ExceptionRecord->NumberParameters >= 3) {
            MU_SEH_TRY {
                void* pThrowInfo = (void*)ep->ExceptionRecord->ExceptionInformation[2];
                if (pThrowInfo) {
                    struct Win32CatchableTypeArray { int count; void* arrayOfTypes[1]; };
                    struct Win32ThrowInfo {
                        unsigned int attributes;
                        void* pmfnUnwind;
                        void* pForwardCompat;
                        Win32CatchableTypeArray* pCatchableTypeArray;
                    };
                    struct Win32CatchableType { unsigned int properties; void* pTypeDescriptor; };
                    struct Win32TypeDescriptor { const void* pVFTable; void* spare; char name[1]; };
                    Win32ThrowInfo* ti = (Win32ThrowInfo*)pThrowInfo;
                    if (ti->pCatchableTypeArray && ti->pCatchableTypeArray->count > 0) {
                        Win32CatchableType* ct = (Win32CatchableType*)ti->pCatchableTypeArray->arrayOfTypes[0];
                        if (ct && ct->pTypeDescriptor) {
                            Win32TypeDescriptor* td = (Win32TypeDescriptor*)ct->pTypeDescriptor;
                            if (td->name[0]) {
                                _snprintf_s(cppExceptionType, sizeof(cppExceptionType), _TRUNCATE, "%s", td->name);
                            }
                        }
                    }
                }
            } MU_SEH_EXCEPT {}
        }
#endif
    }

    MU_SEH_TRY { CrashLog(line1); } MU_SEH_EXCEPT {}
    if (line2[0]) MU_SEH_TRY { CrashLog(line2); } MU_SEH_EXCEPT {}
    if (cppExceptionType[0]) {
        char typeLog[256];
        _snprintf_s(typeLog, sizeof(typeLog), _TRUNCATE, "C++ EXCEPTION TYPE: %s", cppExceptionType);
        MU_SEH_TRY { CrashLog(typeLog); } MU_SEH_EXCEPT {}
    }
    if (cppExceptionWhat[0]) {
        char whatLog[600];
        _snprintf_s(whatLog, sizeof(whatLog), _TRUNCATE, "C++ EXCEPTION WHAT: %s", cppExceptionWhat);
        MU_SEH_TRY { CrashLog(whatLog); } MU_SEH_EXCEPT {}
    }

    // ── Gravação dedicada em crash.log ─────────────────────────────────────────
    FILE* fpCrash = nullptr;
    MU_SEH_TRY {
        fopen_s(&fpCrash, "crash.log", "a+");
        if (fpCrash) {
            SYSTEMTIME st;
            GetLocalTime(&st);
            fprintf(fpCrash, "==================== CRASH REPORT ====================\n");
            fprintf(fpCrash, "Timestamp: %04d-%02d-%02d %02d:%02d:%02d.%03d\n",
                    st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
            fprintf(fpCrash, "%s\n", line1);
            if (line2[0]) fprintf(fpCrash, "%s\n", line2);
            if (cppExceptionType[0]) fprintf(fpCrash, "Type: %s\n", cppExceptionType);
            if (cppExceptionWhat[0]) fprintf(fpCrash, "what(): %s\n", cppExceptionWhat);
            if (dumpOk) fprintf(fpCrash, "Minidump: %s\n", dumpPath);
        }
    } MU_SEH_EXCEPT { fpCrash = nullptr; }

    // ── Stack trace desde el contexto del crash ──────────────────────────────
    MU_SEH_TRY {
#if defined(_M_X64)
        if (ep && ep->ContextRecord) {
            char trace[256];
            char ripRva[128];
            FormatAddressRVA(ripRva, sizeof(ripRva), (uintptr_t)ep->ContextRecord->Rip);
            _snprintf_s(trace, sizeof(trace), _TRUNCATE, "STACK_PC: %s (rsp=0x%p rbp=0x%p)",
                ripRva, (void*)ep->ContextRecord->Rsp, (void*)ep->ContextRecord->Rbp);
            CrashLog(trace);
            if (fpCrash) fprintf(fpCrash, "%s\n", trace);
            char scan[900];
            int used = _snprintf_s(scan, sizeof(scan), _TRUNCATE, "STACK_SCAN:");
            const DWORD64 image = (DWORD64)GetModuleHandleA(nullptr);
            const DWORD64* stack = (const DWORD64*)ep->ContextRecord->Rsp;
            for (int i = 0, found = 0; i < 400 && found < 24; ++i) {
                MU_SEH_TRY {
                    const DWORD64 value = stack[i];
                    if (value > image + 0x1000 && value < image + 0x2000000) {
                        used += _snprintf_s(scan + used, sizeof(scan) - used, _TRUNCATE, " %llX", value - image);
                        ++found;
                    }
                } MU_SEH_EXCEPT { break; }
            }
            CrashLog(scan);
            if (fpCrash) fprintf(fpCrash, "%s\n", scan);
        }
#else
        if (ep && ep->ContextRecord) {
            CONTEXT* ctx = ep->ContextRecord;
            char trace[800];
            char eipRva[128];
            FormatAddressRVA(eipRva, sizeof(eipRva), (uintptr_t)ctx->Eip);
            int n = 0;
            n += _snprintf_s(trace + n, sizeof(trace) - n, _TRUNCATE,
                "STACK_PC: %s (esp=0x%p ebp=0x%p)",
                eipRva, (void*)ctx->Esp, (void*)ctx->Ebp);
            CrashLog(trace);
            if (fpCrash) fprintf(fpCrash, "%s\n", trace);

            // Volcado de registros — los valores de los argumentos suelen ir en eax/ecx/edx
            _snprintf_s(trace, sizeof(trace), _TRUNCATE,
                "REGS: eax=%08X ecx=%08X edx=%08X ebx=%08X esi=%08X edi=%08X",
                (unsigned)ctx->Eax, (unsigned)ctx->Ecx, (unsigned)ctx->Edx,
                (unsigned)ctx->Ebx, (unsigned)ctx->Esi, (unsigned)ctx->Edi);
            CrashLog(trace);
            if (fpCrash) fprintf(fpCrash, "%s\n", trace);

            // ── Recorrido de frames por EBP (direcciones de retorno reales) ─────
            // Por cada frame: prev_ebp = *ebp, return_addr = *(ebp+4), then ebp = prev_ebp.
            n = _snprintf_s(trace, sizeof(trace), _TRUNCATE, "STACK_FRAMES:");
            if (fpCrash) fprintf(fpCrash, "STACK_FRAMES:\n");
            DWORD ebp = ctx->Ebp;
            DWORD prev_ebp_value = 0;
            for (int frame = 0; frame < 16 && n < (int)sizeof(trace) - 32; ++frame) {
                MU_SEH_TRY {
                    if (ebp < 0x10000 || ebp > 0x7FFFFFFF) break;
                    if (ebp & 3) break; // misaligned
                    DWORD ret = *(DWORD*)(ebp + 4);
                    DWORD next = *(DWORD*)(ebp);
                    if (next != 0 && next <= ebp) break; // sanity: ebp must grow upward
                    char retRva[128];
                    FormatAddressRVA(retRva, sizeof(retRva), (uintptr_t)ret);
                    n += _snprintf_s(trace + n, sizeof(trace) - n, _TRUNCATE, " %s", retRva);
                    if (fpCrash) fprintf(fpCrash, "  [%02d] %s\n", frame, retRva);
                    prev_ebp_value = ebp;
                    ebp = next;
                    if (ebp == prev_ebp_value) break;
                } MU_SEH_EXCEPT { break; }
            }
            CrashLog(trace);

            // ── Escaneo crudo del stack (250 dwords desde esp): atrapa frames FPO/sin-EBP ──
            int dumped = 0;
            n = _snprintf_s(trace, sizeof(trace), _TRUNCATE, "STACK_SCAN:");
            if (fpCrash) fprintf(fpCrash, "STACK_SCAN:\n");
            DWORD esp = ctx->Esp;
            for (int i = 0; i < 250 && n < (int)sizeof(trace) - 32 && dumped < 25; ++i) {
                MU_SEH_TRY {
                    DWORD val = *(DWORD*)(esp + i * 4);
                    if (val >= 0x401000 && val < 0x7FFFFFFF) {
                        char candRva[128];
                        FormatAddressRVA(candRva, sizeof(candRva), (uintptr_t)val);
                        n += _snprintf_s(trace + n, sizeof(trace) - n, _TRUNCATE, " [%X]=%s", esp + i * 4, candRva);
                        if (fpCrash) fprintf(fpCrash, "  esp+0x%03X (%X) -> %s\n", i * 4, esp + i * 4, candRva);
                        dumped++;
                    }
                } MU_SEH_EXCEPT { break; }
            }
            CrashLog(trace);
        }
#endif
    } MU_SEH_EXCEPT {}

    if (fpCrash) {
        MU_SEH_TRY {
            fprintf(fpCrash, "======================================================\n\n");
            fflush(fpCrash);
            fclose(fpCrash);
        } MU_SEH_EXCEPT {}
    }

    MU_SEH_TRY {
        if (dumpOk) {
            char b[MAX_PATH + 32];
            _snprintf_s(b, sizeof(b), _TRUNCATE, "MINIDUMP: %s", dumpPath);
            CrashLog(b);
        } else {
            CrashLog("MINIDUMP: no se pudo escribir (dbghelp.dll?)");
        }
    } MU_SEH_EXCEPT {}

    // Antes del cartel: si estabamos en pantalla completa hay que devolverle el
    // escritorio al usuario.
    MU_SEH_TRY { Platform::GetWindow().RestoreDisplay(); } MU_SEH_EXCEPT {}

    MU_SEH_TRY {
        char exDesc[512] = {};
        if (cppExceptionWhat[0]) {
            _snprintf_s(exDesc, sizeof(exDesc), _TRUNCATE, "\nExcecao C++: %s", cppExceptionWhat);
        } else if (cppExceptionType[0]) {
            _snprintf_s(exDesc, sizeof(exDesc), _TRUNCATE, "\nTipo C++: %s", cppExceptionType);
        }

        if (dumpOk) {
            _snprintf_s(full, sizeof(full), _TRUNCATE,
                        "%s\n%s%s\n\nVolcado guardado en:\n%s\nDetalhes em: crash.log",
                        line1, line2, exDesc, dumpPath);
        } else {
            _snprintf_s(full, sizeof(full), _TRUNCATE,
                        "%s\n%s%s\n\nDetalhes gravados em crash.log",
                        line1, line2, exDesc);
        }
        Platform::GetWindow().ShowError(full, "Crash", false, true);
    } MU_SEH_EXCEPT {}

    s_handlerDepth--;
    return EXCEPTION_EXECUTE_HANDLER;
}

}
class Win32Diagnostics final : public IDiagnostics {
public:
    std::int64_t MeasureClockRate(std::uint32_t param_1) override {
    // CPUID(1) — check TSC support (EDX bit 4)
    int regs[4] = {0,0,0,0};
    __cpuid(regs, 1);
    if ((regs[3] & 0x10) == 0) return 0;  // no TSC

    LARGE_INTEGER freq;
    if (!QueryPerformanceFrequency(&freq)) return 0;

    HANDLE hProc = GetCurrentProcess();
    HANDLE hThread = GetCurrentThread();
    DWORD priClass = GetPriorityClass(hProc);
    int   threadPri = GetThreadPriority(hThread);
    DWORD_PTR procAff = 0, sysAff = 0;
    GetProcessAffinityMask(hProc, &procAff, &sysAff);

    SetPriorityClass(hProc, REALTIME_PRIORITY_CLASS);
    SetThreadPriority(hThread, THREAD_PRIORITY_TIME_CRITICAL);
    SetProcessAffinityMask(hProc, 1);

    // cpuid as serializing barrier before RDTSC
    __cpuid(regs, 0);
    LARGE_INTEGER qpcStart, qpcEnd;
    QueryPerformanceCounter(&qpcStart);
    std::uint64_t tscStart = __rdtsc();
    Platform::GetTimer().SleepMilliseconds(param_1);
    QueryPerformanceCounter(&qpcEnd);
    std::uint64_t tscEnd = __rdtsc();

    SetProcessAffinityMask(hProc, procAff);
    SetThreadPriority(hThread, threadPri);
    SetPriorityClass(hProc, priClass);

    double tscDelta = (double)(std::int64_t)(tscEnd - tscStart);
    double qpcDelta = (double)(qpcEnd.QuadPart - qpcStart.QuadPart);
    double seconds  = qpcDelta / (double)freq.QuadPart;
    if (seconds <= 0.0) return 0;
    return (long long)(tscDelta / seconds);
    }

    bool Install(DiagnosticLog log) override {
        Win32DiagnosticsDetail::logger = log;
        SetErrorMode(SEM_FAILCRITICALERRORS);
        SetUnhandledExceptionFilter(Win32DiagnosticsDetail::DbgUnhandledException);
        CMiniDump::Preload();
        return true;
    }
};
}
