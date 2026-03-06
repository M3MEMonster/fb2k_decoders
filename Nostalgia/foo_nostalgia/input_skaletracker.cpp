#include <foobar2000.h>
#include "cfg_vars.h"
#include <SkalePlayer.h>

#include <vector>
#include <string>
#include <cstring>
#include <mutex>
#include <windows.h>

namespace {

static constexpr uint32_t kSkaleSampleRate = 44100;
static std::mutex g_skale_mutex;

static std::wstring g_utf8_to_wide(const char* str) {
    if (!str || !str[0]) return L"";
    int wlen = MultiByteToWideChar(CP_UTF8, 0, str, -1, nullptr, 0);
    if (wlen <= 0) return L"";
    std::wstring ws((size_t)wlen - 1, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, str, -1, ws.data(), wlen);
    return ws;
}

static bool g_read_header(const char* path, uint8_t* out, size_t bytes) {
    if (path == nullptr || out == nullptr || bytes == 0) return false;
    const char* nativePath = path;
    if (pfc::strcmp_partial(path, "file://") == 0) nativePath = path + 7;

    FILE* fp = nullptr;
    std::wstring wpath = g_utf8_to_wide(nativePath);
    if (!wpath.empty()) fp = _wfopen(wpath.c_str(), L"rb");
    if (!fp) fp = fopen(nativePath, "rb");
    if (!fp) return false;

    size_t got = fread(out, 1, bytes, fp);
    fclose(fp);
    return got == bytes;
}

class input_skaletracker;
static input_skaletracker* g_skale_activeInstance = nullptr;

class SkaleEventHandler : public ISkalePlayer::IEventHandler {
public:
    bool hasEnded = false;
    void Event(EEvent eEvent, const TEventInfo*) override {
        if (eEvent == EVENT_ENDOFSONG)
            hasEnded = true;
    }
};

class input_skaletracker : public input_stubs {
public:
    void open(service_ptr_t<file> p_filehint, const char* p_path,
              t_input_open_reason p_reason, abort_callback& p_abort)
    {
        if (p_reason == input_open_info_write)
            throw exception_tagging_unsupported();

        m_file = p_filehint;
        input_open_file_helper(m_file, p_path, p_reason, p_abort);

        t_filesize fileSize = m_file->get_size(p_abort);
        if (fileSize == filesize_invalid || fileSize < 13 || fileSize > 256 * 1024 * 1024)
            throw exception_io_unsupported_format();

        m_fileData.resize(static_cast<size_t>(fileSize));
        m_file->read_object(m_fileData.data(), m_fileData.size(), p_abort);

        const uint8_t magic[] = { 1, 0, 0xfe, 0xff, 9, 0, 0, 0, 0x41, 0x4c, 0x49, 0x4d, 0x33 };
        if (memcmp(m_fileData.data(), magic, sizeof(magic)) != 0)
            throw exception_io_unsupported_format();
    }

    ~input_skaletracker()
    {
        std::lock_guard<std::mutex> lock(g_skale_mutex);
        if (g_skale_activeInstance == this)
        {
            if (m_player)
            {
                m_player->SetPlayMode(ISkalePlayer::PLAY_MODE_STOP_ENGINE);
                if (m_song)
                    m_player->FreeSong(m_song);
                m_player->End();
            }
            g_skale_activeInstance = nullptr;
        }
    }

    unsigned get_subsong_count() { return 1; }
    t_uint32 get_subsong(unsigned) { return 0; }

    void get_info(t_uint32, file_info& p_info, abort_callback&)
    {
        int dur = nostalgia_cfg::cfg_default_duration_sec.get();
        if (dur > 0) p_info.set_length(dur);

        p_info.info_set_int("samplerate", kSkaleSampleRate);
        p_info.info_set_int("channels", 2);
        p_info.info_set_int("bitspersample", 32);
        p_info.info_set("encoding", "synthesized");
        p_info.info_set("codec", "Skale Tracker");

        if (!m_title.empty())
            p_info.meta_set("title", m_title.c_str());
    }

    t_filestats2 get_stats2(unsigned f, abort_callback& a) { return m_file->get_stats2_(f, a); }
    t_filestats get_file_stats(abort_callback& a) { return m_file->get_stats(a); }

    void decode_initialize(t_uint32, unsigned, abort_callback&)
    {
        std::lock_guard<std::mutex> lock(g_skale_mutex);

        if (g_skale_activeInstance && g_skale_activeInstance != this)
        {
            if (g_skale_activeInstance->m_player)
            {
                g_skale_activeInstance->m_player->SetPlayMode(ISkalePlayer::PLAY_MODE_STOP_ENGINE);
                if (g_skale_activeInstance->m_song)
                    g_skale_activeInstance->m_player->FreeSong(g_skale_activeInstance->m_song);
                g_skale_activeInstance->m_player->End();
                g_skale_activeInstance->m_player = nullptr;
                g_skale_activeInstance->m_song = nullptr;
            }
            g_skale_activeInstance->m_decoding = false;
        }
        g_skale_activeInstance = this;

        m_player = ISkalePlayer::GetSkalePlayer();
        if (!m_player)
        {
            g_skale_activeInstance = nullptr;
            throw exception_io_unsupported_format();
        }

        ISkalePlayer::TInitData initData;
        initData.m_eMode = ISkalePlayer::INIT_MODE_SLAVE;
        initData.m_iSamplesPerSecond = kSkaleSampleRate;
        if (m_player->Init(initData) != ISkalePlayer::INIT_OK)
        {
            m_player = nullptr;
            g_skale_activeInstance = nullptr;
            throw exception_io_unsupported_format();
        }

        m_song = m_player->LoadSongFromMemory(m_fileData.data(), (int)m_fileData.size());
        if (!m_song)
        {
            m_player->End();
            m_player = nullptr;
            g_skale_activeInstance = nullptr;
            throw exception_io_unsupported_format();
        }

        m_player->SetCurrentSong(m_song);
        m_player->SetEventHandler(&m_eventHandler);

        ISkalePlayer::ISong::TInfo songInfo;
        m_song->GetInfo(&songInfo);
        m_title = songInfo.m_szTitle;

        m_eventHandler.hasEnded = false;
        m_player->SetPlayMode(ISkalePlayer::PLAY_MODE_PLAY_SONG_FROM_START);
        m_decoding = true;
        m_totalRendered = 0;
    }

    bool decode_run(audio_chunk& p_chunk, abort_callback&)
    {
        if (!m_decoding) return false;

        std::lock_guard<std::mutex> lock(g_skale_mutex);
        if (g_skale_activeInstance != this) { m_decoding = false; return false; }

        if (m_eventHandler.hasEnded)
        {
            m_decoding = false;
            return false;
        }

        static constexpr uint32_t kBlockSize = 1024;
        float buffer[kBlockSize * 2];

        m_player->SlaveProcess(buffer, kBlockSize);

        m_totalRendered += kBlockSize;
        uint64_t maxFrames = (uint64_t)kSkaleSampleRate * nostalgia_cfg::cfg_default_duration_sec.get();
        if (maxFrames > 0 && m_totalRendered > maxFrames)
        {
            m_decoding = false;
            return false;
        }

        p_chunk.set_data_32(buffer, kBlockSize, 2, kSkaleSampleRate);
        return true;
    }

    void decode_seek(double p_seconds, abort_callback& p_abort)
    {
        std::lock_guard<std::mutex> lock(g_skale_mutex);
        if (g_skale_activeInstance != this) return;

        m_player->SetPlayMode(ISkalePlayer::PLAY_MODE_STOP_ENGINE);
        if (m_song) m_player->FreeSong(m_song);
        m_player->End();

        m_player = ISkalePlayer::GetSkalePlayer();
        if (!m_player) { m_decoding = false; return; }

        ISkalePlayer::TInitData initData;
        initData.m_eMode = ISkalePlayer::INIT_MODE_SLAVE;
        initData.m_iSamplesPerSecond = kSkaleSampleRate;
        if (m_player->Init(initData) != ISkalePlayer::INIT_OK) { m_player = nullptr; m_decoding = false; return; }

        m_song = m_player->LoadSongFromMemory(m_fileData.data(), (int)m_fileData.size());
        if (!m_song) { m_player->End(); m_player = nullptr; m_decoding = false; return; }

        m_player->SetCurrentSong(m_song);
        m_player->SetEventHandler(&m_eventHandler);
        m_eventHandler.hasEnded = false;
        m_player->SetPlayMode(ISkalePlayer::PLAY_MODE_PLAY_SONG_FROM_START);
        m_totalRendered = 0;
        m_decoding = true;

        uint64_t targetSamples = (uint64_t)(p_seconds * kSkaleSampleRate);
        float discardBuf[1024 * 2];
        while (m_totalRendered < targetSamples)
        {
            p_abort.check();
            if (m_eventHandler.hasEnded) break;
            uint32_t toProcess = 1024;
            uint64_t left = targetSamples - m_totalRendered;
            if (toProcess > left) toProcess = (uint32_t)left;
            m_player->SlaveProcess(discardBuf, toProcess);
            m_totalRendered += toProcess;
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
    static bool g_is_our_path(const char* p_path, const char* p_extension) {
        if (!nostalgia_cfg::cfg_skaletracker_enabled.get()) return false;
        if (p_extension == nullptr || stricmp_utf8(p_extension, "skm") != 0) return false;

        const uint8_t magic[] = { 1, 0, 0xfe, 0xff, 9, 0, 0, 0, 0x41, 0x4c, 0x49, 0x4d, 0x33 };
        uint8_t hdr[sizeof(magic)] = {};
        if (!g_read_header(p_path, hdr, sizeof(hdr))) return true;
        return memcmp(hdr, magic, sizeof(magic)) == 0;
    }
    static const char* g_get_name() { return "Skale Tracker decoder"; }
    static const GUID g_get_guid() {
        static const GUID guid = { 0xdd44ee55, 0xff66, 0x4007, { 0x11, 0x88, 0x22, 0xaa, 0x33, 0xbb, 0x44, 0xcc } };
        return guid;
    }

private:
    service_ptr_t<file> m_file;
    std::vector<uint8_t> m_fileData;
    ISkalePlayer* m_player = nullptr;
    const ISkalePlayer::ISong* m_song = nullptr;
    SkaleEventHandler m_eventHandler;
    std::string m_title;
    uint64_t m_totalRendered = 0;
    bool m_decoding = false;
};

} // anonymous namespace

static input_factory_t<input_skaletracker> g_input_skaletracker_factory;
