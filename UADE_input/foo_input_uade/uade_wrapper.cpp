/*
 * uade_wrapper.cpp
 * Platform-specific wrapper functions for UADE on Windows.
 * Provides IPC (via semaphores), stdio emulation for data file loading,
 * and various Unix compatibility functions.
 *
 * This file must NOT include UADE headers that redefine fopen/fread/etc.
 * (sysdeps.h, unixsupport.h) to avoid macro conflicts.
 */

#include <windows.h>
#include <io.h>
#include <string>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdarg>
#include <cassert>
#include <filesystem>
#include <atomic>

extern "C" {
#include <uade/uadeipc.h>
#include <uade/unixatomic.h>
}

// ---------------------------------------------------------------------------
// Global data path (set by component init, read by stdio emulation)
// ---------------------------------------------------------------------------
static std::string s_uade_data_path;

extern "C" void fb2k_uade_set_data_path(const char* path)
{
    s_uade_data_path = path ? path : "";
}

extern "C" const char* fb2k_uade_get_data_path()
{
    return s_uade_data_path.c_str();
}

// ---------------------------------------------------------------------------
// Debug log callback (set by component init to write to fb2k console)
// ---------------------------------------------------------------------------
static void (*s_log_callback)(const char*) = nullptr;

extern "C" void fb2k_uade_set_log_callback(void (*cb)(const char*))
{
    s_log_callback = cb;
}

static void dbg_log(const char* fmt, ...)
{
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (s_log_callback)
        s_log_callback(buf);
    OutputDebugStringA(buf);
}

// ---------------------------------------------------------------------------
// Simple file stream wrapper (replaces rePlayer's io::Stream)
// Used for stdio emulation - maps FILE* to a memory buffer or disk file.
// ---------------------------------------------------------------------------
struct FileStream {
    FILE* realFile;     // non-null if backed by a real file
    uint8_t* data;      // non-null if backed by memory
    size_t size;
    size_t pos;
    bool isMemory;
};

// ---------------------------------------------------------------------------
// uade_filesize
// ---------------------------------------------------------------------------
extern "C" int uade_filesize(size_t* size, const char* pathname)
{
    (void)size; (void)pathname;
    return -1;
}

// ---------------------------------------------------------------------------
// uade_dirname
// ---------------------------------------------------------------------------
extern "C" char* uade_dirname(char* dst, char* src, size_t maxlen)
{
    std::filesystem::path dir(src);
    dir.remove_filename();
    auto dirName = dir.string();
    size_t copyLen = dirName.size() + 1;
    if (copyLen > maxlen) copyLen = maxlen;
    memcpy(dst, dirName.c_str(), copyLen);
    return dst;
}

// ---------------------------------------------------------------------------
// uade_find_amiga_file (not used - we use the amiga loader callback)
// ---------------------------------------------------------------------------
extern "C" int uade_find_amiga_file(char*, size_t, const char*, const char*)
{
    assert(0 && "uade_find_amiga_file should not be called");
    return -1;
}

// ---------------------------------------------------------------------------
// IPC mechanism using Windows semaphores
// (Replaces Unix pipes for communication between main thread and uadecore)
// ---------------------------------------------------------------------------
struct IpcSocket {
    CRITICAL_SECTION cs;
    HANDLE semaphore;
    uint64_t messageOffset;
    uint8_t* message;
    size_t messageSize;
    size_t messageCapacity;
};

static IpcSocket s_sockets[2];
static std::atomic<bool> s_isClosed{false};

static void ipc_socket_init(IpcSocket* s)
{
    InitializeCriticalSection(&s->cs);
    s->semaphore = CreateSemaphoreW(nullptr, 0, 1, nullptr);
    s->messageOffset = 0;
    s->message = nullptr;
    s->messageSize = 0;
    s->messageCapacity = 0;
}

static void ipc_socket_cleanup(IpcSocket* s)
{
    if (s->semaphore) {
        CloseHandle(s->semaphore);
        s->semaphore = nullptr;
    }
    DeleteCriticalSection(&s->cs);
    free(s->message);
    s->message = nullptr;
    s->messageSize = 0;
    s->messageCapacity = 0;
    s->messageOffset = 0;
}

// ---------------------------------------------------------------------------
// uade_atomic_close / uade_atomic_read / uade_atomic_write
// ---------------------------------------------------------------------------
extern "C" int uade_atomic_close(int fd)
{
    while (1) {
        if (close(fd) < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        break;
    }
    return 0;
}

extern "C" ssize_t uade_atomic_read(int fd, const void* buf, size_t count)
{
    assert((fd & 0xFFF0) == 0x7770);
    if (s_isClosed.load())
        return 0;

    IpcSocket* sock = &s_sockets[fd & 1];
    uint8_t* dst = (uint8_t*)buf;
    size_t bytes_read = 0;

    while (bytes_read < count) {
        WaitForSingleObject(sock->semaphore, INFINITE);
        if (s_isClosed.load())
            return 0;

        EnterCriticalSection(&sock->cs);

        uint8_t* src = sock->message + sock->messageOffset;
        size_t avail = sock->messageSize - (size_t)sock->messageOffset;
        size_t toRead = count - bytes_read;
        if (toRead > avail) toRead = avail;
        memcpy(dst + bytes_read, src, toRead);

        bytes_read += toRead;
        sock->messageOffset += toRead;
        if ((size_t)sock->messageOffset == sock->messageSize) {
            sock->messageSize = 0;
            sock->messageOffset = 0;
        } else {
            ReleaseSemaphore(sock->semaphore, 1, nullptr);
        }

        LeaveCriticalSection(&sock->cs);
    }
    return (ssize_t)bytes_read;
}

extern "C" ssize_t uade_atomic_write(int fd, const void* buf, size_t count)
{
    assert((fd & 0xFFF0) == 0x7770);
    if (s_isClosed.load())
        return 0;

    IpcSocket* sock = &s_sockets[fd & 1];
    EnterCriticalSection(&sock->cs);

    size_t newSize = sock->messageSize + count;
    if (newSize > sock->messageCapacity) {
        size_t newCap = newSize * 2;
        if (newCap < 4096) newCap = 4096;
        sock->message = (uint8_t*)realloc(sock->message, newCap);
        sock->messageCapacity = newCap;
    }
    memcpy(sock->message + sock->messageSize, buf, count);
    sock->messageSize = newSize;

    LeaveCriticalSection(&sock->cs);
    ReleaseSemaphore(sock->semaphore, 1, nullptr);

    return (ssize_t)count;
}

// ---------------------------------------------------------------------------
// uade_arch_spawn / uade_arch_kill_and_wait_uadecore
// ---------------------------------------------------------------------------
extern "C" int in_m68k_go;
extern "C" int quit_program;
extern "C" int uadecore_reboot;
extern "C" int uadecore_main(int argc, char** argv);

static DWORD WINAPI uadecore_thread_func(LPVOID)
{
    const char* args[] = { "uadecore", "-i", "30577", "-o", "30576" };
    uadecore_main(5, (char**)args);
    return 0;
}

extern "C" int uade_arch_spawn(struct uade_ipc* ipc, void** userdata, const char*)
{
    s_isClosed.store(false);

    // Reset global emulator state left over from any previous cycle.
    // Without this, the new thread's m68k_go() loop exits immediately
    // because quit_program is still 1 from the previous uade_stop().
    quit_program = 0;
    in_m68k_go = 0;
    uadecore_reboot = 0;

    ipc_socket_init(&s_sockets[0]);
    ipc_socket_init(&s_sockets[1]);

    HANDLE hThread = CreateThread(nullptr, 0, uadecore_thread_func, nullptr, 0, nullptr);
    *userdata = (void*)hThread;

    uade_set_peer(ipc, 1, 0x7770, 0x7771);
    return 0;
}

extern "C" void uade_arch_kill_and_wait_uadecore(struct uade_ipc*, void** userdata)
{
    while (in_m68k_go == 0)
        Sleep(1);

    s_isClosed.store(true);
    ReleaseSemaphore(s_sockets[0].semaphore, 1, nullptr);
    ReleaseSemaphore(s_sockets[1].semaphore, 1, nullptr);

    WaitForSingleObject((HANDLE)*userdata, INFINITE);
    CloseHandle((HANDLE)*userdata);

    ipc_socket_cleanup(&s_sockets[0]);
    ipc_socket_cleanup(&s_sockets[1]);
}

// ---------------------------------------------------------------------------
// stdio emulation: stdioemu_* functions
// Called by UADE core code (via sysdeps.h #define fopen -> stdioemu_fopen)
// ---------------------------------------------------------------------------

static FILE* open_data_file(const char* filename, const char* mode)
{
    const char* fixed = filename;
    while (*fixed == '/')
        fixed++;

    if (!s_uade_data_path.empty()) {
        auto p = std::filesystem::path(s_uade_data_path) / fixed;
        const char* m = (mode && mode[0]) ? mode : "rb";
        FILE* f = _fsopen(p.string().c_str(), m, _SH_DENYNO);
        if (f)
            return f;
    } else {
        dbg_log("UADE fopen FAIL (no data path): %s\n", fixed);
    }
    return nullptr;
}

extern "C" FILE* stdioemu_fopen(const char* filename, const char* mode)
{
    return open_data_file(filename, mode);
}

extern "C" int stdioemu_fseek(FILE* stream, int offset, int origin)
{
    return fseek(stream, offset, origin);
}

extern "C" int stdioemu_fread(void* buffer, int size, int count, FILE* stream)
{
    return (int)fread(buffer, (size_t)size, (size_t)count, stream);
}

extern "C" int stdioemu_fwrite(const char*, int, int, FILE*)
{
    return 0;
}

extern "C" int stdioemu_ftell(FILE* stream)
{
    return (int)ftell(stream);
}

extern "C" int stdioemu_fclose(FILE* stream)
{
    if (stream) fclose(stream);
    return 0;
}

extern "C" char* stdioemu_fgets(char* str, int numChars, FILE* stream)
{
    return fgets(str, numChars, stream);
}

extern "C" int stdioemu_feof(FILE* stream)
{
    return feof(stream);
}

// ---------------------------------------------------------------------------
// uade_f* wrappers: called by UADE frontend code (compiled as C)
// (via unixsupport.h #define fopen -> uade_fopen)
// Must use extern "C" to match C linkage expected by the C object files.
// ---------------------------------------------------------------------------

extern "C" FILE* uade_fopen(const char* a, const char* b)
{
    return stdioemu_fopen(a, b);
}

extern "C" int uade_fseek(FILE* a, int b, int c)
{
    return stdioemu_fseek(a, b, c);
}

extern "C" int uade_fread(char* a, int b, int c, FILE* d)
{
    return stdioemu_fread(a, b, c, d);
}

extern "C" int uade_fwrite(const char* a, int b, int c, FILE* d)
{
    return stdioemu_fwrite(a, b, c, d);
}

extern "C" int uade_ftell(FILE* a)
{
    return stdioemu_ftell(a);
}

extern "C" int uade_fclose(FILE* a)
{
    return stdioemu_fclose(a);
}

extern "C" char* uade_fgets(char* a, int b, FILE* c)
{
    return stdioemu_fgets(a, b, c);
}

extern "C" int uade_feof(FILE* a)
{
    return stdioemu_feof(a);
}
