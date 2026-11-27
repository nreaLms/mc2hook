#pragma once
#include <Windows.h>
#include <cstdio>
#include <cstdint>
#include <framework/HookConfig.h>

// Diagnostic only. No game code patching or exception recovery.
class AnimationCrashDiagnostics {
    static inline PVOID registration = nullptr;
    static inline volatile LONG busy = 0;
    static bool Read(DWORD address, void* out, SIZE_T size) {
        SIZE_T got = 0;
        return address >= 0x10000 && ReadProcessMemory(GetCurrentProcess(),
            reinterpret_cast<LPCVOID>(static_cast<uintptr_t>(address)), out, size, &got) && got == size;
    }
    static void Line(HANDLE file, const char* text) {
        DWORD written = 0;
        WriteFile(file, text, static_cast<DWORD>(lstrlenA(text)), &written, nullptr);
    }
    static void Word(HANDLE file, const char* name, DWORD address) {
        char line[160]; DWORD value = 0;
        if (Read(address, &value, sizeof(value)))
            sprintf_s(line, "%s [0x%08lX] = 0x%08lX\r\n", name, address, value);
        else sprintf_s(line, "%s [0x%08lX] = unreadable\r\n", name, address);
        Line(file, line);
    }
    static void Inspect(HANDLE file, const char* label, DWORD address) {
        char line[128]; sprintf_s(line, "\r\n%s: 0x%08lX\r\n", label, address); Line(file,line);
        if (address < 0x10000 || address > 0xFFFFFFC0) return;
        for (DWORD i=0; i<0x40; i+=4) Word(file, "word", address+i);
    }
    static LONG CALLBACK OnException(EXCEPTION_POINTERS* p) {
        if (!p || !p->ExceptionRecord || !p->ContextRecord) return EXCEPTION_CONTINUE_SEARCH;
        const auto& e=*p->ExceptionRecord; const auto& c=*p->ContextRecord;
        if (e.ExceptionCode != EXCEPTION_ACCESS_VIOLATION || c.Eip != 0x005DDD3D ||
            reinterpret_cast<uintptr_t>(e.ExceptionAddress)!=0x005DDD3D) return EXCEPTION_CONTINUE_SEARCH;
        if (InterlockedCompareExchange(&busy,1,0)!=0) return EXCEPTION_CONTINUE_SEARCH;
        HANDLE file=CreateFileA("mc2-animation-crash.log", FILE_APPEND_DATA, FILE_SHARE_READ,
            nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file!=INVALID_HANDLE_VALUE) {
            char line[512]; SYSTEMTIME time{}; GetLocalTime(&time);
            sprintf_s(line,"\r\n=== Animation crash %04u-%02u-%02u %02u:%02u:%02u thread=%lu ===\r\n",
                (unsigned)time.wYear,(unsigned)time.wMonth,(unsigned)time.wDay,(unsigned)time.wHour,(unsigned)time.wMinute,(unsigned)time.wSecond,GetCurrentThreadId()); Line(file,line);
            sprintf_s(line,"EIP=%08lX ESP=%08lX EBP=%08lX EFLAGS=%08lX\r\nEAX=%08lX EBX=%08lX ECX=%08lX EDX=%08lX ESI=%08lX EDI=%08lX\r\n",c.Eip,c.Esp,c.Ebp,c.EFlags,c.Eax,c.Ebx,c.Ecx,c.Edx,c.Esi,c.Edi); Line(file,line);
            if(e.NumberParameters>=2){sprintf_s(line,"Access type=%lu address=%08lX\r\n",static_cast<DWORD>(e.ExceptionInformation[0]),static_cast<DWORD>(e.ExceptionInformation[1]));Line(file,line);}
            DWORD inner=0;
            if(Read(c.Esp+0x20,&inner,4) && inner==0x005DDE49) {
                Line(file,"Verified inner return: 0x005DDE49; decoded 0x005DDE10 frame\r\n");
                Word(file,"saved EDI",c.Esp+0x24); Word(file,"saved ESI",c.Esp+0x28); Word(file,"saved EBX",c.Esp+0x2C); Word(file,"saved EBP",c.Esp+0x30);
                Word(file,"entry ECX",c.Esp+0x34); Word(file,"outer return",c.Esp+0x38); Word(file,"argument 1",c.Esp+0x3C); Word(file,"argument 2",c.Esp+0x40);
                DWORD object=0,arg=0,source=0;
                if(Read(c.Esp+0x34,&object,4)) Inspect(file,"entry ECX object",object);
                if(Read(c.Esp+0x3C,&arg,4)) { Inspect(file,"argument 1 object",arg); if(arg>=0x10000 && arg<=0xFFFFFFF8 && Read(arg+4,&source,4)) Inspect(file,"argument 1 + 4 pointee",source); }
            } else Line(file,"Inner return differs or unreadable; frame not decoded.\r\n");
            Inspect(file,"destination EDX",c.Edx); Inspect(file,"source ESI",c.Esi);
            Line(file,"\r\nRaw stack (not a verified backtrace):\r\n");
            for(DWORD i=0;i<0x100;i+=4) Word(file,"stack",c.Esp+i);
            FlushFileBuffers(file); CloseHandle(file);
        }
        InterlockedExchange(&busy,0);
        return EXCEPTION_CONTINUE_SEARCH;
    }
public:
    static void Install() {
        if(registration || !HookConfig::GetBool("AnimationCrashDiagnostics","Enable",true)) return;
        // Both supplied executables match this instruction sequence.
        const unsigned char expected[]={0x83,0xEC,0x18,0x8D,0x44,0x24,0x04,0x8D,0x4C,0x24,0x00,0x50,0x51,0x66,0x8B,0x0E};
        unsigned char actual[sizeof(expected)]{};
        if(!Read(0x005DDD30,actual,sizeof(actual))) return;
        for(unsigned i=0;i<sizeof(expected);++i) if(actual[i]!=expected[i]) return;
        registration=AddVectoredExceptionHandler(1,&OnException);
    }
};
