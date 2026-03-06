/*
 * foo_input_uade.cpp
 * foobar2000 input component for UADE (Unix Amiga Delitracker Emulator).
 * Plays Amiga music module formats using UADE 3.0.5.
 *
 * Architecture:
 *   - UADE core runs an M68K emulator in a separate thread (per state).
 *   - Only one uade_state can exist at a time (global emulator state).
 *   - A global critical section serializes all UADE operations.
 *   - The CS is held from decode_initialize() until cleanup_decode_state().
 *   - open() acquires the CS before probing.  If the CS is held by an
 *     active playback, open() sets a global stop flag.  The active
 *     decode_run() sees the flag, returns false, and foobar2000 tears
 *     down the decoder — releasing the CS almost immediately.  open()
 *     then acquires the CS and probes normally.
 *     Unknown files are rejected immediately (other inputs handle them).
 *   - decode_run() stops when the emulator signals song-end (hasended).
 *   - g_is_our_path() is very permissive: accepts known UADE extensions,
 *     Amiga-style prefixed filenames, extensionless files, and any
 *     unrecognized extension not in a blacklist. Actual format validation
 *     is done via UADE content detection in open() / decode_initialize().
 *   - Data files (players, score, eagleplayer.conf) are loaded from a
 *     'uade' directory next to the component DLL.
 */

#include "stdafx.h"

extern "C" {
#include <uade/unixatomic.h>
#include <uade/uade.h>
#include <uade/uadestate.h>
#include <include/uadectl.h>
}

#undef fopen
#undef fseek
#undef fread
#undef fwrite
#undef ftell
#undef fclose
#undef fgets
#undef feof

#include "resource.h"
#include <helpers/atl-misc.h>
#include <SDK/cfg_var.h>

// Stub for libPPUI symbol required by atl-misc.h helpers
PFC_NORETURN void WIN32_OP_FAIL() {
    throw std::runtime_error("Win32 operation failed");
}

#include <unordered_set>
#include <filesystem>
#include <algorithm>
#include <cctype>
#include <atomic>

// ---------------------------------------------------------------------------
// External functions from uade_wrapper.cpp
// ---------------------------------------------------------------------------
extern "C" void fb2k_uade_set_data_path(const char* path);
extern "C" const char* fb2k_uade_get_data_path();
extern "C" void fb2k_uade_set_log_callback(void (*cb)(const char*));

// ---------------------------------------------------------------------------
// Constants
// ---------------------------------------------------------------------------
static const unsigned UADE_NUM_CHANNELS = 2;
static const unsigned UADE_BITS_PER_SAMPLE = 16;
static const double   UADE_DEFAULT_DURATION = 300.0;

// ---------------------------------------------------------------------------
// Global state
// ---------------------------------------------------------------------------
static CRITICAL_SECTION g_uade_cs;
static std::atomic<bool> g_force_stop{false};  // signals active decode_run to stop
static std::unordered_set<std::string> g_uade_extensions;

struct FormatEntry {
    std::string prefix;
    std::string playerName;
};
static std::vector<FormatEntry> g_format_list;
static std::unordered_set<std::string> g_disabled_formats;

// ---------------------------------------------------------------------------
// Persistent configuration
// ---------------------------------------------------------------------------
// {3A7B2C1D-8E4F-5A6B-9C0D-1E2F3A4B5C6D}
static constexpr GUID guid_cfg_play_indefinitely = {
    0x3a7b2c1d, 0x8e4f, 0x5a6b,
    { 0x9c, 0x0d, 0x1e, 0x2f, 0x3a, 0x4b, 0x5c, 0x6d }
};
static cfg_bool g_cfg_play_indefinitely(guid_cfg_play_indefinitely, false);

// {D4E5F6A7-B8C9-0123-DEFA-B0C1D2E3F4A5}
static constexpr GUID guid_cfg_sample_rate = {
    0xd4e5f6a7, 0xb8c9, 0x0123,
    { 0xde, 0xfa, 0xb0, 0xc1, 0xd2, 0xe3, 0xf4, 0xa5 }
};
static cfg_int g_cfg_sample_rate(guid_cfg_sample_rate, 96000);

static unsigned get_sample_rate()
{
    int sr = g_cfg_sample_rate.get();
    if (sr < 8000 || sr > 96000) sr = 96000;
    return (unsigned)sr;
}

// Extensions that are definitely NOT Amiga music — skip UADE for these.
static const std::unordered_set<std::string> g_blacklisted_extensions = {
    "mp3", "mp4", "m4a", "m4b", "m4p", "aac", "ogg", "oga", "opus",
    "flac", "wav", "wave", "wma", "ape", "wv", "mpc", "tak", "tta",
    "dsd", "dsf", "dff", "aiff", "aif", "au", "snd", "caf",
    "mp1", "mp2", "ac3", "dts", "eac3", "mka", "webm",
    "mkv", "avi", "wmv", "mpg", "mpeg", "mp4v", "mov", "flv",
    "jpg", "jpeg", "png", "gif", "bmp", "tif", "tiff", "webp", "svg",
    "pdf", "txt", "nfo", "doc", "docx", "xls", "xlsx", "ppt", "rtf",
    "zip", "rar", "7z", "gz", "bz2", "xz", "tar", "cab", "lzma",
    "exe", "dll", "sys", "msi", "com", "bat", "cmd", "ps1", "vbs",
    "iso", "img", "vhd", "vmdk",
    "cue", "log", "m3u", "m3u8", "pls", "sfv", "md5",
    "htm", "html", "xml", "json", "yaml", "yml", "css", "js",
    "py", "c", "cpp", "h", "hpp", "java", "cs", "rb", "php",
    "ttf", "otf", "woff", "woff2", "eot",
    "srt", "sub", "ssa", "ass", "vtt",
    "lrc",
};

static std::string to_lower_ascii(const char* s, size_t len)
{
    std::string result(s, len);
    for (auto& c : result)
        if (c >= 'A' && c <= 'Z') c += ('a' - 'A');
    return result;
}
static std::string to_lower_ascii(const std::string& s)
{
    return to_lower_ascii(s.c_str(), s.size());
}

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static std::string find_uade_data_path()
{
    char dllPath[MAX_PATH] = {};
    HMODULE hMod = nullptr;
    GetModuleHandleExA(
        GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
        (LPCSTR)&find_uade_data_path,
        &hMod);
    GetModuleFileNameA(hMod, dllPath, MAX_PATH);

    std::filesystem::path base(dllPath);
    base = base.parent_path();

    // Check: <components>/uade/
    auto p1 = base / "uade";
    if (std::filesystem::exists(p1 / "eagleplayer.conf"))
        return p1.string();

    // Check: <fb2k_dir>/uade/ (parent of components/)
    auto p2 = base.parent_path() / "uade";
    if (std::filesystem::exists(p2 / "eagleplayer.conf"))
        return p2.string();

    return {};
}

static void load_extensions_from_conf(const std::string& dataPath)
{
    auto confPath = std::filesystem::path(dataPath) / "eagleplayer.conf";
    FILE* f = _fsopen(confPath.string().c_str(), "rb", _SH_DENYNO);
    if (!f) return;

    fseek(f, 0, SEEK_END);
    long fileSize = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (fileSize <= 0) { fclose(f); return; }

    std::vector<char> conf((size_t)fileSize + 1);
    fread(conf.data(), 1, (size_t)fileSize, f);
    conf[(size_t)fileSize] = '\0';
    fclose(f);

    const char* buf = conf.data();
    const char* fileEnd = buf + (size_t)fileSize;

    while (buf < fileEnd) {
        const char* lineEnd = buf;
        while (lineEnd < fileEnd && *lineEnd != '\n' && *lineEnd != '\r')
            lineEnd++;

        // Player name is the first token (before any tab)
        const char* nameEnd = buf;
        while (nameEnd < lineEnd && *nameEnd != '\t')
            nameEnd++;
        std::string playerName(buf, nameEnd);

        const char* prefixes = nullptr;
        for (const char* s = buf; s + 9 <= lineEnd; s++) {
            if (memcmp(s, "prefixes=", 9) == 0) {
                prefixes = s + 9;
                break;
            }
        }

        if (prefixes) {
            const char* p = prefixes;
            while (p < lineEnd && *p != '\t') {
                const char* extStart = p;
                while (p < lineEnd && *p != ',' && *p != '\t')
                    p++;
                if (p > extStart) {
                    std::string ext = to_lower_ascii(extStart, (size_t)(p - extStart));
                    g_uade_extensions.emplace(ext);
                    g_format_list.push_back({ ext, playerName });
                }
                if (p < lineEnd && *p == ',') p++;
            }
        }

        buf = lineEnd;
        while (buf < fileEnd && (*buf == '\n' || *buf == '\r'))
            buf++;
    }

    g_uade_extensions.emplace("tfm");
    g_uade_extensions.emplace("ymst");

    std::sort(g_format_list.begin(), g_format_list.end(),
        [](const FormatEntry& a, const FormatEntry& b) {
            return a.prefix < b.prefix;
        });
}

// {E5F6A7B8-C9D0-1234-EFAB-C0D1E2F3A4B5}
static constexpr GUID guid_cfg_disabled_players = {
    0xe5f6a7b8, 0xc9d0, 0x1234,
    { 0xef, 0xab, 0xc0, 0xd1, 0xe2, 0xf3, 0xa4, 0xb5 }
};
static cfg_string g_cfg_disabled_players(guid_cfg_disabled_players, "");

static void load_disabled_formats()
{
    pfc::string8 val = g_cfg_disabled_players.get();
    std::string s(val.get_ptr(), val.length());
    g_disabled_formats.clear();
    if (s.empty()) return;
    size_t pos = 0;
    while (pos < s.size()) {
        size_t next = s.find('|', pos);
        if (next == std::string::npos) next = s.size();
        if (next > pos)
            g_disabled_formats.insert(s.substr(pos, next - pos));
        pos = next + 1;
    }
}

static void save_disabled_formats()
{
    std::string s;
    for (auto& fmt : g_disabled_formats) {
        if (!s.empty()) s += '|';
        s += fmt;
    }
    g_cfg_disabled_players.set(s.c_str());
}

static bool validate_data_directory(const std::string& dataPath)
{
    bool ok = true;
    if (!std::filesystem::exists(std::filesystem::path(dataPath) / "score")) {
        FB2K_console_formatter()
            << "UADE: WARNING - 'score' file not found in " << dataPath.c_str()
            << "  (this is a compiled 68K binary required by the emulator)";
        ok = false;
    }
    if (!std::filesystem::is_directory(std::filesystem::path(dataPath) / "players")) {
        FB2K_console_formatter()
            << "UADE: WARNING - 'players/' directory not found in " << dataPath.c_str();
        ok = false;
    } else {
        namespace fs = std::filesystem;
        int count = 0;
        for (auto& e : fs::directory_iterator(fs::path(dataPath) / "players")) {
            (void)e;
            count++;
            if (count >= 5) break;
        }
        if (count == 0) {
            FB2K_console_formatter()
                << "UADE: WARNING - 'players/' directory is EMPTY in " << dataPath.c_str();
            ok = false;
        }
    }
    if (!std::filesystem::exists(std::filesystem::path(dataPath) / "eagleplayer.conf")) {
        FB2K_console_formatter()
            << "UADE: WARNING - 'eagleplayer.conf' not found in " << dataPath.c_str();
        ok = false;
    }
    return ok;
}

static struct uade_state* create_uade_state()
{
    struct uade_config* config = uade_new_config();
    uade_config_set_option(config, UC_ONE_SUBSONG, NULL);
    uade_config_set_option(config, UC_IGNORE_PLAYER_CHECK, NULL);
    char freqBuf[16];
    _snprintf_s(freqBuf, sizeof(freqBuf), _TRUNCATE, "%u", get_sample_rate());
    uade_config_set_option(config, UC_FREQUENCY, freqBuf);
    uade_config_set_option(config, UC_NO_PANNING, NULL);
    uade_config_set_option(config, UC_SUBSONG_TIMEOUT_VALUE, "-1");
    uade_config_set_option(config, UC_SILENCE_TIMEOUT_VALUE, "-1");
    uade_config_set_option(config, UC_BASE_DIR, "");

    struct uade_state* state = uade_new_state(config);
    free(config);
    return state;
}

// ---------------------------------------------------------------------------
// Amiga file loader callback
// ---------------------------------------------------------------------------
struct UadeContext {
    const uint8_t* buffer;
    size_t bufferSize;
    std::string filename;
    std::string directory;
};

static bool icase_ends_with(const std::string& haystack, const char* needle, size_t needleLen)
{
    if (needleLen > haystack.size()) return false;
    return _strnicmp(haystack.c_str() + haystack.size() - needleLen,
                     needle, needleLen) == 0;
}

static struct uade_file* amiga_loader_callback(
    const char* name, const char* playerdir, void* context, struct uade_state*)
{
    auto* ctx = reinterpret_cast<UadeContext*>(context);

    size_t nameLen = strlen(name);
    if (nameLen == 0) return nullptr;

    if (name[nameLen - 1] == '/') {
        struct uade_file* f = (struct uade_file*)calloc(1, sizeof(struct uade_file));
        f->name = strdup(name);
        f->data = (char*)malloc(1);
        f->size = 0;
        return f;
    }

    auto loadFile = [](const char* path, const char* originalName) -> struct uade_file* {
        FILE* fp = nullptr;

        // Resolve Unix-style absolute paths through the UADE data directory
        if (path[0] == '/') {
            std::string dp = fb2k_uade_get_data_path();
            if (!dp.empty()) {
                const char* rel = path;
                while (*rel == '/') rel++;
                auto resolved = std::filesystem::path(dp) / rel;
                fp = _fsopen(resolved.string().c_str(), "rb", _SH_DENYNO);
            }
        }

        // Fall back to trying the path as-is (Windows absolute or relative)
        if (!fp)
            fp = _fsopen(path, "rb", _SH_DENYNO);

        if (!fp) return nullptr;

        fseek(fp, 0, SEEK_END);
        long sz = ftell(fp);
        fseek(fp, 0, SEEK_SET);
        if (sz <= 0) { fclose(fp); return nullptr; }

        struct uade_file* f = (struct uade_file*)calloc(1, sizeof(struct uade_file));
        f->name = strdup(originalName);
        f->size = (size_t)sz;
        f->data = (char*)malloc(f->size);
        fread(f->data, 1, f->size, fp);
        fclose(fp);
        return f;
    };

    // Check if the module file itself is requested
    if (icase_ends_with(ctx->filename, name, nameLen) ||
        icase_ends_with(std::string(name, nameLen), ctx->filename.c_str(), ctx->filename.size()))
    {
        struct uade_file* f = (struct uade_file*)calloc(1, sizeof(struct uade_file));
        f->name = strdup(name);
        f->size = ctx->bufferSize;
        f->data = (char*)malloc(ctx->bufferSize);
        memcpy(f->data, ctx->buffer, ctx->bufferSize);
        return f;
    }

    // Handle Amiga ENV: prefix
    if (_strnicmp(name, "ENV:", 4) == 0 && nameLen > 4 && name[4] != '/' && name[4] != '\\') {
        std::string path(playerdir);
        path += name;
        path[strlen(playerdir) + 3] = '/';
        return loadFile(path.c_str(), name);
    }

    // Handle Amiga S: prefix
    if (_strnicmp(name, "S:", 2) == 0 && nameLen > 2 && name[2] != '/' && name[2] != '\\') {
        std::string path(playerdir);
        path += name;
        path[strlen(playerdir) + 1] = '/';
        return loadFile(path.c_str(), name);
    }

    // Try loading from the same directory as the module
    if (!ctx->directory.empty()) {
        auto p = std::filesystem::path(ctx->directory) / name;
        if (auto* f = loadFile(p.string().c_str(), name))
            return f;
    }

    // Try extras directory in data path
    std::string dataPath = fb2k_uade_get_data_path();
    if (!dataPath.empty()) {
        auto extrasPath = std::filesystem::path(dataPath) / "extras" / name;
        if (auto* f = loadFile(extrasPath.string().c_str(), name))
            return f;

        if (strchr(name, ' ')) {
            std::string fixedName(name);
            for (auto& c : fixedName)
                if (c == ' ') c = '_';
            auto p = std::filesystem::path(dataPath) / "extras" / fixedName;
            if (auto* f = loadFile(p.string().c_str(), name))
                return f;
        }
    }

    return nullptr;
}

// ---------------------------------------------------------------------------
// input_uade: foobar2000 input decoder
// ---------------------------------------------------------------------------
class input_uade : public input_stubs {
public:
    input_uade() {}

    ~input_uade()
    {
        cleanup_decode_state();
    }

    void open(service_ptr_t<file> p_filehint, const char* p_path,
              t_input_open_reason p_reason, abort_callback& p_abort)
    {
        if (p_reason == input_open_info_write)
            throw exception_tagging_unsupported();

        m_file = p_filehint;
        input_open_file_helper(m_file, p_path, p_reason, p_abort);

        t_filesize size = m_file->get_size(p_abort);
        if (size == filesize_invalid || size < 4 || size > 64 * 1024 * 1024)
            throw exception_io_unsupported_format();

        m_buffer.resize((size_t)size);
        m_file->read_object(m_buffer.data(), (t_size)size, p_abort);

        m_path = p_path;
        m_filename = std::string(p_path + pfc::scan_filename(p_path));

        {
            pfc::string8 nativePath;
            if (filesystem::g_get_native_path(p_path, nativePath)) {
                std::filesystem::path fp(nativePath.get_ptr());
                m_directory = fp.parent_path().string();
            }
        }

        // Content-based validation: probe the file with UADE.
        // UADE is single-instance, so probing requires exclusive access.
        // First try a quick non-blocking acquisition (covers the common
        // case when nothing is playing or only non-UADE tracks are active).
        {
            bool gotCS = false;
            for (int attempt = 0; attempt < 5; ++attempt) {
                if (TryEnterCriticalSection(&g_uade_cs)) {
                    gotCS = true;
                    break;
                }
                p_abort.check();
                Sleep(50);
            }

            if (!gotCS) {
                // CS is held by active UADE playback.
                // For unknown files, reject immediately so other input
                // components can try them without delay.
                if (!is_likely_uade_file(p_path))
                    throw exception_io_unsupported_format();

                // Signal the active decoder to stop.  If decode_run()
                // is active it will see the flag and return false.
                g_force_stop.store(true, std::memory_order_release);

                // If the decoder is paused, decode_run() isn't called so
                // g_force_stop alone won't release the CS. Trigger stop()
                // (directly on main thread or dispatched from worker thread)
                // so foobar2000 tears down the paused decoder.
                DWORD waitStart = GetTickCount();
                bool stopped = false;
                try {
                    while (!TryEnterCriticalSection(&g_uade_cs)) {
                        p_abort.check();
                        if (!stopped && (GetTickCount() - waitStart) > 300) {
                            if (core_api::is_main_thread()) {
                                try { playback_control::get()->stop(); }
                                catch (...) {}
                            } else {
                                fb2k::inMainThread([] {
                                    try { playback_control::get()->stop(); }
                                    catch (...) {}
                                });
                            }
                            stopped = true;
                        }
                        Sleep(10);
                    }
                } catch (...) {
                    g_force_stop.store(false, std::memory_order_relaxed);
                    throw;
                }
                gotCS = true;
                g_force_stop.store(false, std::memory_order_relaxed);
            }

            m_probed = probe_file();
            LeaveCriticalSection(&g_uade_cs);

            if (!m_probed)
                throw exception_io_unsupported_format();
        }
    }

    unsigned get_subsong_count()
    {
        if (!m_probed || m_subsongMax < m_subsongMin) return 1;
        return (unsigned)(m_subsongMax - m_subsongMin + 1);
    }

    t_uint32 get_subsong(unsigned p_index)
    {
        return (t_uint32)p_index;
    }

    void get_info(t_uint32 p_subsong, file_info& p_info, abort_callback&)
    {
        double duration = (m_uadeDuration > 0.0)
            ? m_uadeDuration : UADE_DEFAULT_DURATION;

        p_info.set_length(duration);

        p_info.info_set_int("samplerate", get_sample_rate());
        p_info.info_set_int("channels", UADE_NUM_CHANNELS);
        p_info.info_set_int("bitspersample", UADE_BITS_PER_SAMPLE);
        p_info.info_set("encoding", "synthesized");

        if (!m_playerName.empty())
            p_info.info_set("codec", m_playerName.c_str());

    }

    t_filestats2 get_stats2(uint32_t f, abort_callback& a)
    {
        return m_file->get_stats2_(f, a);
    }

    void decode_initialize(t_uint32 p_subsong, unsigned, abort_callback& p_abort)
    {
        cleanup_decode_state();

        EnterCriticalSection(&g_uade_cs);
        m_csHeld = true;

        m_state = create_uade_state();
        if (!m_state) {
            FB2K_console_formatter() << "UADE: Failed to create emulator state";
            LeaveCriticalSection(&g_uade_cs);
            m_csHeld = false;
            throw exception_io_data();
        }

        m_loaderCtx = {};
        m_loaderCtx.buffer = m_buffer.data();
        m_loaderCtx.bufferSize = m_buffer.size();
        m_loaderCtx.filename = m_filename;
        m_loaderCtx.directory = m_directory;
        uade_set_amiga_loader(amiga_loader_callback, &m_loaderCtx, m_state);

        int subsong;
        if (m_probed)
            subsong = m_subsongMin + (int)p_subsong;
        else
            subsong = -1;

        int rc = uade_play_from_buffer(
            m_filename.c_str(), m_buffer.data(), m_buffer.size(),
            subsong, m_state);

        if (rc != 1) {
            FB2K_console_formatter()
                << "UADE: Cannot play \"" << m_filename.c_str()
                << "\" (uade_play_from_buffer returned " << rc << ")";
            uade_cleanup_state(m_state);
            m_state = nullptr;
            LeaveCriticalSection(&g_uade_cs);
            m_csHeld = false;
            throw exception_io_unsupported_format();
        }

        if (!m_probed) {
            extract_song_info(m_state);
            m_probed = true;
        }

        if (is_format_disabled()) {
            uade_stop(m_state);
            uade_cleanup_state(m_state);
            m_state = nullptr;
            LeaveCriticalSection(&g_uade_cs);
            m_csHeld = false;
            throw exception_io_unsupported_format();
        }

        uade_set_filter_state(m_state, 0);
        uadecore_set_automatic_song_end(1);

        m_currentSubsong = (int)p_subsong;
        m_decoded_samples = 0;

        FB2K_console_formatter()
            << "UADE: playing \"" << m_filename.c_str()
            << "\" player=\"" << (m_playerName.empty() ? "?" : m_playerName.c_str())
            << "\" ext=\"" << (m_formatExt.empty() ? "?" : m_formatExt.c_str())
            << "\" ep=\"" << (m_epName.empty() ? "?" : m_epName.c_str()) << "\"";
    }

    bool decode_run(audio_chunk& p_chunk, abort_callback& p_abort)
    {
        if (!m_state) return false;

        p_abort.check();

        if (g_force_stop.load(std::memory_order_acquire))
            return false;

        if (!g_cfg_play_indefinitely) {
            if (m_state->hasended) {
                m_state->hasended = 0;
                return false;
            }
        } else {
            if (m_state->hasended)
                m_state->hasended = 0;
        }

        enum { DECODE_SAMPLES = 1024 };
        int16_t buf[DECODE_SAMPLES * UADE_NUM_CHANNELS];
        ssize_t bytes = uade_read(buf, sizeof(buf), m_state);

        if (bytes <= 0)
            return false;

        unsigned samples = (unsigned)bytes / (UADE_NUM_CHANNELS * sizeof(int16_t));
        m_decoded_samples += samples;

        p_chunk.set_data_fixedpoint(
            buf,
            (t_size)bytes,
            get_sample_rate(),
            UADE_NUM_CHANNELS,
            UADE_BITS_PER_SAMPLE,
            audio_chunk::channel_config_stereo);

        return true;
    }

    void decode_seek(double p_seconds, abort_callback& p_abort)
    {
        if (!m_state) return;

        if (p_seconds < 0.0) p_seconds = 0.0;

        // UADE's built-in seek is broken with UC_ONE_SUBSONG (backward seek
        // triggers UADE_EVENT_SONG_END which kills the seek loop).
        // Follow ReplayUADE's approach: stop + replay + fast-forward.
        uade_stop(m_state);

        m_loaderCtx = {};
        m_loaderCtx.buffer = m_buffer.data();
        m_loaderCtx.bufferSize = m_buffer.size();
        m_loaderCtx.filename = m_filename;
        m_loaderCtx.directory = m_directory;
        uade_set_amiga_loader(amiga_loader_callback, &m_loaderCtx, m_state);

        int subsong = m_probed ? (m_subsongMin + m_currentSubsong) : -1;
        int rc = uade_play_from_buffer(
            m_filename.c_str(), m_buffer.data(), m_buffer.size(),
            subsong, m_state);

        if (rc != 1) {
            m_state->hasended = 1;
            return;
        }

        uade_set_filter_state(m_state, 0);
        uadecore_set_automatic_song_end(1);
        m_state->hasended = 0;

        // Fast-forward to target position by reading and discarding samples
        uint64_t targetSamples = (uint64_t)(p_seconds * get_sample_rate());
        uint64_t samplesRead = 0;
        char tmp[4096];
        while (samplesRead < targetSamples) {
            p_abort.check();
            if (m_state->hasended) break;
            ssize_t bytes = uade_read(tmp, sizeof(tmp), m_state);
            if (bytes <= 0) break;
            samplesRead += (uint64_t)bytes / (UADE_NUM_CHANNELS * sizeof(int16_t));
        }

        m_state->hasended = 0;
        m_decoded_samples = targetSamples;
    }

    bool decode_can_seek() { return true; }

    void retag_set_info(t_uint32, const file_info&, abort_callback&)
    {
        throw exception_tagging_unsupported();
    }

    void retag_commit(abort_callback&)
    {
        throw exception_tagging_unsupported();
    }

    void remove_tags(abort_callback&)
    {
        throw exception_tagging_unsupported();
    }

    static bool g_is_our_content_type(const char*) { return false; }

    // Very permissive: accept any file not in the blacklist.
    // Real validation is content-based, done in open() via UADE probe.
    static bool g_is_our_path(const char* p_path, const char* p_extension)
    {
        // 1) Known UADE extension
        if (p_extension && *p_extension) {
            std::string ext = to_lower_ascii(p_extension, strlen(p_extension));

            // Reject if extension is a known UADE format but disabled
            if (g_uade_extensions.count(ext)) {
                if (!g_disabled_formats.empty() && g_disabled_formats.count(ext))
                    return false;
                return true;
            }

            // Blacklisted → always reject
            if (g_blacklisted_extensions.count(ext))
                return false;
        }

        // 2) Amiga-style prefix (e.g. "MOD.songname")
        const char* fn = p_path + pfc::scan_filename(p_path);
        const char* dot = strchr(fn, '.');
        if (dot && dot > fn) {
            std::string prefix = to_lower_ascii(fn, (size_t)(dot - fn));
            if (g_uade_extensions.count(prefix)) {
                if (!g_disabled_formats.empty() && g_disabled_formats.count(prefix))
                    return false;
                return true;
            }
        }

        // 3) Accept extensionless files and unknown extensions.
        //    The probe in open() will do the real validation.
        return true;
    }

    static const char* g_get_name() { return "UADE Amiga Music Decoder"; }

    static GUID g_get_guid()
    {
        static const GUID guid = {
            0xa3b1c2d4, 0xe5f6, 0x4890,
            { 0xab, 0xcd, 0xef, 0x01, 0x23, 0x45, 0x67, 0x89 }
        };
        return guid;
    }

private:
    // Check if file has a known UADE extension or prefix (quick, no CS)
    static bool is_likely_uade_file(const char* p_path)
    {
        const char* fn = p_path + pfc::scan_filename(p_path);

        // Check extension
        const char* lastDot = strrchr(fn, '.');
        if (lastDot && lastDot[1]) {
            std::string ext = to_lower_ascii(lastDot + 1, strlen(lastDot + 1));
            if (g_uade_extensions.count(ext))
                return true;
        }

        // Check prefix
        const char* firstDot = strchr(fn, '.');
        if (firstDot && firstDot > fn) {
            std::string prefix = to_lower_ascii(fn, (size_t)(firstDot - fn));
            if (g_uade_extensions.count(prefix))
                return true;
        }

        return false;
    }

    bool is_filename_disabled() const
    {
        if (g_disabled_formats.empty()) return false;
        const char* fn = m_filename.c_str();
        // Check Amiga-style prefix (e.g. "mod.songname")
        const char* firstDot = strchr(fn, '.');
        if (firstDot && firstDot > fn) {
            std::string prefix = to_lower_ascii(fn, (size_t)(firstDot - fn));
            if (g_disabled_formats.count(prefix))
                return true;
        }
        // Check PC-style extension (e.g. "song.mod")
        const char* lastDot = strrchr(fn, '.');
        if (lastDot && lastDot[1]) {
            std::string ext = to_lower_ascii(lastDot + 1, strlen(lastDot + 1));
            if (g_disabled_formats.count(ext))
                return true;
        }
        return false;
    }

    bool is_format_disabled()
    {
        if (g_disabled_formats.empty()) return false;

        // 1) Check detected format extension from UADE
        std::string extLower = to_lower_ascii(m_formatExt);
        if (!extLower.empty() && g_disabled_formats.count(extLower)) {
            FB2K_console_formatter()
                << "UADE: blocked \"" << m_filename.c_str()
                << "\" — detected format \"" << extLower.c_str() << "\" is disabled";
            return true;
        }

        // 2) Check all prefixes belonging to the same player
        if (!m_epName.empty()) {
            for (auto& fe : g_format_list) {
                if (fe.playerName == m_epName
                    && g_disabled_formats.count(fe.prefix)) {
                    FB2K_console_formatter()
                        << "UADE: blocked \"" << m_filename.c_str()
                        << "\" — player \"" << m_epName.c_str()
                        << "\" (prefix \"" << fe.prefix.c_str() << "\") is disabled";
                    return true;
                }
            }
        }

        // 3) Fallback: check filename prefix/extension
        if (is_filename_disabled()) {
            FB2K_console_formatter()
                << "UADE: blocked \"" << m_filename.c_str()
                << "\" — filename matches disabled format";
            return true;
        }

        return false;
    }

    void extract_song_info(struct uade_state* state)
    {
        const struct uade_song_info* info = uade_get_song_info(state);
        if (info) {
            m_subsongMin = info->subsongs.min;
            m_subsongMax = info->subsongs.max;
            m_playerName = info->playername[0]
                ? info->playername
                : (info->detectioninfo.custom ? "Custom" : "");
            m_formatExt = info->detectioninfo.ext;
            if (m_playerName.empty() && info->detectioninfo.ep)
                m_playerName = info->detectioninfo.ep->playername;
            // Store the eagleplayer.conf name for reliable matching
            if (info->detectioninfo.ep && info->detectioninfo.ep->playername)
                m_epName = info->detectioninfo.ep->playername;
            m_uadeDuration = (info->duration > 0.0) ? info->duration : 0.0;
        }
        if (m_subsongMin < 0 || m_subsongMin > 255) m_subsongMin = 0;
        if (m_subsongMax < 0 || m_subsongMax > 255) m_subsongMax = 0;
        if (m_subsongMax < m_subsongMin) m_subsongMax = m_subsongMin;
    }

    bool probe_file()
    {
        // Quick filename check before spinning up UADE
        if (is_filename_disabled()) {
            FB2K_console_formatter()
                << "UADE: rejected \"" << m_filename.c_str()
                << "\" — filename matches disabled format (early check)";
            return false;
        }

        struct uade_state* state = create_uade_state();
        if (!state) {
            FB2K_console_formatter() << "UADE: probe_file: failed to create state for \""
                                     << m_filename.c_str() << "\"";
            return false;
        }

        UadeContext ctx = {};
        ctx.buffer = m_buffer.data();
        ctx.bufferSize = m_buffer.size();
        ctx.filename = m_filename;
        ctx.directory = m_directory;
        uade_set_amiga_loader(amiga_loader_callback, &ctx, state);

        int rc = uade_play_from_buffer(
            m_filename.c_str(), m_buffer.data(), m_buffer.size(),
            -1, state);

        if (rc != 1) {
            uade_cleanup_state(state);
            return false;
        }

        extract_song_info(state);

        uade_stop(state);
        uade_cleanup_state(state);

        if (is_format_disabled())
            return false;

        return true;
    }

    void cleanup_decode_state()
    {
        if (m_state) {
            if (!m_csHeld) {
                EnterCriticalSection(&g_uade_cs);
                m_csHeld = true;
            }
            uade_stop(m_state);
            uade_cleanup_state(m_state);
            m_state = nullptr;
        }
        if (m_csHeld) {
            m_csHeld = false;
            LeaveCriticalSection(&g_uade_cs);
        }
    }

private:
    service_ptr_t<file> m_file;
    std::vector<uint8_t> m_buffer;
    std::string m_path;
    std::string m_filename;
    std::string m_directory;

    bool m_probed = false;
    int m_subsongMin = 0;
    int m_subsongMax = 0;
    std::string m_playerName;
    std::string m_formatExt;
    std::string m_epName;
    double m_uadeDuration = 0.0;

    struct uade_state* m_state = nullptr;
    bool m_csHeld = false;
    int m_currentSubsong = 0;
    uint64_t m_decoded_samples = 0;
    UadeContext m_loaderCtx = {};
};

// ---------------------------------------------------------------------------
// Factory, file type, component info
// ---------------------------------------------------------------------------
static input_factory_t<input_uade> g_input_uade_factory;

DECLARE_FILE_TYPE("Amiga Music files",
    "*.AHX;*.CUST;*.DM;*.DM2;*.DMF;*.EA;*.FC;*.FC13;*.FC14;"
    "*.FRED;*.GMC;*.HIP;*.HIPC;*.IMS;*.JAM;*.JMF;"
    "*.KH;*.MDAT;*.MED;*.MMDC;*.MOD;*.MON;*.OKTA;"
    "*.PSA;*.RK;*.SA;*.SID;*.SMOD;*.SNG;"
    "*.SNK;*.SOC;*.SMUS;*.SQT;*.THM;*.TFM;*.YMST");

DECLARE_COMPONENT_VERSION(
    "UADE Input",
    "1.0",
    "Unix Amiga Delitracker Emulator (UADE) 3.0.5 input plugin.\n"
    "Plays Amiga music formats using the UADE emulator core.\n\n"
    "Based on UADE by Heikki Orsila & Michael Doering.\n"
    "foobar2000 integration by foo_input_uade.\n\n"
    "Required: place a 'uade' data directory next to this DLL\n"
    "or in the foobar2000 installation directory. It must contain:\n"
    "  - eagleplayer.conf\n"
    "  - score           (compiled 68K binary)\n"
    "  - players/        (directory with player binaries)\n"
    "  - extras/         (optional companion files)\n"
);

VALIDATE_COMPONENT_FILENAME("foo_input_uade.dll");

// ---------------------------------------------------------------------------
// initquit: Initialize/cleanup global state
// ---------------------------------------------------------------------------
class initquit_uade : public initquit {
public:
    static void log_to_console(const char* msg)
    {
        FB2K_console_formatter() << msg;
    }

    void on_init() override
    {
        InitializeCriticalSection(&g_uade_cs);
        fb2k_uade_set_log_callback(log_to_console);

        std::string dataPath = find_uade_data_path();
        if (!dataPath.empty()) {
            fb2k_uade_set_data_path(dataPath.c_str());
            load_extensions_from_conf(dataPath);
            load_disabled_formats();

            FB2K_console_formatter() << "UADE: data path = " << dataPath.c_str();
            FB2K_console_formatter() << "UADE: " << (unsigned)g_uade_extensions.size()
                                     << " extensions, " << (unsigned)g_format_list.size()
                                     << " format entries loaded";
            if (!g_disabled_formats.empty()) {
                FB2K_console_formatter() << "UADE: " << (unsigned)g_disabled_formats.size()
                                         << " formats disabled";
            }

            validate_data_directory(dataPath);
        } else {
            FB2K_console_formatter()
                << "UADE: ERROR - data directory not found! "
                   "Place a 'uade' folder next to foo_input_uade.dll or "
                   "in the foobar2000 directory. It must contain: "
                   "eagleplayer.conf, score, players/";
        }
    }

    void on_quit() override
    {
        g_uade_extensions.clear();
        g_format_list.clear();
        g_disabled_formats.clear();
        DeleteCriticalSection(&g_uade_cs);
    }
};

static initquit_factory_t<initquit_uade> g_initquit_uade;

// ---------------------------------------------------------------------------
// UADE Decoders preferences page
// ---------------------------------------------------------------------------

class CUADEDecoders : public CDialogImpl<CUADEDecoders>,
                      public preferences_page_instance {
public:
    enum { IDD = IDD_UADE_DECODERS };

    CUADEDecoders(preferences_page_callback::ptr callback)
        : m_callback(callback) {}

    t_uint32 get_state() override {
        t_uint32 state = preferences_state::resettable;
        if (m_dirty) state |= preferences_state::changed;
        return state;
    }
    void apply() override;
    void reset() override;

    BEGIN_MSG_MAP_EX(CUADEDecoders)
        MSG_WM_INITDIALOG(OnInitDialog)
        NOTIFY_HANDLER_EX(IDC_DECODER_LIST, LVN_ITEMCHANGED, OnItemChanged)
        COMMAND_HANDLER_EX(IDC_BTN_ENABLE_ALL, BN_CLICKED, OnEnableAll)
        COMMAND_HANDLER_EX(IDC_BTN_DISABLE_ALL, BN_CLICKED, OnDisableAll)
        COMMAND_HANDLER_EX(IDC_SAMPLERATE_COMBO, CBN_SELCHANGE, OnSampleRateChanged)
        COMMAND_HANDLER_EX(IDC_PLAY_INDEFINITELY, BN_CLICKED, OnCheckChanged)
    END_MSG_MAP()

private:
    BOOL OnInitDialog(CWindow, LPARAM);
    LRESULT OnItemChanged(LPNMHDR pnmh);
    void OnEnableAll(UINT, int, CWindow);
    void OnDisableAll(UINT, int, CWindow);
    void OnSampleRateChanged(UINT, int, CWindow) { m_dirty = true; m_callback->on_state_changed(); }
    void OnCheckChanged(UINT, int, CWindow) { m_dirty = true; m_callback->on_state_changed(); }
    void PopulateList();

    static const int s_sampleRates[];
    static const int s_numSampleRates;

    const preferences_page_callback::ptr m_callback;
    CListViewCtrl m_list;
    CComboBox m_srCombo;
    bool m_dirty = false;
    bool m_populating = false;
};

const int CUADEDecoders::s_sampleRates[] = {
    8000, 11025, 16000, 22050, 24000, 32000, 44100, 48000, 64000, 88200, 96000
};
const int CUADEDecoders::s_numSampleRates =
    sizeof(CUADEDecoders::s_sampleRates) / sizeof(CUADEDecoders::s_sampleRates[0]);

BOOL CUADEDecoders::OnInitDialog(CWindow, LPARAM)
{
    CheckDlgButton(IDC_PLAY_INDEFINITELY,
                   g_cfg_play_indefinitely ? BST_CHECKED : BST_UNCHECKED);

    m_srCombo = GetDlgItem(IDC_SAMPLERATE_COMBO);
    int currentSR = g_cfg_sample_rate.get();
    int selIdx = -1;
    for (int i = 0; i < s_numSampleRates; i++) {
        TCHAR buf[16];
        _stprintf_s(buf, _T("%d"), s_sampleRates[i]);
        m_srCombo.AddString(buf);
        if (s_sampleRates[i] == currentSR) selIdx = i;
    }
    if (selIdx < 0) selIdx = s_numSampleRates - 1;
    m_srCombo.SetCurSel(selIdx);

    m_list = GetDlgItem(IDC_DECODER_LIST);
    m_list.SetExtendedListViewStyle(
        LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES | LVS_EX_CHECKBOXES);
    m_list.AddColumn(_T("Format"), 0);
    m_list.AddColumn(_T("Player"), 1);
    m_list.SetColumnWidth(0, 100);
    m_list.SetColumnWidth(1, 210);

    PopulateList();
    return FALSE;
}

void CUADEDecoders::PopulateList()
{
    m_populating = true;
    m_list.DeleteAllItems();
    for (int i = 0; i < (int)g_format_list.size(); i++) {
        auto& fe = g_format_list[i];
        CA2T prefW(fe.prefix.c_str());
        m_list.AddItem(i, 0, prefW);
        CA2T nameW(fe.playerName.c_str());
        m_list.SetItemText(i, 1, nameW);
        bool enabled = (g_disabled_formats.count(fe.prefix) == 0);
        m_list.SetCheckState(i, enabled ? TRUE : FALSE);
    }
    m_populating = false;
}

LRESULT CUADEDecoders::OnItemChanged(LPNMHDR pnmh)
{
    if (m_populating) return 0;
    LPNMLISTVIEW nlv = (LPNMLISTVIEW)pnmh;
    if ((nlv->uChanged & LVIF_STATE) &&
        ((nlv->uNewState ^ nlv->uOldState) & LVIS_STATEIMAGEMASK)) {
        m_dirty = true;
        m_callback->on_state_changed();
    }
    return 0;
}

void CUADEDecoders::OnEnableAll(UINT, int, CWindow)
{
    m_populating = true;
    for (int i = 0; i < m_list.GetItemCount(); i++)
        m_list.SetCheckState(i, TRUE);
    m_populating = false;
    m_dirty = true;
    m_callback->on_state_changed();
}

void CUADEDecoders::OnDisableAll(UINT, int, CWindow)
{
    m_populating = true;
    for (int i = 0; i < m_list.GetItemCount(); i++)
        m_list.SetCheckState(i, FALSE);
    m_populating = false;
    m_dirty = true;
    m_callback->on_state_changed();
}

void CUADEDecoders::apply()
{
    g_cfg_play_indefinitely =
        (IsDlgButtonChecked(IDC_PLAY_INDEFINITELY) == BST_CHECKED);

    int sel = m_srCombo.GetCurSel();
    if (sel >= 0 && sel < s_numSampleRates)
        g_cfg_sample_rate = s_sampleRates[sel];

    g_disabled_formats.clear();
    for (int i = 0; i < m_list.GetItemCount(); i++) {
        if (!m_list.GetCheckState(i) && i < (int)g_format_list.size())
            g_disabled_formats.insert(g_format_list[i].prefix);
    }
    save_disabled_formats();
    if (!g_disabled_formats.empty()) {
        std::string list;
        for (auto& f : g_disabled_formats) {
            if (!list.empty()) list += ", ";
            list += f;
        }
        FB2K_console_formatter()
            << "UADE: disabled formats: " << list.c_str();
    } else {
        FB2K_console_formatter() << "UADE: all formats enabled";
    }
    m_dirty = false;
    m_callback->on_state_changed();
}

void CUADEDecoders::reset()
{
    CheckDlgButton(IDC_PLAY_INDEFINITELY, BST_UNCHECKED);
    m_srCombo.SetCurSel(s_numSampleRates - 1);

    m_populating = true;
    for (int i = 0; i < m_list.GetItemCount(); i++)
        m_list.SetCheckState(i, TRUE);
    m_populating = false;
    m_dirty = true;
    m_callback->on_state_changed();
}

// ---------------------------------------------------------------------------
// Preferences page registration
// ---------------------------------------------------------------------------

// {A1B2C3D4-E5F6-7890-ABCD-EF0123456789}
static constexpr GUID guid_uade_decoders_page = {
    0xa1b2c3d4, 0xe5f6, 0x7890,
    { 0xab, 0xcd, 0xef, 0x01, 0x23, 0x45, 0x67, 0x89 }
};

class preferences_page_uade_decoders
    : public preferences_page_impl<CUADEDecoders> {
public:
    const char* get_name() override { return "UADE"; }
    GUID get_guid() override { return guid_uade_decoders_page; }
    GUID get_parent_guid() override { return preferences_page::guid_input; }
};

static preferences_page_factory_t<preferences_page_uade_decoders>
    g_pref_uade_decoders_factory;
