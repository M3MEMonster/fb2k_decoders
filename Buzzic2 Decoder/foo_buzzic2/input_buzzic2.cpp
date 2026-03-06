#include "stdafx.h"

#include "3rdParty/src/Buzzic2.h"

static constexpr unsigned BUZZIC2_SAMPLE_RATE = 44100;
static constexpr unsigned BUZZIC2_CHANNELS = 2;
static constexpr unsigned DECODE_BLOCK_SAMPLES = 1024;

class input_buzzic2 : public input_stubs {
public:
    void open(service_ptr_t<file> p_filehint, const char* p_path, t_input_open_reason p_reason, abort_callback& p_abort) {
        if (p_reason == input_open_info_write)
            throw exception_tagging_unsupported();

        m_file = p_filehint;
        input_open_file_helper(m_file, p_path, p_reason, p_abort);

        t_filesize size = m_file->get_size(p_abort);
        if (size == filesize_invalid || size > 4 * 1024 * 1024 || size == 0)
            throw exception_io_unsupported_format();

        m_filedata.resize(static_cast<size_t>(size));
        m_file->read_object(m_filedata.data(), static_cast<size_t>(size), p_abort);

        m_buzzic2 = Buzzic2Load(m_filedata.data(), m_filedata.size());
        if (!m_buzzic2)
            throw exception_io_unsupported_format();
    }

    ~input_buzzic2() {
        if (m_buzzic2) Buzzic2Release(m_buzzic2);
    }

    unsigned get_subsong_count() { return 1; }
    t_uint32 get_subsong(unsigned p_index) { return 0; }

    void get_info(t_uint32 p_subsong, file_info& p_info, abort_callback& p_abort) {
        uint32_t dur_ms = Buzzic2DurationMs(m_buzzic2);
        if (dur_ms > 0)
            p_info.set_length(static_cast<double>(dur_ms) / 1000.0);

        p_info.info_set_int("samplerate", BUZZIC2_SAMPLE_RATE);
        p_info.info_set_int("channels", BUZZIC2_CHANNELS);
        p_info.info_set_int("bitspersample", 32);
        p_info.info_set("encoding", "synthesized");
        p_info.info_set("codec", "Buzzic 2");

        uint32_t num_inst = Buzzic2NumIntruments(m_buzzic2);
        if (num_inst > 0) {
            char buf[16];
            snprintf(buf, sizeof(buf), "%u", num_inst);
            p_info.info_set("instruments", buf);
        }
    }

    t_filestats2 get_stats2(uint32_t f, abort_callback& p_abort) {
        return m_file->get_stats2_(f, p_abort);
    }

    void decode_initialize(t_uint32 p_subsong, unsigned p_flags, abort_callback& p_abort) {
        Buzzic2Reset(m_buzzic2);
        m_has_ended = false;
        m_decode_pos_samples = 0;

        uint32_t dur_ms = Buzzic2DurationMs(m_buzzic2);
        m_max_decode_samples = (dur_ms > 0)
            ? static_cast<t_uint64>(static_cast<double>(dur_ms) / 1000.0 * BUZZIC2_SAMPLE_RATE)
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

        m_render_buf.resize(samples_to_produce);
        uint32_t rendered = Buzzic2Render(m_buzzic2, m_render_buf.data(), samples_to_produce);

        if (rendered == 0) {
            m_has_ended = true;
            return false;
        }

        p_chunk.set_data_32(
            reinterpret_cast<const float*>(m_render_buf.data()),
            rendered,
            BUZZIC2_CHANNELS,
            BUZZIC2_SAMPLE_RATE
        );

        m_decode_pos_samples += rendered;
        return true;
    }

    bool decode_can_seek() { return true; }

    void decode_seek(double p_seconds, abort_callback& p_abort) {
        t_uint64 target_samples = static_cast<t_uint64>(p_seconds * BUZZIC2_SAMPLE_RATE);
        if (m_max_decode_samples > 0 && target_samples > m_max_decode_samples)
            target_samples = m_max_decode_samples;

        Buzzic2Reset(m_buzzic2);
        m_decode_pos_samples = 0;
        m_has_ended = false;

        StereoSample discard_buf[DECODE_BLOCK_SAMPLES];
        while (m_decode_pos_samples < target_samples) {
            p_abort.check();
            uint32_t to_skip = static_cast<uint32_t>(
                (std::min)(static_cast<t_uint64>(DECODE_BLOCK_SAMPLES), target_samples - m_decode_pos_samples));
            uint32_t rendered = Buzzic2Render(m_buzzic2, discard_buf, to_skip);
            if (rendered == 0) {
                m_has_ended = true;
                return;
            }
            m_decode_pos_samples += rendered;
        }
    }

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
        return _stricmp(p_extension, "buz2") == 0;
    }

    static const char* g_get_name() { return "Buzzic 2 Decoder"; }

    static const GUID g_get_guid() {
        // {3A5C8D2E-7F41-4B96-AE0D-2C6F8A9B1E3D}
        static const GUID guid = { 0x3a5c8d2e, 0x7f41, 0x4b96, { 0xae, 0x0d, 0x2c, 0x6f, 0x8a, 0x9b, 0x1e, 0x3d } };
        return guid;
    }

private:
    service_ptr_t<file> m_file;
    std::vector<uint8_t> m_filedata;

    Buzzic2* m_buzzic2 = nullptr;

    bool m_has_ended = false;
    t_uint64 m_decode_pos_samples = 0;
    t_uint64 m_max_decode_samples = 0;

    std::vector<StereoSample> m_render_buf;
};

static input_factory_t<input_buzzic2> g_input_buzzic2_factory;

namespace {
    class buzzic2_file_types : public input_file_type {
    public:
        unsigned get_count() override { return 1; }

        bool get_name(unsigned idx, pfc::string_base& out) override {
            if (idx != 0) return false;
            out = "Buzzic 2 music files";
            return true;
        }

        bool get_mask(unsigned idx, pfc::string_base& out) override {
            if (idx != 0) return false;
            out = "*.buz2";
            return true;
        }

        bool is_associatable(unsigned) override { return true; }
    };

    static service_factory_single_t<buzzic2_file_types> g_buzzic2_file_types_factory;
}
