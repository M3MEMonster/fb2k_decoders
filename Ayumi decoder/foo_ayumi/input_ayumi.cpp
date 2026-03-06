#include "stdafx.h"

extern "C" {
#include "3rdParty/ayumi/ayumi.h"
#include "3rdParty/ayumi/load_text.h"

    int fxm_init(const uint8_t* fxm, uint32_t len);
    void fxm_loop();
    int* fxm_get_registers();
    void fxm_get_songinfo(const char** songName, const char** songAuthor);
    extern int isAmad;
}

#include <mutex>

static constexpr unsigned AYUMI_SAMPLE_RATE = 48000;
static constexpr unsigned AYUMI_CHANNELS = 2;
static constexpr unsigned DECODE_BLOCK_SAMPLES = 1024;
static constexpr unsigned AYUMI_DEFAULT_DURATION_MS = 180000;

static std::mutex s_fxm_mutex;

class input_ayumi : public input_stubs {
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

        {
            std::lock_guard<std::mutex> lock(s_fxm_mutex);
            if (!fxm_init(m_filedata.data(), static_cast<uint32_t>(m_filedata.size())))
                throw exception_io_unsupported_format();
            m_is_amad = isAmad != 0;

            const char* name = nullptr;
            const char* author = nullptr;
            fxm_get_songinfo(&name, &author);
            if (name && name[0]) m_title = name;
            if (author && author[0]) m_author = author;
        }

        init_ayumi();
    }

    unsigned get_subsong_count() { return 1; }
    t_uint32 get_subsong(unsigned p_index) { return 0; }

    void get_info(t_uint32 p_subsong, file_info& p_info, abort_callback& p_abort) {
        p_info.set_length(static_cast<double>(AYUMI_DEFAULT_DURATION_MS) / 1000.0);

        p_info.info_set_int("samplerate", AYUMI_SAMPLE_RATE);
        p_info.info_set_int("channels", AYUMI_CHANNELS);
        p_info.info_set_int("bitspersample", 32);
        p_info.info_set("encoding", "synthesized");
        p_info.info_set("codec", m_is_amad ? "AY Amadeus" : "Fuxoft AY Language");

        if (!m_title.empty())
            p_info.meta_set("title", m_title.c_str());
        if (!m_author.empty())
            p_info.meta_set("artist", m_author.c_str());
    }

    t_filestats2 get_stats2(uint32_t f, abort_callback& p_abort) {
        return m_file->get_stats2_(f, p_abort);
    }

    void decode_initialize(t_uint32 p_subsong, unsigned p_flags, abort_callback& p_abort) {
        {
            std::lock_guard<std::mutex> lock(s_fxm_mutex);
            fxm_init(m_filedata.data(), static_cast<uint32_t>(m_filedata.size()));
        }
        init_ayumi();
        m_has_ended = false;
        m_decode_pos_samples = 0;
        m_max_decode_samples = static_cast<t_uint64>(
            static_cast<double>(AYUMI_DEFAULT_DURATION_MS) / 1000.0 * AYUMI_SAMPLE_RATE);
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

        m_render_buf.resize(samples_to_produce * AYUMI_CHANNELS);

        {
            std::lock_guard<std::mutex> lock(s_fxm_mutex);
            for (unsigned i = 0; i < samples_to_produce; i++) {
                step_ayumi();
                m_render_buf[i * 2]     = static_cast<float>(m_ay.left * m_volume);
                m_render_buf[i * 2 + 1] = static_cast<float>(m_ay.right * m_volume);
            }
        }

        p_chunk.set_data_32(
            m_render_buf.data(),
            samples_to_produce,
            AYUMI_CHANNELS,
            AYUMI_SAMPLE_RATE);

        m_decode_pos_samples += samples_to_produce;
        return true;
    }

    bool decode_can_seek() { return true; }

    void decode_seek(double p_seconds, abort_callback& p_abort) {
        {
            std::lock_guard<std::mutex> lock(s_fxm_mutex);
            fxm_init(m_filedata.data(), static_cast<uint32_t>(m_filedata.size()));
        }
        init_ayumi();

        t_uint64 target_samples = static_cast<t_uint64>(p_seconds * AYUMI_SAMPLE_RATE);

        {
            std::lock_guard<std::mutex> lock(s_fxm_mutex);
            for (t_uint64 i = 0; i < target_samples; i++) {
                if (i % 10000 == 0) p_abort.check();
                step_ayumi();
            }
        }

        m_decode_pos_samples = target_samples;
        m_has_ended = false;
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
        return _stricmp(p_extension, "fxm") == 0
            || _stricmp(p_extension, "amad") == 0;
    }

    static const char* g_get_name() { return "Ayumi Decoder"; }

    static const GUID g_get_guid() {
        // {F5B3E9A7-2C4D-5F6E-B8A0-1D3C5E7F9A21}
        static const GUID guid = { 0xf5b3e9a7, 0x2c4d, 0x5f6e, { 0xb8, 0xa0, 0x1d, 0x3c, 0x5e, 0x7f, 0x9a, 0x21 } };
        return guid;
    }

private:
    void init_ayumi() {
        memset(&m_ay, 0, sizeof(m_ay));
        ayumi_configure(&m_ay, 0, 1773400, AYUMI_SAMPLE_RATE);
        ayumi_set_pan(&m_ay, 0, 0.8, 1);
        ayumi_set_pan(&m_ay, 1, 0.2, 1);
        ayumi_set_pan(&m_ay, 2, 0.5, 1);
        m_isr_counter = 1.0;
    }

    void step_ayumi() {
        m_isr_counter += m_isr_step;
        if (m_isr_counter >= 1.0) {
            m_isr_counter -= 1.0;

            fxm_loop();
            int* regs = fxm_get_registers();

            ayumi_set_tone(&m_ay, 0, (regs[1] << 8) | regs[0]);
            ayumi_set_tone(&m_ay, 1, (regs[3] << 8) | regs[2]);
            ayumi_set_tone(&m_ay, 2, (regs[5] << 8) | regs[4]);
            ayumi_set_noise(&m_ay, regs[6]);
            ayumi_set_mixer(&m_ay, 0, regs[7] & 1, (regs[7] >> 3) & 1, regs[8] >> 4);
            ayumi_set_mixer(&m_ay, 1, (regs[7] >> 1) & 1, (regs[7] >> 4) & 1, regs[9] >> 4);
            ayumi_set_mixer(&m_ay, 2, (regs[7] >> 2) & 1, (regs[7] >> 5) & 1, regs[10] >> 4);
            ayumi_set_volume(&m_ay, 0, regs[8] & 0xf);
            ayumi_set_volume(&m_ay, 1, regs[9] & 0xf);
            ayumi_set_volume(&m_ay, 2, regs[10] & 0xf);
            ayumi_set_envelope(&m_ay, (regs[12] << 8) | regs[11]);
            if (regs[13] != 255)
                ayumi_set_envelope_shape(&m_ay, regs[13]);
        }
        ayumi_process(&m_ay);
        ayumi_remove_dc(&m_ay);
    }

    service_ptr_t<file> m_file;
    std::vector<uint8_t> m_filedata;

    struct ayumi m_ay = {};
    bool m_is_amad = false;
    std::string m_title;
    std::string m_author;

    static constexpr double m_isr_step = 50.0 / AYUMI_SAMPLE_RATE;
    static constexpr double m_volume = 0.7;
    double m_isr_counter = 1.0;

    bool m_has_ended = false;
    t_uint64 m_decode_pos_samples = 0;
    t_uint64 m_max_decode_samples = 0;

    std::vector<float> m_render_buf;
};

static input_factory_t<input_ayumi> g_input_ayumi_factory;

namespace {
    class ayumi_file_types : public input_file_type {
    public:
        unsigned get_count() override { return 1; }

        bool get_name(unsigned idx, pfc::string_base& out) override {
            if (idx != 0) return false;
            out = "Ayumi AY music files";
            return true;
        }

        bool get_mask(unsigned idx, pfc::string_base& out) override {
            if (idx != 0) return false;
            out = "*.fxm;*.amad";
            return true;
        }

        bool is_associatable(unsigned) override { return true; }
    };

    static service_factory_single_t<ayumi_file_types> g_ayumi_file_types_factory;
}
