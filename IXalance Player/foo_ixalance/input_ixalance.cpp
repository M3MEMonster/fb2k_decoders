#include "stdafx.h"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "ixalance_config.h"

#include "3rdParty/webixs/PlayerIXS.h"

namespace {
static constexpr unsigned IXS_SAMPLE_RATE = 48000;
static constexpr unsigned IXS_CHANNELS = 2;
static constexpr unsigned IXS_BITS = 16;
static constexpr unsigned IXS_BLOCK_SAMPLES = 1024;
static constexpr uint32_t IXS_MAGIC = 0x21535849; // "IXS!"

class input_ixalance : public input_stubs {
public:
    ~input_ixalance() {
        cleanup_player();
    }

    void open(service_ptr_t<file> p_filehint, const char* p_path, t_input_open_reason p_reason, abort_callback& p_abort) {
        if (p_reason == input_open_info_write) {
            throw exception_tagging_unsupported();
        }

        m_file = p_filehint;
        input_open_file_helper(m_file, p_path, p_reason, p_abort);

        const t_filesize fileSize = m_file->get_size(p_abort);
        if (fileSize == filesize_invalid || fileSize < 4 || fileSize > 128 * 1024 * 1024) {
            throw exception_io_unsupported_format();
        }

        m_data.resize(static_cast<size_t>(fileSize));
        m_file->read_object(m_data.data(), m_data.size(), p_abort);
        if (!is_valid_ixs(m_data)) {
            throw exception_io_unsupported_format();
        }

        create_player();
        if (m_player == nullptr) {
            throw exception_io_unsupported_format();
        }

        const char* title = m_player->vftable->getSongTitle(m_player);
        if (title != nullptr) {
            m_title = title;
        } else {
            m_title.clear();
        }
        m_player->vftable->initAudioOut(m_player);
    }

    unsigned get_subsong_count() { return 1; }
    t_uint32 get_subsong(unsigned p_index) { return p_index; }

    void get_info(t_uint32, file_info& p_info, abort_callback&) {
        p_info.info_set_int("samplerate", IXS_SAMPLE_RATE);
        p_info.info_set_int("channels", IXS_CHANNELS);
        p_info.info_set_int("bitspersample", IXS_BITS);
        p_info.info_set("encoding", "synthesized");
        p_info.info_set("codec", "iXalance");
        if (!m_title.empty()) {
            p_info.meta_set("title", m_title.c_str());
        }
        p_info.set_length(static_cast<double>(ixalance_config::default_duration_seconds()));
    }

    t_filestats2 get_stats2(uint32_t f, abort_callback& p_abort) {
        return m_file->get_stats2_(f, p_abort);
    }

    void decode_initialize(t_uint32, unsigned, abort_callback&) {
        if (m_player == nullptr) {
            return;
        }
        m_player->vftable->initAudioOut(m_player);
        m_buffer = nullptr;
        m_bufferSamples = 0;
        m_ended = false;
        m_currentSamples = 0;
        if (ixalance_config::play_indefinitely()) {
            m_maxSamples = 0;
        } else {
            m_maxSamples = static_cast<uint64_t>(ixalance_config::default_duration_seconds()) * IXS_SAMPLE_RATE;
        }
        m_render.resize(IXS_BLOCK_SAMPLES * IXS_CHANNELS);
    }

    bool decode_run(audio_chunk& p_chunk, abort_callback& p_abort) {
        p_abort.check();
        if (m_ended || m_player == nullptr) {
            return false;
        }
        if (m_maxSamples != 0 && m_currentSamples >= m_maxSamples) {
            m_ended = true;
            return false;
        }

        const uint32_t rendered = render_samples(m_render.data(), IXS_BLOCK_SAMPLES);
        if (rendered == 0) {
            m_ended = true;
            return false;
        }

        uint32_t emit = rendered;
        if (m_maxSamples != 0) {
            const uint64_t remain = m_maxSamples - m_currentSamples;
            emit = static_cast<uint32_t>((std::min)(remain, static_cast<uint64_t>(rendered)));
        }
        if (emit == 0) {
            m_ended = true;
            return false;
        }

        p_chunk.set_data_fixedpoint(
            m_render.data(),
            emit * IXS_CHANNELS * sizeof(int16_t),
            IXS_SAMPLE_RATE,
            IXS_CHANNELS,
            IXS_BITS,
            audio_chunk::channel_config_stereo);

        m_currentSamples += emit;
        if (m_maxSamples != 0 && m_currentSamples >= m_maxSamples) {
            m_ended = true;
        }
        return true;
    }

    bool decode_can_seek() { return false; }

    void decode_seek(double, abort_callback&) {
        throw exception_io_object_not_seekable();
    }

    void retag_set_info(t_uint32, const file_info&, abort_callback&) { throw exception_tagging_unsupported(); }
    void retag_commit(abort_callback&) { throw exception_tagging_unsupported(); }
    void remove_tags(abort_callback&) { throw exception_tagging_unsupported(); }

    static bool g_is_our_content_type(const char*) { return false; }

    static bool g_is_our_path(const char*, const char* p_extension) {
        return p_extension != nullptr && _stricmp(p_extension, "ixs") == 0;
    }

    static const char* g_get_name() { return "iXalance Decoder"; }

    static const GUID g_get_guid() {
        // {7DFCB9F0-28A0-4D79-8E90-B86BAEBE307D}
        static const GUID guid = { 0x7dfcb9f0, 0x28a0, 0x4d79, { 0x8e, 0x90, 0xb8, 0x6b, 0xae, 0xbe, 0x30, 0x7d } };
        return guid;
    }

private:
    static bool is_valid_ixs(const std::vector<uint8_t>& data) {
        if (data.size() < 4) return false;
        uint32_t magic = 0;
        memcpy(&magic, data.data(), sizeof(magic));
        return magic == IXS_MAGIC;
    }

    void cleanup_player() {
        if (m_player != nullptr) {
            m_player->vftable->delete0(m_player);
            m_player = nullptr;
        }
    }

    bool create_player() {
        cleanup_player();
        m_player = IXS::IXS__PlayerIXS__createPlayer_00405d90(IXS_SAMPLE_RATE);
        if (m_player == nullptr) {
            return false;
        }
        float progress = 0.0f;
        const int r = m_player->vftable->loadIxsFileData(m_player, m_data.data(), static_cast<uint>(m_data.size()), nullptr, nullptr, &progress);
        if (r != 0) {
            cleanup_player();
            return false;
        }
        return true;
    }

    uint32_t render_samples(int16_t* dst, uint32_t needSamples) {
        uint32_t done = 0;
        while (done < needSamples) {
            if (m_bufferSamples > 0) {
                const uint32_t take = (std::min)(needSamples - done, m_bufferSamples);
                memcpy(dst + done * IXS_CHANNELS, m_buffer, take * IXS_CHANNELS * sizeof(int16_t));
                m_buffer += take * IXS_CHANNELS;
                m_bufferSamples -= take;
                done += take;
                continue;
            }

            m_player->vftable->genAudio(m_player);
            m_buffer = reinterpret_cast<int16_t*>(m_player->vftable->getAudioBuffer(m_player));
            m_bufferSamples = m_player->vftable->getAudioBufferLen(m_player);
            if (m_buffer == nullptr || m_bufferSamples == 0) {
                break;
            }
        }
        return done;
    }

private:
    service_ptr_t<file> m_file;
    std::vector<uint8_t> m_data;
    std::string m_title;

    IXS::PlayerIXS* m_player = nullptr;
    int16_t* m_buffer = nullptr;
    uint32_t m_bufferSamples = 0;
    std::vector<int16_t> m_render;

    uint64_t m_currentSamples = 0;
    uint64_t m_maxSamples = 0;
    bool m_ended = false;
};

static input_factory_t<input_ixalance> g_input_ixalance_factory;

class ixalance_file_types : public input_file_type {
public:
    unsigned get_count() override { return 1; }

    bool get_name(unsigned idx, pfc::string_base& out) override {
        if (idx != 0) return false;
        out = "iXalance music files";
        return true;
    }

    bool get_mask(unsigned idx, pfc::string_base& out) override {
        if (idx != 0) return false;
        out = "*.ixs";
        return true;
    }

    bool is_associatable(unsigned) override { return true; }
};

static service_factory_single_t<ixalance_file_types> g_ixalance_file_types_factory;
} // namespace
