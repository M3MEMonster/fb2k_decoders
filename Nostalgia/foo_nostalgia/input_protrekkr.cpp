#include <foobar2000.h>
#include "cfg_vars.h"

#include <include/version.h>
#include <replay/include/replay.h>
#include <files/include/files.h>

#include <vector>
#include <string>
#include <cstring>
#include <mutex>
#include <windows.h>

int AUDIO_Play_Flag;

char artist[20];
char style[20];
char SampleName[128][16][64];
int Midiprg[128];
char nameins[128][20];

int Chan_Midi_Prg[MAX_TRACKS];
char Chan_History_State[256][MAX_TRACKS];

int done = 0;

extern Uint8* Mod_Memory;
extern char replayerPtkName[20];
int Calc_Length(void);
Uint32 STDCALL Mixer(Uint8* Buffer, Uint32 Len);
int Alloc_Patterns_Pool(void);

void Set_Default_Channels_Polyphony(void)
{
    for (int i = 0; i < MAX_TRACKS; i++)
        Channels_Polyphony[i] = DEFAULT_POLYPHONY;
}

namespace {

static std::mutex g_ptk_mutex;

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

class input_protrekkr;
static input_protrekkr* g_ptk_activeInstance = nullptr;

class input_protrekkr : public input_stubs {
public:
    void open(service_ptr_t<file> p_filehint, const char* p_path,
              t_input_open_reason p_reason, abort_callback& p_abort)
    {
        if (p_reason == input_open_info_write)
            throw exception_tagging_unsupported();

        m_file = p_filehint;
        input_open_file_helper(m_file, p_path, p_reason, p_abort);

        t_filesize fileSize = m_file->get_size(p_abort);
        if (fileSize == filesize_invalid || fileSize < 8 || fileSize > 256 * 1024 * 1024)
            throw exception_io_unsupported_format();

        m_fileData.resize(static_cast<size_t>(fileSize));
        m_file->read_object(m_fileData.data(), m_fileData.size(), p_abort);

        if (memcmp(m_fileData.data(), "PROTREK", 7) != 0 &&
            memcmp(m_fileData.data(), "TWNNSNG", 7) != 0)
            throw exception_io_unsupported_format();
    }

    ~input_protrekkr()
    {
        std::lock_guard<std::mutex> lock(g_ptk_mutex);
        if (g_ptk_activeInstance == this)
        {
            Ptk_Stop();
            Free_Samples();
            if (Mod_Memory) { free(Mod_Memory); Mod_Memory = nullptr; }
            if (RawPatterns) { free(RawPatterns); RawPatterns = nullptr; }
            g_ptk_activeInstance = nullptr;
        }
    }

    unsigned get_subsong_count() { return 1; }
    t_uint32 get_subsong(unsigned) { return 0; }

    void get_info(t_uint32, file_info& p_info, abort_callback&)
    {
        if (m_duration == 0)
        {
            std::unique_lock<std::mutex> lock(g_ptk_mutex, std::try_to_lock);
            if (lock.owns_lock() && g_ptk_activeInstance == nullptr)
            {
                g_ptk_activeInstance = this;
                Ptk_InitDriver();
                Alloc_Patterns_Pool();
                ReplayerFile rf(m_fileData.data(), m_fileData.size());
                if (Load_Ptk(rf))
                {
                    m_duration = (uint32_t)Calc_Length();
                    m_ptkName = replayerPtkName;
                    m_artist = ::artist;
                    m_style = ::style;
                }
                Ptk_Stop();
                Free_Samples();
                if (Mod_Memory) { free(Mod_Memory); Mod_Memory = nullptr; }
                if (RawPatterns) { free(RawPatterns); RawPatterns = nullptr; }
                g_ptk_activeInstance = nullptr;
            }
        }

        if (m_duration > 0)
            p_info.set_length(static_cast<double>(m_duration) / 1000.0);
        else
        {
            int dur = nostalgia_cfg::cfg_default_duration_sec.get();
            if (dur > 0) p_info.set_length(dur);
        }

        p_info.info_set_int("samplerate", MIX_RATE);
        p_info.info_set_int("channels", 2);
        p_info.info_set_int("bitspersample", 32);
        p_info.info_set("encoding", "synthesized");
        p_info.info_set("codec", "ProTrekkr");

        if (!m_ptkName.empty() && m_ptkName[0])
            p_info.meta_set("title", m_ptkName.c_str());
        if (!m_artist.empty() && m_artist[0])
            p_info.meta_set("artist", m_artist.c_str());
    }

    t_filestats2 get_stats2(unsigned f, abort_callback& a) { return m_file->get_stats2_(f, a); }
    t_filestats get_file_stats(abort_callback& a) { return m_file->get_stats(a); }

    void decode_initialize(t_uint32, unsigned, abort_callback&)
    {
        std::lock_guard<std::mutex> lock(g_ptk_mutex);

        if (g_ptk_activeInstance && g_ptk_activeInstance != this)
        {
            Ptk_Stop();
            Free_Samples();
            if (Mod_Memory) { free(Mod_Memory); Mod_Memory = nullptr; }
            if (RawPatterns) { free(RawPatterns); RawPatterns = nullptr; }
            g_ptk_activeInstance->m_decoding = false;
        }
        g_ptk_activeInstance = this;

        Ptk_InitDriver();
        Alloc_Patterns_Pool();

        ReplayerFile rf(m_fileData.data(), m_fileData.size());
        if (!Load_Ptk(rf))
        {
            g_ptk_activeInstance = nullptr;
            throw exception_io_unsupported_format();
        }

        m_duration = (uint32_t)Calc_Length();
        m_ptkName = replayerPtkName;
        m_artist = ::artist;
        m_style = ::style;

        Ptk_Play();
        m_decoding = true;
    }

    bool decode_run(audio_chunk& p_chunk, abort_callback&)
    {
        if (!m_decoding) return false;

        std::lock_guard<std::mutex> lock(g_ptk_mutex);
        if (g_ptk_activeInstance != this) { m_decoding = false; return false; }

        static constexpr uint32_t kBlockSize = 1024;
        float buffer[kBlockSize * 2];

        uint32_t rendered = Mixer((Uint8*)buffer, kBlockSize);
        if (rendered == 0)
        {
            m_decoding = false;
            return false;
        }

        p_chunk.set_data_32(buffer, rendered, 2, MIX_RATE);
        return true;
    }

    void decode_seek(double p_seconds, abort_callback& p_abort)
    {
        std::lock_guard<std::mutex> lock(g_ptk_mutex);
        if (g_ptk_activeInstance != this) return;

        Ptk_Stop();

        ReplayerFile rf(m_fileData.data(), m_fileData.size());
        Load_Ptk(rf);
        Ptk_Play();
        m_decoding = true;

        uint64_t targetSamples = (uint64_t)(p_seconds * MIX_RATE);
        float discardBuf[1024 * 2];
        uint64_t rendered = 0;
        while (rendered < targetSamples)
        {
            p_abort.check();
            uint32_t toMix = 1024;
            uint64_t left = targetSamples - rendered;
            if (toMix > left) toMix = (uint32_t)left;
            uint32_t got = Mixer((Uint8*)discardBuf, toMix);
            if (got == 0) break;
            rendered += got;
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
        if (!nostalgia_cfg::cfg_protrekkr_enabled.get()) return false;
        if (p_extension == nullptr || stricmp_utf8(p_extension, "ptk") != 0) return false;

        uint8_t hdr[7] = {};
        if (!g_read_header(p_path, hdr, sizeof(hdr))) return true;
        return memcmp(hdr, "PROTREK", 7) == 0 || memcmp(hdr, "TWNNSNG", 7) == 0;
    }
    static const char* g_get_name() { return "ProTrekkr decoder"; }
    static const GUID g_get_guid() {
        static const GUID guid = { 0xbb22cc33, 0xdd44, 0x4ee5, { 0xff, 0x66, 0x00, 0x88, 0x11, 0x99, 0x22, 0xaa } };
        return guid;
    }

private:
    service_ptr_t<file> m_file;
    std::vector<uint8_t> m_fileData;
    std::string m_ptkName, m_artist, m_style;
    uint32_t m_duration = 0;
    bool m_decoding = false;
};

} // anonymous namespace

static input_factory_t<input_protrekkr> g_input_protrekkr_factory;
