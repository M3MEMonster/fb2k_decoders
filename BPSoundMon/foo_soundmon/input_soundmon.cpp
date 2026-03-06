#include "stdafx.h"

#include "3rdParty/BSPlay/SoundMonModule.h"
#include "3rdParty/BSPlay/SoundMonPlayer.h"

static constexpr unsigned SOUNDMON_SAMPLE_RATE = 48000;
static constexpr unsigned SOUNDMON_CHANNELS = 2;
static constexpr unsigned DECODE_BLOCK_SAMPLES = 1024;

class input_soundmon : public input_stubs {
public:
    void open(service_ptr_t<file> p_filehint, const char* p_path, t_input_open_reason p_reason, abort_callback& p_abort) {
        if (p_reason == input_open_info_write)
            throw exception_tagging_unsupported();

        m_file = p_filehint;
        input_open_file_helper(m_file, p_path, p_reason, p_abort);

        t_filesize size = m_file->get_size(p_abort);
        if (size == filesize_invalid || size > 1024 * 1024 || size == 0)
            throw exception_io_unsupported_format();

        m_filedata.resize(static_cast<size_t>(size));
        m_file->read_object(m_filedata.data(), static_cast<size_t>(size), p_abort);

        m_module = SoundMon::Module::Load(m_filedata.data(), m_filedata.size());
        if (!m_module)
            throw exception_io_unsupported_format();

        m_player = new SoundMon::Player(m_module, SOUNDMON_SAMPLE_RATE);
    }

    ~input_soundmon() {
        delete m_player;
        if (m_module) m_module->Release();
    }

    unsigned get_subsong_count() {
        return m_player ? m_player->GetNumSubsongs() : 1;
    }

    t_uint32 get_subsong(unsigned p_index) {
        return p_index;
    }

    void get_info(t_uint32 p_subsong, file_info& p_info, abort_callback& p_abort) {
        m_player->SetSubsong(static_cast<uint8_t>(p_subsong));
        uint32_t dur_ms = m_player->GetDuration();
        if (dur_ms > 0)
            p_info.set_length(static_cast<double>(dur_ms) / 1000.0);

        p_info.info_set_int("samplerate", SOUNDMON_SAMPLE_RATE);
        p_info.info_set_int("channels", SOUNDMON_CHANNELS);
        p_info.info_set_int("bitspersample", 32);
        p_info.info_set("encoding", "synthesized");

        char codec[32];
        snprintf(codec, sizeof(codec), "SoundMon %d.0", m_module->GetVersion());
        p_info.info_set("codec", codec);

        const char* songname = m_module->GetSongName();
        if (songname && songname[0])
            p_info.meta_set("title", songname);
    }

    t_filestats2 get_stats2(uint32_t f, abort_callback& p_abort) {
        return m_file->get_stats2_(f, p_abort);
    }

    void decode_initialize(t_uint32 p_subsong, unsigned p_flags, abort_callback& p_abort) {
        m_current_subsong = static_cast<int>(p_subsong);
        m_player->SetSubsong(static_cast<uint8_t>(p_subsong));
        m_player->Stop();
        m_has_ended = false;
        m_decode_pos_samples = 0;

        uint32_t dur_ms = m_player->GetDuration();
        m_max_decode_samples = (dur_ms > 0)
            ? static_cast<t_uint64>(static_cast<double>(dur_ms) / 1000.0 * SOUNDMON_SAMPLE_RATE)
            : 0;
    }

    bool decode_run(audio_chunk& p_chunk, abort_callback& p_abort) {
        p_abort.check();

        if (m_has_ended)
            return false;

        if (m_max_decode_samples > 0 && m_decode_pos_samples >= m_max_decode_samples) {
            m_has_ended = true;
            return false;
        }

        unsigned samples_to_produce = DECODE_BLOCK_SAMPLES;
        if (m_max_decode_samples > 0) {
            t_uint64 remaining = m_max_decode_samples - m_decode_pos_samples;
            if (remaining < samples_to_produce)
                samples_to_produce = static_cast<unsigned>(remaining);
        }

        m_render_buf.resize(samples_to_produce * SOUNDMON_CHANNELS);
        uint32_t rendered = m_player->Render(m_render_buf.data(), samples_to_produce);

        if (rendered == 0) {
            m_has_ended = true;
            return false;
        }

        p_chunk.set_data_32(
            m_render_buf.data(),
            rendered,
            SOUNDMON_CHANNELS,
            SOUNDMON_SAMPLE_RATE
        );

        m_decode_pos_samples += rendered;
        return true;
    }

    void decode_seek(double p_seconds, abort_callback& p_abort) {
        uint32_t target_ms = static_cast<uint32_t>(p_seconds * 1000.0);
        m_player->Seek(target_ms);
        m_decode_pos_samples = static_cast<t_uint64>(p_seconds * SOUNDMON_SAMPLE_RATE);
        m_has_ended = false;
    }

    bool decode_can_seek() { return true; }

    void retag_set_info(t_uint32, const file_info&, abort_callback&) {
        throw exception_tagging_unsupported();
    }
    void retag_commit(abort_callback&) {
        throw exception_tagging_unsupported();
    }
    void remove_tags(abort_callback&) {
        throw exception_tagging_unsupported();
    }

    static bool g_is_our_content_type(const char*) { return false; }

    static bool g_is_our_path(const char* p_path, const char* p_extension) {
        if (!p_extension) return false;
        return _stricmp(p_extension, "bp") == 0
            || _stricmp(p_extension, "bp3") == 0
            || _stricmp(p_extension, "bs") == 0;
    }

    static const char* g_get_name() { return "SoundMon Decoder"; }

    static const GUID g_get_guid() {
        // {7F4E2D1C-5A3B-4C8D-9E0F-1A2B3C4D5E6F}
        static const GUID guid = { 0x7f4e2d1c, 0x5a3b, 0x4c8d, { 0x9e, 0x0f, 0x1a, 0x2b, 0x3c, 0x4d, 0x5e, 0x6f } };
        return guid;
    }

private:
    service_ptr_t<file> m_file;
    std::vector<uint8_t> m_filedata;

    SoundMon::Module* m_module = nullptr;
    SoundMon::Player* m_player = nullptr;

    int m_current_subsong = 0;
    bool m_has_ended = false;
    t_uint64 m_decode_pos_samples = 0;
    t_uint64 m_max_decode_samples = 0;

    std::vector<float> m_render_buf;
};

static input_factory_t<input_soundmon> g_input_soundmon_factory;

namespace {
    class soundmon_file_types : public input_file_type {
    public:
        unsigned get_count() override { return 1; }

        bool get_name(unsigned idx, pfc::string_base& out) override {
            if (idx != 0) return false;
            out = "SoundMon music files";
            return true;
        }

        bool get_mask(unsigned idx, pfc::string_base& out) override {
            if (idx != 0) return false;
            out = "*.bp;*.bp3;*.bs";
            return true;
        }

        bool is_associatable(unsigned) override { return true; }
    };

    static service_factory_single_t<soundmon_file_types> g_soundmon_file_types_factory;
}
