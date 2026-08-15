#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>
#include <shellapi.h>

#include <cstdint>
#include <cwchar>

namespace {

int InjectDll(DWORD pid, const wchar_t* dllPath) {
    if (pid == 0 || dllPath == nullptr || dllPath[0] == L'\0') {
        return 2;
    }

    HANDLE process = OpenProcess(PROCESS_CREATE_THREAD |
                                         PROCESS_QUERY_INFORMATION |
                                         PROCESS_VM_OPERATION |
                                         PROCESS_VM_WRITE |
                                         PROCESS_VM_READ,
                                 FALSE,
                                 pid);
    if (process == nullptr) {
        return 10;
    }

    const SIZE_T bytes = (std::wcslen(dllPath) + 1) * sizeof(wchar_t);
    void* remoteString =
            VirtualAllocEx(process, nullptr, bytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (remoteString == nullptr) {
        CloseHandle(process);
        return 11;
    }

    if (!WriteProcessMemory(process, remoteString, dllPath, bytes, nullptr)) {
        VirtualFreeEx(process, remoteString, 0, MEM_RELEASE);
        CloseHandle(process);
        return 12;
    }

    HMODULE kernel32 = GetModuleHandleW(L"kernel32.dll");
    auto* loadLibrary =
            reinterpret_cast<LPTHREAD_START_ROUTINE>(GetProcAddress(kernel32, "LoadLibraryW"));
    if (loadLibrary == nullptr) {
        VirtualFreeEx(process, remoteString, 0, MEM_RELEASE);
        CloseHandle(process);
        return 13;
    }

    HANDLE thread = CreateRemoteThread(process, nullptr, 0, loadLibrary, remoteString, 0, nullptr);
    if (thread == nullptr) {
        VirtualFreeEx(process, remoteString, 0, MEM_RELEASE);
        CloseHandle(process);
        return 14;
    }

    WaitForSingleObject(thread, INFINITE);
    DWORD remoteResult = 0;
    GetExitCodeThread(thread, &remoteResult);
    CloseHandle(thread);
    VirtualFreeEx(process, remoteString, 0, MEM_RELEASE);
    CloseHandle(process);

    return remoteResult != 0 ? 0 : 15;
}

DWORD ParsePid(const wchar_t* value) {
    if (value == nullptr || value[0] == L'\0') {
        return 0;
    }
    wchar_t* end = nullptr;
    const unsigned long parsed = std::wcstoul(value, &end, 10);
    if (end == value || *end != L'\0') {
        return 0;
    }
    return static_cast<DWORD>(parsed);
}

}  // namespace

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
    int argc = 0;
    wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (argv == nullptr) {
        return 1;
    }

    const int result = argc >= 3 ? InjectDll(ParsePid(argv[1]), argv[2]) : 2;
    LocalFree(argv);
    return result;
}
