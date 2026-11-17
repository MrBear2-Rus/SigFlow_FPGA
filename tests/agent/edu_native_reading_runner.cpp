#include <windows.h>

#include <cstdio>
#include <cstdlib>
#include <string>

int wmain(int argc, wchar_t** argv)
{
    if (argc != 2) {
        std::fputs("Expected the native GUI test executable path.\n", stderr);
        return EXIT_FAILURE;
    }

    // The loader can fail before the GUI executable reaches main().
    // Its process inherits this error mode from the test runner.
    SetErrorMode(GetErrorMode() | SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);

    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    startup.hStdOutput = GetStdHandle(STD_OUTPUT_HANDLE);
    startup.hStdError = GetStdHandle(STD_ERROR_HANDLE);
    PROCESS_INFORMATION process{};
    std::wstring command = L"\"" + std::wstring(argv[1]) + L"\"";
    if (!CreateProcessW(argv[1], command.data(), nullptr, nullptr, TRUE, 0,
                        nullptr, nullptr, &startup, &process)) {
        std::fprintf(stderr, "Could not launch the native GUI test (Win32 error %lu).\n",
                     GetLastError());
        return EXIT_FAILURE;
    }
    CloseHandle(process.hThread);

    constexpr DWORD testTimeoutMs = 25000;
    const DWORD wait = WaitForSingleObject(process.hProcess, testTimeoutMs);
    if (wait != WAIT_OBJECT_0) {
        if (wait == WAIT_TIMEOUT) {
            std::fputs("Native GUI test timed out after 25 seconds.\n", stderr);
        } else {
            std::fprintf(stderr, "Could not wait for the native GUI test (Win32 error %lu).\n",
                         GetLastError());
        }
        TerminateProcess(process.hProcess, EXIT_FAILURE);
        CloseHandle(process.hProcess);
        return EXIT_FAILURE;
    }

    DWORD status = 0;
    const BOOL readStatus = GetExitCodeProcess(process.hProcess, &status);
    const DWORD statusError = readStatus ? ERROR_SUCCESS : GetLastError();
    CloseHandle(process.hProcess);
    if (!readStatus) {
        std::fprintf(stderr, "Could not read the native GUI test exit code (Win32 error %lu).\n",
                     statusError);
        return EXIT_FAILURE;
    }

    constexpr DWORD dllInitializationFailed = 0xc0000142UL;
    if (status == dllInitializationFailed) {
        std::fputs("Native GUI test could not start (0xc0000142, DLL initialization failed). "
                   "Run CTest from PowerShell on the interactive Windows desktop.\n", stderr);
    } else if (status != 0) {
        std::fprintf(stderr, "Native GUI test exited with status 0x%08lx.\n", status);
    }
    return status == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
