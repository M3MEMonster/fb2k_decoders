#include <foobar2000.h>
#include "cfg_vars.h"

#include "furnace/src/engine/engine.h"
#include "furnace/src/engine/fileOps/fileOpsCommon.h"
#include "furnace/src/ta-log.h"

#include <vector>
#include <string>
#include <mutex>

// Stub required by the Furnace engine (ta-utils.h)
void reportError(String) {}

namespace {

static std::mutex g_furnace_mutex;
static bool g_log_initialized = false;

// ---------------------------------------------------------------
// Persistent global engine — created once, NEVER quit()'d.
//
// DivEngine::quit() corrupts unidentified global state, making
// subsequent engine instances unable to load files.  Instead we
// keep one engine alive for the lifetime of the process and use
// load()'s built-in reload mechanism: when the engine is active,
// load→loadFur/loadDMF/loadFTM/etc. internally call
//   quitDispatch() → reload song → initDispatch() → renderSamples() → reset()
// This properly reinitializes per-song dispatch without touching
// the problematic global teardown path.
// ---------------------------------------------------------------
static DivEngine* g_engine = nullptr;
static bool g_engine_initialized = false;  // true after first init()
static const void* g_engine_owner = nullptr;  // which input instance last loaded

static void ensure_log_init()
{
    if (!g_log_initialized)
    {
        FILE* devnull = fopen("NUL", "w");
        initLog(devnull ? devnull : stderr);
        logLevel = -1;
        g_log_initialized = true;
    }
}

static bool check_extension_enabled(const char* ext)
{
    if (stricmp_utf8(ext, "fur") == 0) return furnace_cfg::cfg_fur_enabled.get();
    if (stricmp_utf8(ext, "dmf") == 0) return furnace_cfg::cfg_dmf_enabled.get();
    if (stricmp_utf8(ext, "ftm") == 0) return furnace_cfg::cfg_ftm_enabled.get();
    if (stricmp_utf8(ext, "dnm") == 0) return furnace_cfg::cfg_dnm_enabled.get();
    if (stricmp_utf8(ext, "0cc") == 0) return furnace_cfg::cfg_0cc_enabled.get();
    if (stricmp_utf8(ext, "eft") == 0) return furnace_cfg::cfg_eft_enabled.get();
    if (stricmp_utf8(ext, "tfm") == 0) return furnace_cfg::cfg_tfm_enabled.get();
    if (stricmp_utf8(ext, "tfe") == 0) return furnace_cfg::cfg_tfe_enabled.get();
    return false;
}

// Load file data into the persistent global engine.
// Caller MUST hold g_furnace_mutex.
static bool load_global_engine(const std::vector<uint8_t>& fileData, const char* path)
{
    ensure_log_init();

    if (!g_engine)
    {
        g_engine = new DivEngine;
        g_engine->preInit();
    }

    // load() takes ownership of the buffer via delete[].
    auto* loadBuf = new unsigned char[fileData.size()];
    memcpy(loadBuf, fileData.data(), fileData.size());

    if (!g_engine->load(loadBuf, fileData.size(), path))
    {
        FB2K_console_formatter() << "foo_furnace: load() failed: "
            << g_engine->getLastError().c_str();
        return false;
    }

    // First time: full init (audio backend, buffers, dispatch).
    // Subsequent loads: load() already handled reinit internally
    // because engine's `active` flag is true.
    if (!g_engine_initialized)
    {
        if (!g_engine->init())
        {
            FB2K_console_formatter() << "foo_furnace: init() failed";
            return false;
        }
        g_engine_initialized = true;
    }

    return true;
}

class input_furnace : public input_stubs {
public:
    void open(service_ptr_t<file> p_filehint, const char* p_path,
        t_input_open_reason p_reason, abort_callback& p_abort)
    {
        if (p_reason == input_open_info_write) throw exception_tagging_unsupported();

        m_file = p_filehint;
        input_open_file_helper(m_file, p_path, p_reason, p_abort);

        t_filesize fileSize = m_file->get_size(p_abort);
        if (fileSize == filesize_invalid || fileSize < 8 || fileSize > 256 * 1024 * 1024)
            throw exception_io_unsupported_format();

        m_fileData.resize(static_cast<size_t>(fileSize));
        m_file->read_object(m_fileData.data(), m_fileData.size(), p_abort);
        m_path = p_path;
    }

    ~input_furnace()
    {
        // Release ownership but NEVER quit the engine.
        std::lock_guard<std::mutex> lock(g_furnace_mutex);
        if (g_engine_owner == this)
            g_engine_owner = nullptr;
    }

    unsigned get_subsong_count()
    {
        std::lock_guard<std::mutex> lock(g_furnace_mutex);

        if (!load_global_engine(m_fileData, m_path.c_str()))
            throw exception_io_unsupported_format();

        g_engine_owner = this;

        // Cache all metadata so get_info() needs no engine access.
        unsigned count = static_cast<unsigned>(g_engine->song.subsong.size());
        if (count == 0) count = 1;

        m_cachedSubsongCount = count;
        m_songName = g_engine->song.name;
        m_songAuthor = g_engine->song.author;
        m_songSystemName = g_engine->song.systemName;

        // Build system description
        m_systems.clear();
        uint8_t sysCount[DIV_MAX_CHIPS] = {};
        DivSystem sysId[DIV_MAX_CHIPS];
        uint8_t numSys = 0;
        for (uint8_t i = 0; i < g_engine->song.systemLen; ++i)
        {
            uint8_t curSys = 0;
            for (; curSys < numSys; ++curSys)
            {
                if (sysId[curSys] == g_engine->song.system[i])
                    break;
            }
            sysId[curSys] = g_engine->song.system[i];
            if (sysCount[curSys]++ == 0)
                numSys++;
        }
        for (uint8_t i = 0; i < numSys; ++i)
        {
            if (i > 0) m_systems += '+';
            if (sysCount[i] > 1)
            {
                char buf[8];
                sprintf(buf, "[%dx]", sysCount[i]);
                m_systems += buf;
            }
            m_systems += g_engine->getSystemDef(sysId[i])->name;
        }

        m_sampleRate = static_cast<uint32_t>(g_engine->getAudioDescGot().rate);
        if (m_sampleRate == 0) m_sampleRate = 44100;
        m_totalChannels = g_engine->getTotalChannelCount();
        m_isDMF = g_engine->song.isDMF;
        m_version = g_engine->song.version;

        // Cache subsong names
        m_subsongNames.clear();
        m_subsongNames.reserve(count);
        for (unsigned i = 0; i < count; i++)
        {
            if (i < g_engine->song.subsong.size())
                m_subsongNames.push_back(g_engine->song.subsong[i]->name);
            else
                m_subsongNames.push_back("");
        }

        return count;
    }

    t_uint32 get_subsong(unsigned p_index) { return p_index; }

    void get_info(t_uint32 p_subsong, file_info& p_info, abort_callback&)
    {
        // Uses only cached metadata — no engine access needed.
        int dur = furnace_cfg::cfg_default_duration_sec.get();
        if (dur > 0) p_info.set_length(dur);

        p_info.info_set_int("samplerate", m_sampleRate ? m_sampleRate : 44100);
        p_info.info_set_int("channels", 2);
        p_info.info_set_int("bitspersample", 32);
        p_info.info_set("encoding", "synthesized");

        if (m_isDMF)
            p_info.info_set("codec", "DefleMask (Furnace)");
        else if (m_version == DIV_VERSION_FTM)
            p_info.info_set("codec", "FamiTracker (Furnace)");
        else
            p_info.info_set("codec", "Furnace");

        if (!m_systems.empty())
            p_info.info_set("furnace_system", m_systems.c_str());

        if (m_totalChannels > 0)
            p_info.info_set_int("furnace_channels", m_totalChannels);

        if (!m_songName.empty())
            p_info.meta_set("album", m_songName.c_str());

        if (!m_songAuthor.empty())
            p_info.meta_set("artist", m_songAuthor.c_str());

        if (p_subsong < m_subsongNames.size() && !m_subsongNames[p_subsong].empty())
            p_info.meta_set("title", m_subsongNames[p_subsong].c_str());
        else if (!m_songName.empty())
            p_info.meta_set("title", m_songName.c_str());
    }

    t_filestats2 get_stats2(unsigned f, abort_callback& a) { return m_file->get_stats2_(f, a); }
    t_filestats get_file_stats(abort_callback& a) { return m_file->get_stats(a); }

    void decode_initialize(t_uint32 p_subsong, unsigned, abort_callback&)
    {
        std::lock_guard<std::mutex> lock(g_furnace_mutex);

        // Reload our file if another instance has taken over the engine.
        if (g_engine_owner != this)
        {
            if (!load_global_engine(m_fileData, m_path.c_str()))
                throw exception_io_unsupported_format();
            g_engine_owner = this;
        }

        if (p_subsong < g_engine->song.subsong.size())
            g_engine->changeSongP(p_subsong);

        g_engine->play();
        m_sampleRate = static_cast<uint32_t>(g_engine->getAudioDescGot().rate);
        if (m_sampleRate == 0) m_sampleRate = 44100;

        m_renderedSamples = 0;
        m_lastLoopPos = -1;
        m_numRemainingSamples = 0;
        m_decoding = true;
        m_currentSubsong = p_subsong;
    }

    bool decode_run(audio_chunk& p_chunk, abort_callback&)
    {
        if (!m_decoding) return false;

        std::lock_guard<std::mutex> lock(g_furnace_mutex);

        // If another instance has preempted us, stop gracefully.
        if (g_engine_owner != this || !g_engine)
        {
            m_decoding = false;
            return false;
        }

        // Check duration limit
        int durSec = furnace_cfg::cfg_default_duration_sec.get();
        if (durSec > 0)
        {
            double elapsed = static_cast<double>(m_renderedSamples) / m_sampleRate;
            if (elapsed >= durSec)
            {
                m_decoding = false;
                return false;
            }
        }

        static constexpr uint32_t kBlockFrames = 1024;

        uint32_t framesToRender = kBlockFrames;

        // Use buffered samples if available
        if (m_numRemainingSamples == 0)
        {
            float* samples[] = { m_samplesL, m_samplesR };
            g_engine->nextBuf(nullptr, samples, 0, 2, kMaxSamples);
            m_numRemainingSamples = kMaxSamples;
            m_lastLoopPos = g_engine->lastLoopPos;
        }

        uint32_t available = m_numRemainingSamples;
        if (m_lastLoopPos > -1)
        {
            uint32_t offset = kMaxSamples - m_numRemainingSamples;
            if (static_cast<uint32_t>(m_lastLoopPos) > offset)
                available = static_cast<uint32_t>(m_lastLoopPos) - offset;
            else
            {
                // Loop point reached
                m_decoding = false;
                return false;
            }
        }

        uint32_t toCopy = (framesToRender < available) ? framesToRender : available;
        uint32_t srcOffset = kMaxSamples - m_numRemainingSamples;

        // Interleave stereo
        m_convertBuffer.resize(static_cast<size_t>(toCopy) * 2);
        for (uint32_t i = 0; i < toCopy; i++)
        {
            m_convertBuffer[i * 2]     = m_samplesL[srcOffset + i];
            m_convertBuffer[i * 2 + 1] = m_samplesR[srcOffset + i];
        }

        m_numRemainingSamples -= toCopy;
        m_renderedSamples += toCopy;
        p_chunk.set_data_32(m_convertBuffer.data(), toCopy, 2, m_sampleRate);
        return true;
    }

    void decode_seek(double p_seconds, abort_callback& p_abort)
    {
        std::lock_guard<std::mutex> lock(g_furnace_mutex);

        if (g_engine_owner != this || !g_engine) return;

        // Reset and fast-forward
        g_engine->changeSongP(m_currentSubsong);
        g_engine->play();
        m_renderedSamples = 0;
        m_lastLoopPos = -1;
        m_numRemainingSamples = 0;
        m_decoding = true;

        const uint64_t targetSamples = static_cast<uint64_t>(p_seconds * m_sampleRate);
        float discardL[kMaxSamples];
        float discardR[kMaxSamples];
        float* discardBufs[] = { discardL, discardR };

        while (m_renderedSamples < targetSamples)
        {
            p_abort.check();
            uint64_t remain = targetSamples - m_renderedSamples;
            uint32_t thisBlock = static_cast<uint32_t>(
                remain < kMaxSamples ? remain : kMaxSamples);
            g_engine->nextBuf(nullptr, discardBufs, 0, 2, thisBlock);
            m_renderedSamples += thisBlock;
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

    static bool g_is_our_path(const char*, const char* p_extension)
    {
        if (p_extension == nullptr) return false;
        return check_extension_enabled(p_extension);
    }

    static const char* g_get_name() { return "Furnace Emulator decoder"; }

    static const GUID g_get_guid()
    {
        // {8B9CADBE-F012-4345-6789-ABCDEF012345}
        static const GUID guid = { 0x8b9cadbe, 0xf012, 0x4345, { 0x67, 0x89, 0xab, 0xcd, 0xef, 0x01, 0x23, 0x45 } };
        return guid;
    }

private:
    static constexpr uint32_t kMaxSamples = 2048;

    service_ptr_t<file> m_file;
    std::vector<uint8_t> m_fileData;
    std::string m_path;
    std::vector<float> m_convertBuffer;

    float m_samplesL[kMaxSamples] = {};
    float m_samplesR[kMaxSamples] = {};
    uint64_t m_renderedSamples = 0;
    int32_t m_lastLoopPos = -1;
    uint32_t m_numRemainingSamples = 0;
    bool m_decoding = false;
    t_uint32 m_currentSubsong = 0;

    // Cached metadata
    uint32_t m_sampleRate = 44100;
    int m_totalChannels = 0;
    bool m_isDMF = false;
    unsigned short m_version = 0;
    unsigned m_cachedSubsongCount = 0;
    std::string m_songName;
    std::string m_songAuthor;
    std::string m_songSystemName;
    std::string m_systems;
    std::vector<std::string> m_subsongNames;
};

} // anonymous namespace

static input_factory_t<input_furnace> g_input_furnace_factory;
