#include <foobar2000.h>
#include "cfg_vars.h"

#include <ksnd.h>

#include <vector>
#include <string>
#include <mutex>
#include <windows.h>

namespace {

static constexpr uint32_t kSampleRate = 44100;
static constexpr uint32_t kChannels = 2;
static std::mutex g_kt_mutex;

static std::wstring g_get_dll_dir() {
    wchar_t path[MAX_PATH] = {};
    HMODULE hMod = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
        reinterpret_cast<LPCWSTR>(&g_get_dll_dir), &hMod)) {
        return L"";
    }
    DWORD len = GetModuleFileNameW(hMod, path, _countof(path));
    if (len == 0 || len >= _countof(path)) return L"";
    for (size_t i = len; i > 0; --i) {
        if (path[i - 1] == L'\\' || path[i - 1] == L'/') {
            path[i - 1] = L'\0';
            break;
        }
    }
    return path;
}

class KsndApi {
public:
    using fnCreatePlayerUnregistered = KPlayer* (*)(int);
    using fnFreePlayer = void (*)(KPlayer*);
    using fnLoadSongFromMemory = KSong* (*)(KPlayer*, void*, int);
    using fnFreeSong = void (*)(KSong*);
    using fnPlaySong = void (*)(KPlayer*, KSong*, int);
    using fnStop = void (*)(KPlayer*);
    using fnFillBuffer = int (*)(KPlayer*, short int*, int);
    using fnGetSongInfo = const KSongInfo* (*)(KSong*, KSongInfo*);
    using fnGetSongLength = int (*)(const KSong*);
    using fnGetPlayTime = int (*)(KSong*, int);
    using fnSetLooping = void (*)(KPlayer*, int);

    bool ensure_loaded() {
        if (m_module) return true;

        std::wstring moduleDir = g_get_dll_dir();
        std::wstring pathLocal = moduleDir.empty() ? L"" : (moduleDir + L"\\ksnd.dll");
        std::wstring pathLocalAlt = moduleDir.empty() ? L"" : (moduleDir + L"\\libksnd.dll");

        if (!pathLocal.empty()) m_module = LoadLibraryW(pathLocal.c_str());
        if (!m_module && !pathLocalAlt.empty()) m_module = LoadLibraryW(pathLocalAlt.c_str());
        if (!m_module) m_module = LoadLibraryW(L"ksnd.dll");
        if (!m_module) m_module = LoadLibraryW(L"libksnd.dll");
        if (!m_module) return false;

        createPlayerUnregistered = reinterpret_cast<fnCreatePlayerUnregistered>(GetProcAddress(m_module, "KSND_CreatePlayerUnregistered"));
        freePlayer = reinterpret_cast<fnFreePlayer>(GetProcAddress(m_module, "KSND_FreePlayer"));
        loadSongFromMemory = reinterpret_cast<fnLoadSongFromMemory>(GetProcAddress(m_module, "KSND_LoadSongFromMemory"));
        freeSong = reinterpret_cast<fnFreeSong>(GetProcAddress(m_module, "KSND_FreeSong"));
        playSong = reinterpret_cast<fnPlaySong>(GetProcAddress(m_module, "KSND_PlaySong"));
        stop = reinterpret_cast<fnStop>(GetProcAddress(m_module, "KSND_Stop"));
        fillBuffer = reinterpret_cast<fnFillBuffer>(GetProcAddress(m_module, "KSND_FillBuffer"));
        getSongInfo = reinterpret_cast<fnGetSongInfo>(GetProcAddress(m_module, "KSND_GetSongInfo"));
        getSongLength = reinterpret_cast<fnGetSongLength>(GetProcAddress(m_module, "KSND_GetSongLength"));
        getPlayTime = reinterpret_cast<fnGetPlayTime>(GetProcAddress(m_module, "KSND_GetPlayTime"));
        setLooping = reinterpret_cast<fnSetLooping>(GetProcAddress(m_module, "KSND_SetLooping"));

        if (!createPlayerUnregistered || !freePlayer || !loadSongFromMemory || !freeSong ||
            !playSong || !stop || !fillBuffer || !getSongInfo || !getSongLength || !getPlayTime || !setLooping) {
            FreeLibrary(m_module);
            m_module = nullptr;
            return false;
        }
        return true;
    }

    ~KsndApi() {
        if (m_module) FreeLibrary(m_module);
    }

    fnCreatePlayerUnregistered createPlayerUnregistered = nullptr;
    fnFreePlayer freePlayer = nullptr;
    fnLoadSongFromMemory loadSongFromMemory = nullptr;
    fnFreeSong freeSong = nullptr;
    fnPlaySong playSong = nullptr;
    fnStop stop = nullptr;
    fnFillBuffer fillBuffer = nullptr;
    fnGetSongInfo getSongInfo = nullptr;
    fnGetSongLength getSongLength = nullptr;
    fnGetPlayTime getPlayTime = nullptr;
    fnSetLooping setLooping = nullptr;

private:
    HMODULE m_module = nullptr;
};

static KsndApi g_ksnd;

class input_klystrack;
static input_klystrack* g_active_instance = nullptr;

class input_klystrack : public input_stubs {
public:
    void open(service_ptr_t<file> p_filehint, const char* p_path,
        t_input_open_reason p_reason, abort_callback& p_abort) {
        if (p_reason == input_open_info_write) throw exception_tagging_unsupported();

        m_file = p_filehint;
        input_open_file_helper(m_file, p_path, p_reason, p_abort);

        t_filesize fileSize = m_file->get_size(p_abort);
        if (fileSize == filesize_invalid || fileSize < 8 || fileSize > 256 * 1024 * 1024) {
            throw exception_io_unsupported_format();
        }

        m_fileData.resize(static_cast<size_t>(fileSize));
        m_file->read_object(m_fileData.data(), m_fileData.size(), p_abort);
    }

    ~input_klystrack() {
        std::lock_guard<std::mutex> lock(g_kt_mutex);
        if (g_active_instance == this) {
            cleanup_decoder();
            g_active_instance = nullptr;
        }
    }

    unsigned get_subsong_count() { return 1; }
    t_uint32 get_subsong(unsigned) { return 0; }

    void get_info(t_uint32, file_info& p_info, abort_callback&) {
        if (m_durationSec > 0) p_info.set_length(m_durationSec);
        else {
            int dur = nostalgia_cfg::cfg_default_duration_sec.get();
            if (dur > 0) p_info.set_length(dur);
        }

        p_info.info_set_int("samplerate", kSampleRate);
        p_info.info_set_int("channels", kChannels);
        p_info.info_set_int("bitspersample", 16);
        p_info.info_set("encoding", "synthesized");
        p_info.info_set("codec", "Klystrack");

        if (!m_title.empty()) p_info.meta_set("title", m_title.c_str());
    }

    t_filestats2 get_stats2(unsigned f, abort_callback& a) { return m_file->get_stats2_(f, a); }
    t_filestats get_file_stats(abort_callback& a) { return m_file->get_stats(a); }

    void decode_initialize(t_uint32, unsigned, abort_callback&) {
        std::lock_guard<std::mutex> lock(g_kt_mutex);

        if (g_active_instance && g_active_instance != this) {
            g_active_instance->cleanup_decoder();
            g_active_instance->m_decoding = false;
        }
        g_active_instance = this;

        if (!g_ksnd.ensure_loaded()) throw exception_io_unsupported_format();

        m_player = g_ksnd.createPlayerUnregistered(kSampleRate);
        if (!m_player) throw exception_io_unsupported_format();

        m_song = g_ksnd.loadSongFromMemory(m_player, m_fileData.data(), static_cast<int>(m_fileData.size()));
        if (!m_song) {
            cleanup_decoder();
            throw exception_io_unsupported_format();
        }

        g_ksnd.setLooping(m_player, 1);
        g_ksnd.playSong(m_player, m_song, 0);
        m_renderedSamples = 0;
        m_decoding = true;

        KSongInfo info = {};
        const KSongInfo* pInfo = g_ksnd.getSongInfo(m_song, &info);
        m_title = (pInfo && pInfo->song_title) ? pInfo->song_title : "";

        int rows = g_ksnd.getSongLength(m_song);
        if (rows > 0) {
            int totalMs = g_ksnd.getPlayTime(m_song, rows);
            if (totalMs > 0) m_durationSec = static_cast<double>(totalMs) / 1000.0;
        }
    }

    bool decode_run(audio_chunk& p_chunk, abort_callback&) {
        if (!m_decoding) return false;

        std::lock_guard<std::mutex> lock(g_kt_mutex);
        if (g_active_instance != this || !m_player || !m_song) {
            m_decoding = false;
            return false;
        }

        static constexpr uint32_t kBlockFrames = 1024;
        short pcm16[kBlockFrames * kChannels] = {};
        int got = g_ksnd.fillBuffer(m_player, pcm16, sizeof(pcm16));
        if (got <= 0) {
            m_decoding = false;
            return false;
        }

        uint32_t frames = static_cast<uint32_t>(got);
        if (frames > kBlockFrames) {
            if ((frames % kChannels) == 0 && (frames / kChannels) <= kBlockFrames) frames /= kChannels;
            else frames = kBlockFrames;
        }

        m_convertBuffer.resize(static_cast<size_t>(frames) * kChannels);
        constexpr float kScale = 1.0f / 32768.0f;
        for (size_t i = 0; i < m_convertBuffer.size(); ++i) m_convertBuffer[i] = static_cast<float>(pcm16[i]) * kScale;

        m_renderedSamples += frames;
        p_chunk.set_data_32(m_convertBuffer.data(), frames, kChannels, kSampleRate);
        return true;
    }

    void decode_seek(double p_seconds, abort_callback& p_abort) {
        std::lock_guard<std::mutex> lock(g_kt_mutex);
        if (g_active_instance != this || !m_player || !m_song) return;

        g_ksnd.stop(m_player);
        g_ksnd.playSong(m_player, m_song, 0);
        m_renderedSamples = 0;
        m_decoding = true;

        const uint64_t targetSamples = static_cast<uint64_t>(p_seconds * kSampleRate);
        short discard[kChannels * 1024] = {};
        while (m_renderedSamples < targetSamples) {
            p_abort.check();
            const uint64_t remain = targetSamples - m_renderedSamples;
            const uint32_t thisFrames = static_cast<uint32_t>(pfc::min_t<uint64_t>(1024, remain));
            const int got = g_ksnd.fillBuffer(m_player, discard, static_cast<int>(thisFrames * kChannels * sizeof(short)));
            if (got <= 0) {
                m_decoding = false;
                break;
            }
            uint32_t frames = static_cast<uint32_t>(got);
            if (frames > thisFrames) {
                if ((frames % kChannels) == 0 && (frames / kChannels) <= thisFrames) frames /= kChannels;
                else frames = thisFrames;
            }
            m_renderedSamples += frames;
        }
    }
    bool decode_can_seek() { return true; }
    bool decode_get_dynamic_info(file_info&, double&) { return false; }
    bool decode_get_dynamic_info_track(file_info&, double&) { return false; }
    void decode_on_idle(abort_callback&) {}

    void retag_set_info(t_uint32, const file_info&, abort_callback&) { throw exception_tagging_unsupported(); }
    void retag_commit(abort_callback&) { throw exception_tagging_unsupported(); }
    void remove_tags(abort_callback&) { throw exception_tagging_unsupported(); }

    static bool g_is_our_content_type(const char*) { return false; }
    static bool g_is_our_path(const char*, const char* p_extension) {
        if (!nostalgia_cfg::cfg_klystrack_enabled.get()) return false;
        if (p_extension == nullptr) return false;
        return stricmp_utf8(p_extension, "kt") == 0;
    }
    static const char* g_get_name() { return "Klystrack decoder"; }
    static const GUID g_get_guid() {
        static const GUID guid = { 0x7f8e9d10, 0x2b3c, 0x4d5e, { 0x81, 0x92, 0xa3, 0xb4, 0xc5, 0xd6, 0xe7, 0xf8 } };
        return guid;
    }

private:
    void cleanup_decoder() {
        if (m_player) {
            g_ksnd.stop(m_player);
            if (m_song) {
                g_ksnd.freeSong(m_song);
                m_song = nullptr;
            }
            g_ksnd.freePlayer(m_player);
            m_player = nullptr;
        }
    }

    service_ptr_t<file> m_file;
    std::vector<uint8_t> m_fileData;
    std::vector<float> m_convertBuffer;
    KPlayer* m_player = nullptr;
    KSong* m_song = nullptr;
    std::string m_title;
    double m_durationSec = 0;
    uint64_t m_renderedSamples = 0;
    bool m_decoding = false;
};

} // anonymous namespace

static input_factory_t<input_klystrack> g_input_klystrack_factory;
