
#define WINDOWS_LEAN_AND_MEAN
#include <windows.h>

#include <Tchar.h>
#include <strsafe.h>

#ifndef LOG_BUFFER_SIZE
#define LOG_BUFFER_SIZE 512
#endif

// Max open files is 32. BitField is set to have 32 bits, so let that be max.
#define MAX_OPEN_FILES 32

struct platform_window
{
    HWND Window;
};

typedef struct file
{
    HANDLE Handle;
    u64    FileSize;
    void  *Memory;
    u32    Index; // HACK(Dustin): backpointer to quickly access the original index
} file;

file_global HWND ClientWindow;

file_global file OpenFiles[MAX_OPEN_FILES];
// bitfield for open files. 1 = open, 0 = closed
file_global u32 OpenFileBitField = 0;

mstr Win32NormalizePath(char* path);
mstr Win32GetExeFilepath();
i32  __Win32FormatString(char *buff, i32 len, char *fmt, va_list list);
void __Win32PrintMessage(console_color text_color, console_color background_color, char *fmt, va_list args);
void __Win32PrintError(console_color text_color, console_color background_color, char *fmt, va_list args);

// source: Windows API doc: https://docs.microsoft.com/en-us/windows/win32/fileio/opening-a-file-for-reading-or-writing
file_internal void DisplayError(LPTSTR lpszFunction)
// Routine Description:
// Retrieve and output the system error message for the last-error code
{
    LPVOID lpMsgBuf;
    LPVOID lpDisplayBuf;
    DWORD dw = GetLastError();
    
    FormatMessage(FORMAT_MESSAGE_ALLOCATE_BUFFER |
                  FORMAT_MESSAGE_FROM_SYSTEM |
                  FORMAT_MESSAGE_IGNORE_INSERTS,
                  NULL,
                  dw,
                  MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
                  (LPTSTR) &lpMsgBuf,
                  0,
                  NULL );
    
    lpDisplayBuf =
        (LPVOID)LocalAlloc( LMEM_ZEROINIT,
                           ( lstrlen((LPCTSTR)lpMsgBuf)
                            + lstrlen((LPCTSTR)lpszFunction)
                            + 40) // account for format string
                           * sizeof(TCHAR) );
    
    if (FAILED( StringCchPrintf((LPTSTR)lpDisplayBuf,
                                LocalSize(lpDisplayBuf) / sizeof(TCHAR),
                                TEXT("%s failed with error code %d as follows:\n%s"),
                                lpszFunction,
                                dw,
                                lpMsgBuf)))
    {
        mprinte("FATAL ERROR: Unable to output error code.\n");
    }
    
    _tprintf(TEXT("ERROR: %s\n"), (LPCTSTR)lpDisplayBuf);
    
    LocalFree(lpMsgBuf);
    LocalFree(lpDisplayBuf);
}

// An internal allocation scheme that allocates from the 2MB Platform Linear allcoator.
// this functions is primarily used by print/formatting functions that need temporary,
// dynamic memory. When the heap is filled, the allocator is reset.
file_internal void* PlatformLocalAlloc(u32 Size)
{
    void* Result = NULL;
    
#if 0
    if (!PlatformHeap.Start)
        PlatformHeap = TaggedHeapRequestAllocation(&Core->TaggedHeap, PlatformTag);
    
    Result = TaggedHeapBlockAlloc(&PlatformHeap, Size);
    if (!Result)
    {
        PlatformHeap.Brkp = PlatformHeap.Start;
        
        Result = TaggedHeapBlockAlloc(&PlatformHeap, Size);
        if (!Result)
            mprinte("Error allocating from Platform Heap Allocator!\n");
    }
#else
    
    // TODO(Dustin): Temporary fix for not having a tagged heap/string right now
    Result = malloc(Size);
    
#endif
    
    return Result;
}

void PlatformFatalError(char *Fmt, ...)
{
    va_list Args;
    va_start(Args, Fmt);
    
    char *Message = NULL;
    int CharsRead = 1 + __Win32FormatString(Message, 1, Fmt, Args);
    Message = (char*)PlatformLocalAlloc(CharsRead);
    __Win32FormatString(Message, CharsRead, Fmt, Args);
    
    MessageBox(ClientWindow, Message, "FATAL ERROR", MB_OK);
    //MapleShutdown();
    exit(1);
}

//~ Logging

typedef struct
{
    HANDLE handle; // Stream handle (STD_OUTPUT_HANDLE or STD_ERROR_HANDLE).
    bool is_redirected; // True if redirected to file.
    bool is_wide; // True if appending to a UTF-16 file.
    bool is_little_endian; // True if file is UTF-16 little endian.
} Win32StandardStream;

file_internal bool RedirectConsoleIO()
{
    bool result = true;
    FILE* fp;
    
    // Redirect STDIN if the console has an input handle
    if (GetStdHandle(STD_INPUT_HANDLE) != INVALID_HANDLE_VALUE)
    {
        if (freopen_s(&fp, "CONIN$", "r", stdin) != 0)
            result = false;
    }
    else
    {
        setvbuf(stdin, NULL, _IONBF, 0);
    }
    
    // Redirect STDOUT if the console has an output handle
    if (GetStdHandle(STD_OUTPUT_HANDLE) != INVALID_HANDLE_VALUE)
    {
        if (freopen_s(&fp, "CONOUT$", "w", stdout) != 0)
            result = false;
    }
    else
    {
        setvbuf(stdout, NULL, _IONBF, 0);
    }
    
    // Redirect STDERR if the console has an error handle
    if (GetStdHandle(STD_ERROR_HANDLE) != INVALID_HANDLE_VALUE)
    {
        if (freopen_s(&fp, "CONOUT$", "w", stderr) != 0)
            result = false;
    }
    else
    {
        setvbuf(stderr, NULL, _IONBF, 0);
    }
    
    return result;
}

// Sets up a standard stream (stdout or stderr).
file_internal Win32StandardStream Win32GetStandardStream(DWORD stream_type)
{
    Win32StandardStream result = {0};
    
    // If we don't have our own stream and can't find a parent console,
    // allocate a new console.
    result.handle = GetStdHandle(stream_type);
    if (!result.handle || result.handle == INVALID_HANDLE_VALUE)
    {
        if (!AttachConsole(ATTACH_PARENT_PROCESS))
        {
            AllocConsole();
            RedirectConsoleIO();
        }
        result.handle = GetStdHandle(stream_type);
    }
    
    // Check if the stream is redirected to a file. If it is, check if
    // the file already exists. If so, parse the encoding.
    if (result.handle != INVALID_HANDLE_VALUE)
    {
        DWORD type = GetFileType(result.handle) & (~FILE_TYPE_REMOTE);
        DWORD dummy;
        result.is_redirected = (type == FILE_TYPE_CHAR) ? !GetConsoleMode(result.handle, &dummy) : true;
        if (type == FILE_TYPE_DISK)
        {
            LARGE_INTEGER file_size;
            GetFileSizeEx(result.handle, &file_size);
            if (file_size.QuadPart > 1)
            {
                LARGE_INTEGER large = {0};
                u16 bom = 0;
                SetFilePointerEx(result.handle, large, 0, FILE_BEGIN);
                ReadFile(result.handle, &bom, 2, &dummy, 0);
                SetFilePointerEx(result.handle, large, 0, FILE_END);
                result.is_wide = (bom == (u16)0xfeff || bom == (u16)0xfffe);
                result.is_little_endian = (bom == (u16)0xfffe);
            }
        }
    }
    return result;
}

// Translates foreground/background color into a WORD text attribute.
file_internal WORD Win32TranslateConsoleColors(console_color text_color, console_color background_color)
{
    WORD result = 0;
    switch (text_color)
    {
        case ConsoleColor_White:
        result |=  FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_BLUE | FOREGROUND_INTENSITY;
        break;
        case ConsoleColor_DarkGrey:
        result |= FOREGROUND_INTENSITY;
        break;
        case ConsoleColor_Grey:
        result |= FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_BLUE;
        break;
        case ConsoleColor_DarkRed:
        result |= FOREGROUND_RED;
        break;
        case ConsoleColor_Red:
        result |= FOREGROUND_RED | FOREGROUND_INTENSITY;
        break;
        case ConsoleColor_DarkGreen:
        result |= FOREGROUND_GREEN;
        break;
        case ConsoleColor_Green:
        result |= FOREGROUND_GREEN | FOREGROUND_INTENSITY;
        break;
        case ConsoleColor_DarkBlue:
        result |= FOREGROUND_BLUE;
        break;
        case ConsoleColor_Blue:
        result |= FOREGROUND_BLUE | FOREGROUND_INTENSITY;
        break;
        case ConsoleColor_DarkCyan:
        result |= FOREGROUND_GREEN | FOREGROUND_BLUE;
        break;
        case ConsoleColor_Cyan:
        result |= FOREGROUND_GREEN | FOREGROUND_BLUE | FOREGROUND_INTENSITY;
        break;
        case ConsoleColor_DarkPurple:
        result |= FOREGROUND_RED | FOREGROUND_BLUE;
        break;
        case ConsoleColor_Purple:
        result |= FOREGROUND_RED | FOREGROUND_BLUE | FOREGROUND_INTENSITY;
        break;
        case ConsoleColor_DarkYellow:
        result |= FOREGROUND_RED | FOREGROUND_GREEN;
        break;
        case ConsoleColor_Yellow:
        result |= FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_INTENSITY;
        break;
        default:
        break;
    }
    
    switch (background_color)
    {
        case ConsoleColor_White:
        result |=  FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_BLUE | FOREGROUND_INTENSITY;
        break;
        case ConsoleColor_DarkGrey:
        result |=  FOREGROUND_INTENSITY;
        break;
        case ConsoleColor_Grey:
        result |= FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_BLUE;
        break;
        case ConsoleColor_DarkRed:
        result |= FOREGROUND_RED;
        break;
        case ConsoleColor_Red:
        result |= FOREGROUND_RED | FOREGROUND_INTENSITY;
        break;
        case ConsoleColor_DarkGreen:
        result |= FOREGROUND_GREEN;
        break;
        case ConsoleColor_Green:
        result |= FOREGROUND_GREEN | FOREGROUND_INTENSITY;
        break;
        case ConsoleColor_DarkBlue:
        result |= FOREGROUND_BLUE;
        break;
        case ConsoleColor_Blue:
        result |= FOREGROUND_BLUE | FOREGROUND_INTENSITY;
        break;
        case ConsoleColor_DarkCyan:
        result |= FOREGROUND_GREEN | FOREGROUND_BLUE;
        break;
        case ConsoleColor_Cyan:
        result |= FOREGROUND_GREEN | FOREGROUND_BLUE | FOREGROUND_INTENSITY;
        break;
        case ConsoleColor_DarkPurple:
        result |= FOREGROUND_RED | FOREGROUND_BLUE;
        break;
        case ConsoleColor_Purple:
        result |= FOREGROUND_RED | FOREGROUND_BLUE | FOREGROUND_INTENSITY;
        break;
        case ConsoleColor_DarkYellow:
        result |= FOREGROUND_RED | FOREGROUND_GREEN;
        break;
        case ConsoleColor_Yellow:
        result |= FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_INTENSITY;
        break;
        default:
        break;
    }
    
    return result;
}

// Prints a message to a platform stream. If the stream is a console, uses
// supplied colors.
file_internal void Win32PrintToStream(const char* message, Win32StandardStream stream, console_color text_color, console_color background_color)
{
    
    // If redirected, write to a file instead of console.
    DWORD dummy;
    if (stream.is_redirected)
    {
        if (stream.is_wide)
        {
            static wchar_t buf[LOG_BUFFER_SIZE];
            i32 required_size = MultiByteToWideChar(CP_UTF8, 0, message, -1, 0, 0) - 1;
            i32 offset;
            for (offset = 0; offset + LOG_BUFFER_SIZE , required_size; offset += LOG_BUFFER_SIZE)
            {
                // TODO(Matt): Little endian BOM.
                MultiByteToWideChar(CP_UTF8, 0, &message[offset], LOG_BUFFER_SIZE, buf, LOG_BUFFER_SIZE);
                WriteFile(stream.handle, buf, LOG_BUFFER_SIZE * 2, &dummy, 0);
            }
            i32 mod = required_size % LOG_BUFFER_SIZE;
            i32 size = MultiByteToWideChar(CP_UTF8, 0, &message[offset], mod, buf, LOG_BUFFER_SIZE) * 2;
            WriteFile(stream.handle, buf, size, &dummy, 0);
        }
        else
        {
            WriteFile(stream.handle, message, (DWORD)strlen(message), &dummy, 0);
        }
    }
    else
    {
        WORD attribute = Win32TranslateConsoleColors(text_color, background_color);
        SetConsoleTextAttribute(stream.handle, attribute);
        WriteConsole(stream.handle, message, (DWORD)strlen(message), &dummy, 0);
        attribute = FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_BLUE;
        SetConsoleTextAttribute(stream.handle, attribute);
    }
}

i32 __Win32FormatString(char *buff, i32 len, char *fmt, va_list list)
{
    // if a caller doesn't actually know the length of the
    // format list, and is querying for the required size,
    // attempt to format the string into the buffer first
    // before copying in the chars.
    //
    // This handles the case where the buffer is declared like:
    //     char *buff = nullptr;
    char test_buff[12];
    va_list cpy;
    va_copy(cpy, list);
    u32 needed_chars = vsnprintf(NULL, 0, fmt, cpy);
    va_end(cpy);
    
    if (needed_chars < len)
    {
        needed_chars = vsnprintf(buff, len, fmt, list);
    }
    
    return needed_chars;
}

void __Win32PrintMessage(console_color text_color, console_color background_color, char *fmt, va_list args)
{
    char *message = NULL;
    int chars_read = 1 + __Win32FormatString(message, 1, fmt, args);
    message = (char*)PlatformLocalAlloc(chars_read);
    __Win32FormatString(message, chars_read, fmt, args);
    
    // If we are in the debugger, output there.
    if (IsDebuggerPresent())
    {
        OutputDebugStringA(message);
    }
    else
    {
        // Otherwise, output to stdout.
        Win32StandardStream stream = Win32GetStandardStream(STD_OUTPUT_HANDLE);
        Win32PrintToStream(message, stream, text_color, background_color);
    }
}

void __Win32PrintError(console_color text_color, console_color background_color, char *fmt, va_list args)
{
    char *message = NULL;
    int chars_read = 1 + __Win32FormatString(message, 1, fmt, args);
    message = (char*)PlatformLocalAlloc(chars_read);
    __Win32FormatString(message, chars_read, fmt, args);
    
    if (IsDebuggerPresent())
    {
        OutputDebugStringA(message);
    }
    else
    {
        // Otherwise, output to stderr.
        Win32StandardStream stream = Win32GetStandardStream(STD_ERROR_HANDLE);
        Win32PrintToStream(message, stream, text_color, background_color);
    }
}

i32 PlatformFormatString(char *buff, i32 len, char* fmt, ...)
{
    va_list list;
    va_start(list, fmt);
    int chars_read = __Win32FormatString(buff, len, fmt, list);
    va_end(list);
    
    return chars_read;
}

void PlatformPrintMessage(console_color text_color, console_color background_color, char* fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    __Win32PrintMessage(text_color, background_color, fmt, args);
    va_end(args);
}

void PlatformPrintError(console_color text_color, console_color background_color, char* fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    __Win32PrintError(text_color, background_color, fmt, args);
    va_end(args);
}

inline void mprint(char *fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    __Win32PrintMessage(ConsoleColor_White, ConsoleColor_DarkGrey, fmt, args);
    va_end(args);
}

inline void mprinte(char *fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    __Win32PrintError(ConsoleColor_Red, ConsoleColor_DarkGrey, fmt, args);
    va_end(args);
}

void* PlatformRequestMemory(u64 Size)
{
    SYSTEM_INFO sSysInfo;
    DWORD       dwPageSize;
    LPVOID      lpvBase;
    u64         ActualSize;
    
    GetSystemInfo(&sSysInfo);
    dwPageSize = sSysInfo.dwPageSize;
    
    ActualSize = (Size + (u64)dwPageSize - 1) & ~((u64)dwPageSize - 1);
    
    lpvBase = VirtualAlloc(NULL,                    // System selects address
                           ActualSize,              // Size of allocation
                           MEM_COMMIT|MEM_RESERVE,  // Allocate reserved pages
                           PAGE_READWRITE);          // Protection = no access
    
    return lpvBase;
}

void PlatformReleaseMemory(void *Ptr, u64 Size)
{
    BOOL bSuccess = VirtualFree(Ptr,           // Base address of block
                                0,             // Bytes of committed pages
                                MEM_RELEASE);  // Decommit the pages
    assert(bSuccess && "Unable to free a VirtualAlloc allocation!");
}


void PlatformGetClientWindowDimensions(u32 *Width, u32 *Height)
{
    RECT rect;
    GetClientRect(ClientWindow, &rect);
    
    *Width  = rect.right - rect.left;
    *Height = rect.bottom - rect.top;
}

window_rect PlatformGetClientWindowRect()
{
    window_rect Result = {0};
    
    RECT Rect;
    GetClientRect(ClientWindow, &Rect);
    
    Result.Left   = Rect.left;
    Result.Right  = Rect.right;
    Result.Top    = Rect.top;
    Result.Bottom = Rect.bottom;
    
    return Result;
}

void PlatformGetClientWindow(platform_window *Window)
{
    HWND *Win32Window = (HWND*)Window;
    Win32Window = &ClientWindow;
}

const char* PlatformGetRequiredInstanceExtensions(bool validation_layers)
{
    VkResult err;
    VkExtensionProperties* ep;
    u32 count = 0;
    err = vk::vkEnumerateInstanceExtensionProperties(NULL, &count, NULL);
    if (err)
    {
        mprinte("Error enumerating Instance extension properties!\n");
    }
    
    ep = palloc<VkExtensionProperties>(count);
    err = vk::vkEnumerateInstanceExtensionProperties(NULL, &count, ep);
    if (err)
    {
        mprinte("Unable to retrieve enumerated extension properties!\n");
        count = 0;
    }
    
    for (u32 i = 0;  i < count;  i++)
    {
        if (strcmp(ep[i].extensionName, "VK_KHR_win32_surface") == 0)
        {
            const char *plat_exts = "VK_KHR_win32_surface";
            
            pfree(ep);
            
            return plat_exts;
        }
    }
    
    pfree(ep);
    mprinte("Could not find win32 vulkan surface extension!\n");
    
    return "";
}

void PlatformVulkanCreateSurface(VkSurfaceKHR *surface, VkInstance vulkan_instance)
{
    VkWin32SurfaceCreateInfoKHR surface_info = {};
    surface_info.sType = VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR;
    surface_info.hinstance = GetModuleHandle(NULL);;
    surface_info.hwnd      = ClientWindow;
    
    VK_CHECK_RESULT(vk::vkCreateWin32SurfaceKHR(vulkan_instance, &surface_info, nullptr, surface),
                    "Unable to create XCB Surface!\n");
}

void PlatformSetClientWindow(platform_window *Window)
{
    ClientWindow = *((HWND*)Window);
}

//~ File I/O

// Takes a path relative to the executable, and normalizes it into a full path.
// Tries to handle malformed input, but returns an empty string if unsuccessful.
mstr Win32NormalizePath(char* path)
{
    // If the string is null or has length < 2, just return an empty one.
    if (!path || !path[0] || !path[1]) return {};
    
    // Start with our relative path appended to the full executable path.
    mstr exe_path = Win32GetExeFilepath();
    mstr result = cstr_add(mstr_to_cstr(&exe_path), exe_path.Len, path, strlen(path));
    
    char *Str = mstr_to_cstr(&result);
    
    // Swap any back slashes for forward slashes.
    for (u32 i = 0; i < (u32)result.Len; ++i) if (Str[i] == '\\') Str[i] = '/';
    
    // Strip double separators.
    for (u32 i = 0; i < (u32)result.Len - 1; ++i)
    {
        if (Str[i] == '/' && Str[i + 1] == '/')
        {
            for (u32 j = i; j < (u32)result.Len; ++j) Str[j] = Str[j + 1];
            --result.Len;
            --i;
        }
    }
    
    // Evaluate any relative specifiers (./).
    if (Str[0] == '.' && Str[1] == '/')
    {
        for (u32 i = 0; i < (u32)result.Len - 1; ++i) Str[i] = Str[i + 2];
        result.Len -= 2;
    }
    for (u32 i = 0; i < (u32)result.Len - 1; ++i)
    {
        if (Str[i] != '.' && Str[i + 1] == '.' && Str[i + 2] == '/')
        {
            for (u32 j = i + 1; Str[j + 1]; ++j) Str[j] = Str[j + 2];
            result.Len -= 2;
        }
    }
    
    // Evaluate any parent specifiers (../).
    u32 last_separator = 0;
    for (u32 i = 0; (i < (u32)result.Len - 1); ++i)
    {
        if (Str[i] == '.' && Str[i + 1] == '.' && Str[i + 2] == '/')
        {
            u32 base = i + 2;
            u32 count = result.Len - base;
            
            for (u32 j = 0; j <= count; ++j)
            {
                Str[last_separator + j] = Str[base + j];
            }
            
            result.Len -= base - last_separator;
            i = last_separator;
            
            if (i > 0)
            {
                bool has_separator = false;
                for (i32 j = last_separator - 1; j >= 0; --j)
                {
                    if (Str[j] == '/')
                    {
                        last_separator = j;
                        has_separator = true;
                        break;
                    }
                }
                if (!has_separator) return {};
            }
        }
        if (i > 0 && Str[i - 1] == '/') last_separator = i - 1;
    }
    
    mstr_free(&exe_path);
    return result;
}

#define WIN32_STATE_FILE_NAME_COUNT MAX_PATH
mstr Win32GetExeFilepath()
{
    // Get the full filepath
    char exe[WIN32_STATE_FILE_NAME_COUNT];
    size_t filename_size = sizeof(exe);
    
    DWORD filepath_size = GetModuleFileNameA(0, exe, filename_size);
    
    // find last "\"
    char *pos = 0;
    for (char *Scanner = exe; *Scanner; ++Scanner)
    {
        if (*Scanner == '\\')
        {
            *Scanner = '/'; // normalize the slash to be unix style
            pos = Scanner + 1;
        }
    }
    
    int len = (pos - exe) + 1;
    
    mstr result = mstr_init(exe, (pos - exe));
    
    return result;
}

file_t FindAvailableFileHandle()
{
    file_t Result = NULL;
    
    // Negate it so that 1s are unused bits
    // and 0s are used bits
    u32 NegatedBitfield = ~OpenFileBitField;
    u32 Index = PlatformCtz(NegatedBitfield);
    
    if (Index < 32)
        
    {
        Result = OpenFiles + Index;
    }
    else
    {
        mprinte("No available file handles!");
    }
    
    BIT_TOGGLE_1(OpenFileBitField, Index);
    
    return Result;
}

file_t PlatformLoadFile(char *Filename, bool Append)
{
    file_t Result = NULL;
    
    // Find an open file handle
#if 0
    for (u32 i = 0; i < MAX_OPEN_FILES; ++i)
    {
        if (OpenFiles[i].Handle == INVALID_HANDLE_VALUE)
        {
            Result = OpenFiles + i;
            break;
        }
    }
#else
    
    Result = FindAvailableFileHandle();
    
#endif
    
    if (Result)
    {
        mstr AbsPath = Win32NormalizePath(Filename);
        
        Result->Handle = CreateFileA(mstr_to_cstr(&AbsPath),
                                     GENERIC_READ,
                                     0,
                                     NULL,
                                     OPEN_EXISTING,
                                     FILE_ATTRIBUTE_NORMAL,
                                     NULL);
        
        if (Result->Handle == INVALID_HANDLE_VALUE)
        {
            DisplayError(TEXT("CreateFile"));
            mstr_free(&AbsPath);
            
            BIT_TOGGLE_0(OpenFileBitField, Result->Index);
            Result = NULL;
            
            return Result;
        }
        
        // retrieve the file information the
        BY_HANDLE_FILE_INFORMATION file_info;
        // NOTE(Dustin): This can also be used to query the last time the file has been written to
        BOOL err = GetFileInformationByHandle(Result->Handle, &file_info);
        
        if (!err)
        {
            DisplayError(TEXT("GetFileInformationByHandle"));
            CloseHandle(Result->Handle);
            Result->Handle = INVALID_HANDLE_VALUE;
            
            BIT_TOGGLE_0(OpenFileBitField, Result->Index);
            Result = NULL;
            
            return Result;
        }
        
        // size of a DWORD is always 32 bits
        // NOTE(Dustin): Will there be a case where the file size will
        // be greater than 2^32? If so, might want to stream the data.
        // ReadFile max size seems to be 2^32.
        DWORD low_size  = file_info.nFileSizeLow;
        DWORD high_size = file_info.nFileSizeHigh;
        
        // NOTE(Dustin): Instead of OR'ing the values together, could probably
        // just check to see if high_size is greater than 0
        u64 file_size = (((u64)high_size)<<32) | (((u64)low_size)<<0);
        if (file_size >= ((u64)1<<32))
        {
            mprinte("File \"%s\" is too large and should probably be streamed from disc!\n", 
                    mstr_to_cstr(&AbsPath));
            CloseHandle(Result->Handle);
            Result->Handle = INVALID_HANDLE_VALUE;
            
            BIT_TOGGLE_0(OpenFileBitField, Result->Index);
            Result = NULL;
            
            return Result;
        }
        
        DWORD bytes_read = 0;
        Result->FileSize = file_size;
        Result->Memory = PlatformRequestMemory(file_size);
        
        err = ReadFile(Result->Handle,
                       Result->Memory,
                       low_size,
                       &bytes_read,
                       NULL);
        
        
        
        if (err == 0)
        {
            // NOTE(Dustin): Note that 0 can be returned if the
            // read operating is occuring asynchronously
            DisplayError(TEXT("ReadFile"));
            CloseHandle(Result->Handle);
            
            BIT_TOGGLE_0(OpenFileBitField, Result->Index);
            Result = NULL;
            
            return Result;
        }
        
        mstr_to_cstr(&AbsPath);
    }
    else
    {
        mprinte("Unable to find an open file handle. Please close a file handle to open another!\n");
    }
    
    return Result;
}

file_t PlatformOpenFile(char *Filename, bool Append)
{
    file_t Result = NULL;
    
    // Find an open file handle
#if 0
    for (u32 i = 0; i < MAX_OPEN_FILES; ++i)
    {
        if (OpenFiles[i].Handle == INVALID_HANDLE_VALUE)
        {
            Result = OpenFiles + i;
            break;
        }
    }
#else
    
    Result = FindAvailableFileHandle();
    
#endif
    
    if (Result)
    {
        mstr AbsPath = Win32NormalizePath(Filename);
        
        if (Append)
        {
            Result->Handle = CreateFileA(mstr_to_cstr(&AbsPath), GENERIC_WRITE, 0, 0,
                                         OPEN_EXISTING,
                                         FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, 0);
        }
        else
        {
            Result->Handle = CreateFileA(mstr_to_cstr(&AbsPath), GENERIC_WRITE, 0, 0,
                                         TRUNCATE_EXISTING,
                                         FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, 0);
        }
        
        // file doesn't currently exist
        if (Result->Handle == INVALID_HANDLE_VALUE)
        {
            Result->Handle = CreateFileA(mstr_to_cstr(&AbsPath), GENERIC_WRITE, 0, 0, CREATE_NEW,
                                         FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, 0);
        }
        
        if (Result->Handle == INVALID_HANDLE_VALUE)
        {
            DisplayError(TEXT("CreateFile"));
            _tprintf(TEXT("Terminal failure: Unable to open file \"%s\" for write.\n"), 
                     mstr_to_cstr(&AbsPath));
            
            BIT_TOGGLE_0(OpenFileBitField, Result->Index);
            Result = NULL;
        }
        
        // TODO(Dustin): Acquire some memory to write to!
    }
    else
    {
        mprinte("Unable to find an open file handle. Please close a file handle to open another!\n");
    }
    
    return Result;
}

void PlatformCloseFile(file_t File)
{
    if (File->Memory) PlatformReleaseMemory(File->Memory, File->FileSize);
    File->Memory = nullptr;
    
    if (File->Handle) CloseHandle(File->Handle);
    File->FileSize = 0;
    File->Handle   = INVALID_HANDLE_VALUE;
    
    BIT_TOGGLE_0(OpenFileBitField, File->Index);
}

void* GetFileBuffer(file_t File)
{
    return File->Memory;
}

// Get the current size of the file. Only useful when reading files.
u64 PlatformGetFileSize(file_t File)
{
    return File->FileSize;
}


u32 PlatformClz(u32 Value)
{
    unsigned long LeadingZero = 0;
    
    if (_BitScanReverse64(&LeadingZero, Value))
        return 31 - LeadingZero;
    else
        return 32;
}

u32 PlatformCtz(u32 Value)
{
    unsigned long TrailingZero = 0;
    
    if (Value == 0) return 0;
    else if (_BitScanForward64(&TrailingZero, Value))
        return TrailingZero;
    else
        return 32;
}

u32 PlatformClzl(u64 Value)
{
    unsigned long LeadingZero = 0;
    
    if (Value == 0) return 0;
    else if (_BitScanReverse64(&LeadingZero, Value))
        return 31 - LeadingZero;
    else
        return 32;
}

u32 PlatformCtzl(u64 Value)
{
    unsigned long TrailingZero = 0;
    
    if (Value == 0) return 0;
    else if (_BitScanForward64(&TrailingZero, Value))
        return TrailingZero;
    else
        return 32;
}