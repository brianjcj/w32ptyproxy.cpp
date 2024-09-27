// w32ptyproxy.cpp
//
#include <Windows.h>
#include <process.h>
#include <io.h>
#include <stdio.h>
#include <fcntl.h>
#include <wchar.h>

// Forward declarations
HRESULT CreatePseudoConsoleAndPipes(HPCON*, HANDLE*, HANDLE*, short, short);
HRESULT InitializeStartupInfoAttachedToPseudoConsole(STARTUPINFOEX*, HPCON);
void __cdecl PipeListener(LPVOID);
void __cdecl namePipeListener(LPVOID);


HRESULT(WINAPI* pCreatePseudoConsole)(COORD, HANDLE, HANDLE, DWORD, HPCON*);
HRESULT(WINAPI* pResizePseudoConsole)(HPCON, COORD);
void(WINAPI* pClosePseudoConsole)(HPCON);

const DWORD PSUEDOCONSOLE_INHERIT_CURSOR = 0x1;
const DWORD PSEUDOCONSOLE_RESIZE_QUIRK = 0x2;
const DWORD PSEUDOCONSOLE_WIN32_INPUT_MODE = 0x4;


bool static odyn_conpty_init(void)
{
    HINSTANCE hGetProcIDDLL = LoadLibrary(L"conpty.dll");

    if (hGetProcIDDLL == NULL) {
        perror("cannot load conpty.dll");
        return false;
    }
    static struct {
        const char* name;
        FARPROC* ptr;
    } conpty_entry[] = {
      { "CreatePseudoConsole", (FARPROC*)&pCreatePseudoConsole },
      { "ResizePseudoConsole", (FARPROC*)&pResizePseudoConsole },
      { "ClosePseudoConsole", (FARPROC*)&pClosePseudoConsole },
      { NULL, NULL }
    };

    for (int i = 0; conpty_entry[i].name != NULL
        && conpty_entry[i].ptr != NULL; ++i)
    {
        if ((*conpty_entry[i].ptr = (FARPROC)GetProcAddress(hGetProcIDDLL,
            conpty_entry[i].name)) == NULL)
        {
            perror("failed to find function");
            return false;
        }
        else
        {
            printf("function %s loaded\n", conpty_entry[i].name);
        }
    }

    return true;
}

HPCON hPC{ INVALID_HANDLE_VALUE };

int wmain(int argc, wchar_t** argv)
{
    if (!odyn_conpty_init()) {
        return -1;
    }

    wchar_t szCommand[]{ L"cmd.exe" };
    //wchar_t szCommand[]{ L"powershell.exe" };
    //wchar_t szCommand[]{ L"pwsh.exe" };

    wchar_t* pCmd = NULL;

    const wchar_t* pipeName = NULL;

    if (argc < 2)
    {
        pCmd = szCommand;
    }
    else
    {
        pCmd = argv[1];
    }

    short rows{ 28 };
    short cols{ 80 };
    if (argc >= 4)
    {
        rows = (short)wcstol(argv[2], NULL, 10);
        cols = (short)wcstol(argv[3], NULL, 10);
    }

    if (argc >= 5) {
        pipeName = argv[4];
    }
    else
    {
        pipeName = L"\\\\.\\pipe\\vtermPipe-1234";
    }

    printf("pipe name:%s\n", pipeName);

    HRESULT hr{ E_UNEXPECTED };
    HANDLE hConsole = { GetStdHandle(STD_OUTPUT_HANDLE) };

    // Enable Console VT Processing
    DWORD consoleMode{};
    BOOL boo = GetConsoleMode(hConsole, &consoleMode);
    if (boo)
    {
        hr = SetConsoleMode(hConsole, consoleMode | ENABLE_VIRTUAL_TERMINAL_PROCESSING | DISABLE_NEWLINE_AUTO_RETURN)
            ? S_OK
            : GetLastError();
    }

    DWORD consoleModeIn{};
    HANDLE hConsoleIn = { GetStdHandle(STD_INPUT_HANDLE) };
    GetConsoleMode(hConsoleIn, &consoleModeIn);
    hr = SetConsoleMode(hConsoleIn, consoleMode & ~(ENABLE_ECHO_INPUT | ENABLE_LINE_INPUT | ENABLE_PROCESSED_INPUT) | ENABLE_VIRTUAL_TERMINAL_INPUT | ENABLE_EXTENDED_FLAGS | ENABLE_QUICK_EDIT_MODE)
        ? S_OK
        : GetLastError();

    SetConsoleOutputCP(65001);

    int result;
    result = _setmode(_fileno(stdout), _O_BINARY);
    if (result == -1)
    {
        perror("Cannot set mode");
        return 1;
    }
    result = _setmode(_fileno(stdin), _O_BINARY);
    if (result == -1)
    {
        perror("Cannot set mode");
        return 1;
    }

    hr = S_OK;  // it is ok when running in emacs

    if (S_OK == hr)
    {

        //  Create the Pseudo Console and pipes to it
        HANDLE hPipeIn{ INVALID_HANDLE_VALUE };
        HANDLE hPipeOut{ INVALID_HANDLE_VALUE };
        hr = CreatePseudoConsoleAndPipes(&hPC, &hPipeIn, &hPipeOut, rows, cols);
        if (S_OK == hr)
        {
            // Create & start thread to listen to the incoming pipe
            // Note: Using CRT-safe _beginthread() rather than CreateThread()
            HANDLE hPipeListenerThread{ reinterpret_cast<HANDLE>(_beginthread(PipeListener, 0, hPipeIn)) };

            // Initialize the necessary startup info struct
            STARTUPINFOEX startupInfo{};
            if (S_OK == InitializeStartupInfoAttachedToPseudoConsole(&startupInfo, hPC))
            {
                // Launch ping to emit some text back via the pipe
                PROCESS_INFORMATION piClient{};
                hr = CreateProcess(
                    NULL,                           // No module name - use Command Line
                    pCmd,                           // Command Line
                    NULL,                           // Process handle not inheritable
                    NULL,                           // Thread handle not inheritable
                    FALSE,                          // Inherit handles
                    EXTENDED_STARTUPINFO_PRESENT | CREATE_UNICODE_ENVIRONMENT,   // Creation flags
                    NULL,                           // Use parent's environment block
                    NULL,                           // Use parent's starting directory
                    &startupInfo.StartupInfo,       // Pointer to STARTUPINFO
                    &piClient)                      // Pointer to PROCESS_INFORMATION
                    ? S_OK
                    : GetLastError();

                if (S_OK == hr)
                {

                    // create a name pipe

                    HANDLE hNamePipe = CreateNamedPipe(pipeName,
                        PIPE_ACCESS_DUPLEX,
                        // PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT,   // FILE_FLAG_FIRST_PIPE_INSTANCE is not needed but forces CreateNamedPipe(..) to fail if the pipe already exists...
                        PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT,
                        PIPE_UNLIMITED_INSTANCES, //1,
                        1024 * 16,
                        1024 * 16,
                        NMPWAIT_USE_DEFAULT_WAIT,
                        NULL);
                    if (hNamePipe == INVALID_HANDLE_VALUE)
                    {
                        printf("failed to create name pipe");
                        return EXIT_FAILURE;
                    }

                    HANDLE hPipeListenerThread{ reinterpret_cast<HANDLE>(_beginthread(namePipeListener, 0, hNamePipe)) };


                    const DWORD BUFF_SIZE{ 512 };
                    char szBuffer[BUFF_SIZE]{};
                    HANDLE hConsoleIn{ GetStdHandle(STD_INPUT_HANDLE) };
                    DWORD dwBytesWritten{};
                    DWORD dwBytesRead{};
                    BOOL fRead{ FALSE };
                    do
                    {
                        // Read from console/stdin
                        fRead = ReadFile(hConsoleIn, szBuffer, BUFF_SIZE, &dwBytesRead, NULL);
                        if (!fRead)
                        {
                            break;
                        }

                        DWORD written{ 0 };
                        while (1)
                        {
                            BOOL boo = WriteFile(hPipeOut, szBuffer + written, dwBytesRead - written, &dwBytesWritten, NULL);
                            if (!boo)
                            {
                                printf("failed to write to pipe!\n");
                                exit(-1);
                            }
                            written += dwBytesWritten;

                            if (dwBytesRead == written)
                            {
                                break;
                            }
                        }

                    } while (fRead && dwBytesRead >= 0);

                }

                // --- CLOSEDOWN ---

                // Now safe to clean-up client app's process-info & thread
                CloseHandle(piClient.hThread);
                CloseHandle(piClient.hProcess);

                // Cleanup attribute list
                if (startupInfo.lpAttributeList != NULL) {
                    DeleteProcThreadAttributeList(startupInfo.lpAttributeList);
                    free(startupInfo.lpAttributeList);
                }
            }

            // Close ConPTY - this will terminate client process if running
            pClosePseudoConsole(hPC);

            // Clean-up the pipes
            if (INVALID_HANDLE_VALUE != hPipeOut) CloseHandle(hPipeOut);
            if (INVALID_HANDLE_VALUE != hPipeIn) CloseHandle(hPipeIn);
        }
    }

    return S_OK == hr ? EXIT_SUCCESS : EXIT_FAILURE;
}

HRESULT CreatePseudoConsoleAndPipes(HPCON* phPC, HANDLE* phPipeIn, HANDLE* phPipeOut, short rows, short cols)
{
    HRESULT hr{ E_UNEXPECTED };
    HANDLE hPipePTYIn{ INVALID_HANDLE_VALUE };
    HANDLE hPipePTYOut{ INVALID_HANDLE_VALUE };

    // Create the pipes to which the ConPTY will connect
    if (CreatePipe(&hPipePTYIn, phPipeOut, NULL, 0) &&
        CreatePipe(phPipeIn, &hPipePTYOut, NULL, 0))
    {
        // Determine required size of Pseudo Console
        COORD consoleSize{};
        CONSOLE_SCREEN_BUFFER_INFO csbi{};
        HANDLE hConsole{ GetStdHandle(STD_OUTPUT_HANDLE) };
        if (GetConsoleScreenBufferInfo(hConsole, &csbi))
        {
            consoleSize.X = csbi.srWindow.Right - csbi.srWindow.Left + 1;
            consoleSize.Y = csbi.srWindow.Bottom - csbi.srWindow.Top + 1;
        }
        else {
            consoleSize.X = cols;
            consoleSize.Y = rows;
        }

        // Create the Pseudo Console of the required size, attached to the PTY-end of the pipes
        hr = pCreatePseudoConsole(consoleSize, hPipePTYIn, hPipePTYOut,
            PSUEDOCONSOLE_INHERIT_CURSOR | PSEUDOCONSOLE_RESIZE_QUIRK | PSEUDOCONSOLE_WIN32_INPUT_MODE,
            phPC);


        // Note: We can close the handles to the PTY-end of the pipes here
        // because the handles are dup'ed into the ConHost and will be released
        // when the ConPTY is destroyed.
        if (INVALID_HANDLE_VALUE != hPipePTYOut) CloseHandle(hPipePTYOut);
        if (INVALID_HANDLE_VALUE != hPipePTYIn) CloseHandle(hPipePTYIn);
    }

    return hr;
}

// Initializes the specified startup info struct with the required properties and
// updates its thread attribute list with the specified ConPTY handle
HRESULT InitializeStartupInfoAttachedToPseudoConsole(STARTUPINFOEX* pStartupInfo, HPCON hPC)
{
    HRESULT hr{ E_UNEXPECTED };

    if (pStartupInfo)
    {
        size_t attrListSize{};

        pStartupInfo->StartupInfo.cb = sizeof(STARTUPINFOEX);

        // Get the size of the thread attribute list.
        InitializeProcThreadAttributeList(NULL, 1, 0, &attrListSize);

        // Allocate a thread attribute list of the correct size
        pStartupInfo->lpAttributeList =
            reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(malloc(attrListSize));

        // Initialize thread attribute list
        if (pStartupInfo->lpAttributeList
            && InitializeProcThreadAttributeList(pStartupInfo->lpAttributeList, 1, 0, &attrListSize))
        {
            pStartupInfo->StartupInfo.dwFlags |= STARTF_USESTDHANDLES;

            // Set Pseudo Console attribute
            hr = UpdateProcThreadAttribute(
                pStartupInfo->lpAttributeList,
                0,
                PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE,
                hPC,
                sizeof(HPCON),
                NULL,
                NULL)
                ? S_OK
                : HRESULT_FROM_WIN32(GetLastError());
        }
        else
        {
            hr = HRESULT_FROM_WIN32(GetLastError());
        }
    }
    return hr;
}

void __cdecl PipeListener(LPVOID pipe)
{
    HANDLE hPipe{ pipe };
    HANDLE hConsole{ GetStdHandle(STD_OUTPUT_HANDLE) };

    const DWORD BUFF_SIZE{ 512 };
    char szBuffer[BUFF_SIZE]{};

    DWORD dwBytesWritten{};
    DWORD dwBytesRead{};
    BOOL fRead{ FALSE };
    do
    {
        // Read from the pipe
        fRead = ReadFile(hPipe, szBuffer, BUFF_SIZE, &dwBytesRead, NULL);
        if (!fRead)
        {
            break;
        }

        // Write received text to the Console
        // Note: Write to the Console using WriteFile(hConsole...), not printf()/puts() to
        // prevent partially-read VT sequences from corrupting output

        DWORD written{ 0 };
        while (1)
        {
            BOOL boo = WriteFile(hConsole, szBuffer + written, dwBytesRead - written, &dwBytesWritten, NULL);
            if (!boo)
            {
                printf("failed to write to pipe!\n");
                exit(-1);
            }
            written += dwBytesWritten;

            if (dwBytesRead == written)
            {
                break;
            }
        }

    } while (fRead && dwBytesRead >= 0);

    printf("PipeListener quit!\n");
    exit(-1);
}

void __cdecl namePipeListener(LPVOID pipe)
{
    HANDLE hPipe{ pipe };
    char buffer[1024];
    DWORD dwRead;

    while (hPipe != INVALID_HANDLE_VALUE) {
        if (ConnectNamedPipe(hPipe, NULL) != FALSE)   // wait for someone to connect to the pipe
        {
            while (ReadFile(hPipe, buffer, sizeof(buffer) - 1, &dwRead, NULL) != FALSE)
            {

                byte msgType = buffer[0];

                if (msgType == 1) {
                    short* ps = (short*)(buffer + 1);
                    short rows = *ps;
                    short cols = *(ps + 1);
                    printf("msg type: %d, rows: %d, cols: %d\n", msgType, rows, cols);

                    COORD consoleSize{};
                    consoleSize.X = cols;
                    consoleSize.Y = rows;

                    pResizePseudoConsole(hPC, consoleSize);
                }

            }
        }

        DisconnectNamedPipe(hPipe);
    }


}
