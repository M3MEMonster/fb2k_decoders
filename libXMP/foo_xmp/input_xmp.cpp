#include "stdafx.h"
#include "xmp_config.h"
#include <xmp.h>
#include <vector>
#include <algorithm>

static constexpr unsigned SAMPLE_RATE = 48000;
static constexpr unsigned CHANNELS    = 2;
static constexpr unsigned BITS        = 16;
static constexpr unsigned BLOCK_FRAMES = 1024;
static constexpr unsigned BLOCK_BYTES  = BLOCK_FRAMES * CHANNELS * (BITS / 8);

class input_xmp : public input_stubs {
public:
    ~input_xmp() { cleanup(); }

    void open(service_ptr_t<file> p_filehint, const char* p_path,
              t_input_open_reason p_reason, abort_callback& p_abort)
    {
        if (p_reason == input_open_info_write)
            throw exception_tagging_unsupported();

        m_file = p_filehint;
        input_open_file_helper(m_file, p_path, p_reason, p_abort);

        t_filesize size = m_file->get_size(p_abort);
        if (size == filesize_invalid || size == 0 || size > 128 * 1024 * 1024)
            throw exception_io_unsupported_format();

        m_filedata.resize(static_cast<size_t>(size));
        m_file->read_object(m_filedata.data(), m_filedata.size(), p_abort);

        if (xmp_test_module_from_memory(m_filedata.data(),
                static_cast<long>(m_filedata.size()), nullptr) != 0)
            throw exception_io_unsupported_format();

        m_ctx = xmp_create_context();
        if (!m_ctx)
            throw exception_io_unsupported_format();

        if (xmp_load_module_from_memory(m_ctx, m_filedata.data(),
                static_cast<long>(m_filedata.size())) != 0) {
            cleanup();
            throw exception_io_unsupported_format();
        }
        m_loaded = true;

        xmp_get_module_info(m_ctx, &m_info);
    }

    unsigned get_subsong_count() {
        return (unsigned)(std::max)(1, m_info.num_sequences);
    }

    t_uint32 get_subsong(unsigned p_index) { return p_index; }

    void get_info(t_uint32 p_subsong, file_info& p_info, abort_callback&) {
        int seq = static_cast<int>(p_subsong);

        if (m_info.seq_data && seq < m_info.num_sequences) {
            double dur_ms = m_info.seq_data[seq].duration;
            if (dur_ms > 0)
                p_info.set_length(dur_ms / 1000.0);
        }

        p_info.info_set_int("samplerate", SAMPLE_RATE);
        p_info.info_set_int("channels", CHANNELS);
        p_info.info_set_int("bitspersample", BITS);
        p_info.info_set("encoding", "synthesized");

        if (m_info.mod) {
            if (m_info.mod->type[0])
                p_info.info_set("codec", m_info.mod->type);
            if (m_info.mod->chn > 0)
                p_info.info_set_int("module_channels", m_info.mod->chn);
            if (m_info.mod->name[0])
                p_info.meta_set("title", m_info.mod->name);
        }
        if (m_info.comment && m_info.comment[0])
            p_info.meta_set("comment", m_info.comment);
    }

    t_filestats2 get_stats2(uint32_t f, abort_callback& p_abort) {
        return m_file->get_stats2_(f, p_abort);
    }

    void decode_initialize(t_uint32 p_subsong, unsigned, abort_callback&) {
        if (m_playing) { xmp_end_player(m_ctx); m_playing = false; }

        if (xmp_start_player(m_ctx, SAMPLE_RATE, 0) != 0)
            throw std::runtime_error("xmp_start_player failed");
        m_playing = true;

        xmp_set_player(m_ctx, XMP_PLAYER_INTERP, XMP_INTERP_SPLINE);
        xmp_set_player(m_ctx, XMP_PLAYER_DSP, XMP_DSP_ALL);

        int seq = static_cast<int>(p_subsong);
        if (m_info.seq_data && seq < m_info.num_sequences)
            xmp_set_position(m_ctx, m_info.seq_data[seq].entry_point);

        m_subsong = seq;
        m_ended = false;

        if (m_info.seq_data && seq < m_info.num_sequences)
            m_duration_ms = m_info.seq_data[seq].duration;
        else
            m_duration_ms = 0;
    }

    bool decode_run(audio_chunk& p_chunk, abort_callback& p_abort) {
        p_abort.check();
        if (m_ended) return false;

        m_pcm.resize(BLOCK_FRAMES * CHANNELS);

        int rc = xmp_play_buffer(m_ctx, m_pcm.data(), BLOCK_BYTES, 0);
        if (rc == -XMP_END || rc < -1) {
            m_ended = true;
            return false;
        }

        xmp_frame_info fi;
        xmp_get_frame_info(m_ctx, &fi);

        if (m_duration_ms > 0 && fi.time >= m_duration_ms) {
            m_ended = true;
            return false;
        }

        if (fi.loop_count > 0) {
            m_ended = true;
            return false;
        }

        p_chunk.set_data_fixedpoint(
            m_pcm.data(), BLOCK_BYTES,
            SAMPLE_RATE, CHANNELS, BITS,
            audio_chunk::channel_config_stereo);
        return true;
    }

    bool decode_can_seek() { return true; }

    void decode_seek(double p_seconds, abort_callback&) {
        xmp_seek_time(m_ctx, static_cast<int>(p_seconds * 1000.0));
        m_ended = false;
    }

    void retag_set_info(t_uint32, const file_info&, abort_callback&) {
        throw exception_tagging_unsupported();
    }
    void retag_commit(abort_callback&) { throw exception_tagging_unsupported(); }
    void remove_tags(abort_callback&)  { throw exception_tagging_unsupported(); }

    static bool g_is_our_content_type(const char*) { return false; }

    static bool g_is_our_path(const char*, const char* p_ext) {
        if (!p_ext) return false;
        return xmp_config::is_format_enabled(p_ext);
    }

    static const char* g_get_name() { return "XMP Module Decoder"; }

    static const GUID g_get_guid() {
        static const GUID guid = {
            0x5a6b7c8d, 0x9e0f, 0x1a2b,
            { 0x3c, 0x4d, 0x5e, 0x6f, 0x7a, 0x8b, 0x9c, 0x0d }
        };
        return guid;
    }

private:
    void cleanup() {
        if (m_playing) { xmp_end_player(m_ctx); m_playing = false; }
        if (m_loaded)  { xmp_release_module(m_ctx); m_loaded = false; }
        if (m_ctx)     { xmp_free_context(m_ctx); m_ctx = nullptr; }
    }

    service_ptr_t<file>   m_file;
    std::vector<uint8_t>  m_filedata;
    xmp_context           m_ctx = nullptr;
    bool                  m_loaded  = false;
    bool                  m_playing = false;
    bool                  m_ended   = false;
    xmp_module_info       m_info{};
    int                   m_subsong = 0;
    int                   m_duration_ms = 0;
    std::vector<int16_t>  m_pcm;
};

static input_factory_t<input_xmp> g_input_xmp;

// File type registration for foobar2000's "open file" dialog
namespace {
class xmp_file_types : public input_file_type {
public:
    unsigned get_count() override { return 1; }

    bool get_name(unsigned idx, pfc::string_base& out) override {
        if (idx) return false;
        out = "Module music files (libXMP)";
        return true;
    }

    bool get_mask(unsigned idx, pfc::string_base& out) override {
        if (idx) return false;
        out =
            "*.669;*.abk;*.amf;*.arch;*.coco;"
            "*.dbm;*.digi;*.dsym;*.dt;*.dtm;"
            "*.emod;*.far;*.flt;*.flx;*.fnk;"
            "*.gdm;*.hmn;*.ice;*.imf;*.ims;"
            "*.it;*.j2b;*.liq;*.m15;*.mdl;"
            "*.med;*.mfp;*.mgt;*.mmd;*.mmd0;"
            "*.mmd1;*.mmd2;*.mmd3;*.mod;*.mtn;"
            "*.mtm;*.musx;*.nst;*.okt;*.ps16;"
            "*.psm;*.pt3;*.pt36;*.ptm;*.rtm;"
            "*.s3m;*.sfx;*.stim;*.stk;*.stm;"
            "*.stx;*.sym;*.ult;*.umx;*.wow;"
            "*.xm;*.xmf";
        return true;
    }

    bool is_associatable(unsigned) override { return true; }
};
static service_factory_single_t<xmp_file_types> g_xmp_file_types;
}
