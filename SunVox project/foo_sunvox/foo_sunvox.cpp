#include <foobar2000.h>

#include <sundog.h>
#include <sunvox_engine.h>

#include <mutex>
#include <vector>
#include <string>
#include <cstring>

const char* g_app_name = "foo_sunvox";
const char* g_app_name_short = "foo_sunvox";
const char* g_app_config[] = { nullptr };
const char* g_app_log = nullptr;
const char* g_app_usage = nullptr;
int g_app_window_xsize = 0;
int g_app_window_ysize = 0;
uint g_app_window_flags = 0;
app_option g_app_options[] = { { nullptr, 0 } };

int app_global_init()
{
    return sunvox_global_init() ? -1 : 0;
}

int app_global_deinit()
{
    return sunvox_global_deinit() ? -1 : 0;
}

app_parameter* get_app_parameters() { return nullptr; }

int render_piece_of_sound(sundog_sound* ss, int slot_num)
{
    if (!ss) return 0;

    sundog_sound_slot* slot = &ss->slots[slot_num];
    sunvox_engine* s = (sunvox_engine*)slot->user_data;

    if (!s) return 0;
    if (!s->initialized) return 0;

    sunvox_render_data rdata;
    SMEM_CLEAR_STRUCT(rdata);
    rdata.buffer_type = ss->out_type;
    rdata.buffer = slot->buffer;
    rdata.frames = slot->frames;
    rdata.channels = ss->out_channels;
    rdata.out_latency = ss->out_latency;
    rdata.out_latency2 = ss->out_latency2;
    rdata.out_time = slot->time;
    rdata.in_buffer = slot->in_buffer;
    rdata.in_type = ss->in_type;
    rdata.in_channels = ss->in_channels;

    int handled = sunvox_render_piece_of_sound(&rdata, s);

    if (handled && rdata.silence)
        handled = 2;

    return handled;
}

namespace {

static constexpr uint32_t kSampleRate = 48000;
static constexpr int kNumChannels = 2;
static constexpr int kBlockSize = 1024;

static std::mutex g_globalInitMutex;
static int g_globalInitCount = 0;

static void ensure_global_init()
{
    std::lock_guard<std::mutex> lock(g_globalInitMutex);
    if (g_globalInitCount++ == 0)
        sundog_global_init();
}

static void ensure_global_deinit()
{
    std::lock_guard<std::mutex> lock(g_globalInitMutex);
    if (--g_globalInitCount == 0)
        sundog_global_deinit();
}

class input_sunvox : public input_stubs {
public:
    input_sunvox() = default;

    ~input_sunvox()
    {
        cleanup();
    }

    void open(service_ptr_t<file> p_filehint, const char* p_path,
              t_input_open_reason p_reason, abort_callback& p_abort)
    {
        if (p_reason == input_open_info_write)
            throw exception_tagging_unsupported();

        m_file = p_filehint;
        input_open_file_helper(m_file, p_path, p_reason, p_abort);

        t_filesize fileSize = m_file->get_size(p_abort);
        if (fileSize == filesize_invalid || fileSize > 256 * 1024 * 1024)
            throw exception_io_unsupported_format();

        m_fileData.resize(static_cast<size_t>(fileSize));
        m_file->read_object(m_fileData.data(), m_fileData.size(), p_abort);

        ensure_global_init();
        m_globalInited = true;

        m_engine = new sunvox_engine;
        memset(m_engine, 0, sizeof(sunvox_engine));
        sunvox_engine_init(
            SUNVOX_FLAG_CREATE_PATTERN | SUNVOX_FLAG_CREATE_MODULES |
            SUNVOX_FLAG_MAIN | SUNVOX_FLAG_ONE_THREAD | SUNVOX_FLAG_PLAYER_ONLY,
            kSampleRate, nullptr, nullptr, nullptr, nullptr, m_engine);

        sfs_file f = sfs_open_in_memory(m_fileData.data(), m_fileData.size());
        int rv = sunvox_load_proj_from_fd(f, 0, m_engine);
        sfs_close(f);

        if (rv || m_engine->type == 1 || m_engine->type == 2)
        {
            sunvox_engine_close(m_engine);
            delete m_engine;
            m_engine = nullptr;
            throw exception_io_unsupported_format();
        }

        m_numFrames = sunvox_get_proj_frames(0, 0, m_engine);

        memset(&m_sound, 0, sizeof(m_sound));
        sundog_sound_init(&m_sound, nullptr, sound_buffer_float32,
                          kSampleRate, kNumChannels,
                          SUNDOG_SOUND_FLAG_ONE_THREAD | SUNDOG_SOUND_FLAG_USER_CONTROLLED);

        sundog_sound_set_slot_callback(&m_sound, 0, &render_piece_of_sound, m_engine);
        sundog_sound_play(&m_sound, 0);
        sundog_sound_input(&m_sound, true);

        m_engine->net->global_volume = 256;

        if (m_engine->proj_name && m_engine->proj_name[0])
            m_projName = m_engine->proj_name;
        else
            m_projName.clear();

        m_durationMs = static_cast<uint32_t>((static_cast<uint64_t>(m_numFrames) * 1000) / kSampleRate);
    }

    unsigned get_subsong_count() { return 1; }
    t_uint32 get_subsong(unsigned p_index) { return 0; }

    void get_info(t_uint32 p_subsong, file_info& p_info, abort_callback& p_abort)
    {
        if (m_durationMs > 0)
            p_info.set_length(static_cast<double>(m_durationMs) / 1000.0);

        p_info.info_set_int("samplerate", kSampleRate);
        p_info.info_set_int("channels", kNumChannels);
        p_info.info_set_int("bitspersample", 32);
        p_info.info_set("encoding", "synthesized");
        p_info.info_set("codec", "SunVox");

        if (m_engine)
        {
            char buf[64];
            sprintf_s(buf, "%u.%u.%u.%u",
                m_engine->base_version >> 24,
                (m_engine->base_version >> 16) & 0xff,
                (m_engine->base_version >> 8) & 0xff,
                m_engine->base_version & 0xff);
            p_info.info_set("sunvox_project_version", buf);
        }

        if (!m_projName.empty())
            p_info.meta_set("title", m_projName.c_str());
    }

    t_filestats2 get_stats2(unsigned f, abort_callback& a) {
        return m_file->get_stats2_(f, a);
    }

    t_filestats get_file_stats(abort_callback& p_abort) {
        return m_file->get_stats(p_abort);
    }

    void decode_initialize(t_uint32 p_subsong, unsigned p_flags, abort_callback& p_abort)
    {
        set_position(0);
        m_currentFrame = 0;
        m_decoding = true;
    }

    bool decode_run(audio_chunk& p_chunk, abort_callback& p_abort)
    {
        if (!m_decoding || !m_engine) return false;

        uint32_t numFrames = m_numFrames;
        uint32_t currentFrame = m_currentFrame;
        uint32_t samplesToRender = kBlockSize;

        if (currentFrame + samplesToRender > numFrames)
            samplesToRender = numFrames - currentFrame;

        if (samplesToRender == 0)
        {
            m_decoding = false;
            return false;
        }

        float outputBuffer[kBlockSize * kNumChannels];

        m_engine->stop_at_the_end_of_proj = true;
        user_controlled_sound_callback(&m_sound, outputBuffer, samplesToRender, 0, 0);

        m_currentFrame = currentFrame + samplesToRender;

        p_chunk.set_data_32(outputBuffer, samplesToRender, kNumChannels, kSampleRate);
        return true;
    }

    void decode_seek(double p_seconds, abort_callback& p_abort)
    {
        if (!m_engine) return;

        uint64_t targetFrame = static_cast<uint64_t>(p_seconds * kSampleRate);
        if (targetFrame > m_numFrames)
            targetFrame = m_numFrames;

        uint64_t frameToCheck = targetFrame % m_numFrames;
        uint64_t baseFrames = targetFrame - frameToCheck;

        int numLines = sunvox_get_proj_lines(m_engine);
        sunvox_time_map_item* map = SMEM_ALLOC2(sunvox_time_map_item, numLines);
        uint32_t* frameMap = SMEM_ALLOC2(uint32_t, numLines + 1);
        sunvox_get_time_map(map, frameMap, 0, numLines, m_engine);
        smem_free(map);
        frameMap[numLines] = ~uint32_t(0);

        for (int i = 0; i <= numLines; ++i)
        {
            if (frameMap[i] >= frameToCheck)
            {
                int pos = (frameMap[i] == frameToCheck) ? i : i - 1;
                if (pos < 0) pos = 0;
                frameToCheck = frameMap[pos];
                set_position(pos);
                break;
            }
        }
        smem_free(frameMap);

        m_currentFrame = static_cast<uint32_t>(frameToCheck);
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
        return stricmp_utf8(p_extension, "sunvox") == 0;
    }
    static const char* g_get_name() { return "SunVox decoder"; }
    static const GUID g_get_guid() {
        static const GUID guid = {
            0xa1b2c3d4, 0xe5f6, 0x4a7b,
            { 0x8c, 0x9d, 0x0e, 0x1f, 0x2a, 0x3b, 0x5c, 0x6d }
        };
        return guid;
    }

private:
    void set_position(int position)
    {
        if (!m_engine) return;
        if (sunvox_get_playing_status(m_engine))
            sunvox_stop(m_engine);
        sunvox_stop(m_engine);
        sunvox_play(position, true, -1, m_engine);
    }

    void cleanup()
    {
        if (m_engine)
        {
            sunvox_engine_close(m_engine);
            sundog_sound_deinit(&m_sound);
            delete m_engine;
            m_engine = nullptr;
        }
        if (m_globalInited)
        {
            ensure_global_deinit();
            m_globalInited = false;
        }
    }

    service_ptr_t<file> m_file;
    std::vector<uint8_t> m_fileData;
    sunvox_engine* m_engine = nullptr;
    sundog_sound m_sound = {};
    uint32_t m_numFrames = 0;
    uint32_t m_currentFrame = 0;
    uint32_t m_durationMs = 0;
    bool m_decoding = false;
    bool m_globalInited = false;
    std::string m_projName;
};

} // anonymous namespace

static input_factory_t<input_sunvox> g_input_sunvox_factory;
