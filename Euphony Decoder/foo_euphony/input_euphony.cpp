#include "stdafx.h"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

#include "euphony_config.h"

#include "3rdParty/eupmini/eupplayer.hpp"
#include "3rdParty/eupmini/eupplayer_townsEmulator.hpp"

namespace {
static constexpr unsigned EUP_CHANNELS = 2;
static constexpr unsigned EUP_BITS = 16;
static constexpr unsigned EUP_BLOCK_SAMPLES = 1024;

struct EUPHEAD {
    char title[32];
    char artist[8];
    char dummy[44];
    char trk_name[32][16];
    char short_trk_name[32][8];
    uint8_t trk_mute[32];
    uint8_t trk_port[32];
    uint8_t trk_midi_ch[32];
    uint8_t trk_key_bias[32];
    uint8_t trk_transpose[32];
    uint8_t trk_play_filter[32][7];
    char instruments_name[128][4];
    uint8_t fm_midi_ch[6];
    uint8_t pcm_midi_ch[8];
    char fm_file_name[8];
    char pcm_file_name[8];
    char reserved[260];
    char appli_name[8];
    char appli_version[2];
    int32_t size;
    char signature;
    uint8_t first_tempo;
};

static std::string trim_fixed_ascii(const char* data, size_t size) {
    size_t end = size;
    while (end > 0 && (data[end - 1] == '\0' || data[end - 1] == ' ')) {
        --end;
    }
    return std::string(data, end);
}

class input_euphony : public input_stubs {
public:
    ~input_euphony() {
        cleanup_player();
    }

    void open(service_ptr_t<file> p_filehint, const char* p_path, t_input_open_reason p_reason, abort_callback& p_abort) {
        if (p_reason == input_open_info_write) {
            throw exception_tagging_unsupported();
        }

        m_file = p_filehint;
        input_open_file_helper(m_file, p_path, p_reason, p_abort);
        m_path = p_path ? p_path : "";

        const t_filesize fileSize = m_file->get_size(p_abort);
        if (fileSize == filesize_invalid || fileSize <= static_cast<t_filesize>(sizeof(EUPHEAD))) {
            throw exception_io_unsupported_format();
        }
        if (fileSize > 64 * 1024 * 1024) {
            throw exception_io_unsupported_format();
        }

        m_data.resize(static_cast<size_t>(fileSize));
        m_file->read_object(m_data.data(), m_data.size(), p_abort);

        if (!is_valid_eup(m_data)) {
            throw exception_io_unsupported_format();
        }
    }

    unsigned get_subsong_count() { return 1; }
    t_uint32 get_subsong(unsigned p_index) { return p_index; }

    void get_info(t_uint32, file_info& p_info, abort_callback&) {
        const auto* header = reinterpret_cast<const EUPHEAD*>(m_data.data());
        p_info.info_set_int("samplerate", streamAudioRate);
        p_info.info_set_int("channels", EUP_CHANNELS);
        p_info.info_set_int("bitspersample", EUP_BITS);
        p_info.info_set("encoding", "synthesized");
        p_info.info_set("codec", "Euphony");

        const auto title = trim_fixed_ascii(header->title, sizeof(header->title));
        if (!title.empty()) {
            p_info.meta_set("title", title.c_str());
        }
        const auto artist = trim_fixed_ascii(header->artist, sizeof(header->artist));
        if (!artist.empty()) {
            p_info.meta_set("artist", artist.c_str());
        }
        // Keep a visible fallback length in UI even when "Play indefinitely" is enabled.
        p_info.set_length(static_cast<double>(euphony_config::default_duration_seconds()));
    }

    t_filestats2 get_stats2(uint32_t f, abort_callback& p_abort) {
        return m_file->get_stats2_(f, p_abort);
    }

    void decode_initialize(t_uint32, unsigned, abort_callback& p_abort) {
        create_player(p_abort);
        m_ended = false;
        m_currentSamples = 0;
        m_render.resize(EUP_BLOCK_SAMPLES * EUP_CHANNELS);
        if (euphony_config::play_indefinitely()) {
            m_maxSamples = 0;
        } else {
            const uint64_t seconds = static_cast<uint64_t>(euphony_config::default_duration_seconds());
            m_maxSamples = seconds * streamAudioRate;
        }
    }

    bool decode_run(audio_chunk& p_chunk, abort_callback& p_abort) {
        p_abort.check();
        if (m_ended || m_player == nullptr || m_device == nullptr) {
            return false;
        }
        if (m_maxSamples != 0 && m_currentSamples >= m_maxSamples) {
            m_ended = true;
            return false;
        }

        const uint32_t rendered = render_samples(m_render.data(), EUP_BLOCK_SAMPLES);
        if (rendered == 0) {
            m_ended = true;
            return false;
        }
        uint32_t emit = rendered;
        if (m_maxSamples != 0) {
            const uint64_t remaining = m_maxSamples - m_currentSamples;
            emit = static_cast<uint32_t>((std::min)(remaining, static_cast<uint64_t>(rendered)));
        }
        if (emit == 0) {
            m_ended = true;
            return false;
        }

        p_chunk.set_data_fixedpoint(
            m_render.data(),
            emit * EUP_CHANNELS * sizeof(int16_t),
            streamAudioRate,
            EUP_CHANNELS,
            EUP_BITS,
            audio_chunk::channel_config_stereo);
        m_currentSamples += emit;
        if (m_maxSamples != 0 && m_currentSamples >= m_maxSamples) {
            m_ended = true;
        }
        return true;
    }

    bool decode_can_seek() { return true; }

    void decode_seek(double p_seconds, abort_callback& p_abort) {
        const uint64_t target = static_cast<uint64_t>(p_seconds * streamAudioRate);
        if (target < m_currentSamples) {
            create_player(p_abort);
            m_currentSamples = 0;
            m_ended = false;
        }

        std::vector<int16_t> discard(EUP_BLOCK_SAMPLES * EUP_CHANNELS);
        while (m_currentSamples < target) {
            const uint32_t need = static_cast<uint32_t>((std::min)(static_cast<uint64_t>(EUP_BLOCK_SAMPLES), target - m_currentSamples));
            const uint32_t got = render_samples(discard.data(), need);
            if (got == 0) {
                m_ended = true;
                break;
            }
            m_currentSamples += got;
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

    static bool g_is_our_path(const char*, const char* p_extension) {
        return p_extension != nullptr && _stricmp(p_extension, "eup") == 0;
    }

    static const char* g_get_name() { return "Euphony Decoder"; }

    static const GUID g_get_guid() {
        // {B0F0C251-12AA-4F9E-B1A8-7E8B6D26C211}
        static const GUID guid = {
            0xb0f0c251, 0x12aa, 0x4f9e, { 0xb1, 0xa8, 0x7e, 0x8b, 0x6d, 0x26, 0xc2, 0x11 }
        };
        return guid;
    }

private:
    static bool is_valid_eup(const std::vector<uint8_t>& data) {
        if (data.size() <= sizeof(EUPHEAD)) {
            return false;
        }
        const auto* header = reinterpret_cast<const EUPHEAD*>(data.data());
        const int32_t seqSize = header->size;
        if (seqSize < 0) {
            return false;
        }
        return (static_cast<int64_t>(data.size()) - seqSize) == 3598;
    }

    void cleanup_player() {
        if (m_player != nullptr) {
            delete m_player;
            m_player = nullptr;
        }
        if (m_device != nullptr) {
            delete m_device;
            m_device = nullptr;
        }
    }

    static bool load_sidecar_binary(const std::string& sourcePath, const char* fixedName, const char* ext, std::vector<uint8_t>& out, abort_callback& p_abort) {
        const std::string stem = trim_fixed_ascii(fixedName, 8);
        if (stem.empty()) {
            return false;
        }

        const std::filesystem::path src = std::filesystem::u8path(sourcePath);
        std::filesystem::path sidecar = src.parent_path() / stem;
        sidecar.replace_extension(ext);

        std::string sideUtf8;
        {
            const auto u8 = sidecar.u8string();
            sideUtf8.assign(u8.begin(), u8.end());
        }

        try {
            service_ptr_t<file> sideFile;
            filesystem::g_open_read(sideFile, sideUtf8.c_str(), p_abort);
            const t_filesize sideSize = sideFile->get_size(p_abort);
            if (sideSize == filesize_invalid || sideSize == 0 || sideSize > 16 * 1024 * 1024) {
                return false;
            }
            out.resize(static_cast<size_t>(sideSize));
            sideFile->read_object(out.data(), out.size(), p_abort);
            return true;
        } catch (...) {
            return false;
        }
    }

    void create_player(abort_callback& p_abort) {
        cleanup_player();
        if (!is_valid_eup(m_data)) {
            throw exception_io_unsupported_format();
        }

        const auto* header = reinterpret_cast<const EUPHEAD*>(m_data.data());
        if (m_data.size() < (2048 + 6 + 6)) {
            throw exception_io_unsupported_format();
        }

        m_device = new EUP_TownsEmulator();
        m_player = new EUPPlayer();
        m_device->outputSampleUnsigned(false);
        m_device->outputSampleLSBFirst(true);
        m_device->outputSampleSize(streamAudioSampleOctectSize);
        m_device->outputSampleChannels(streamAudioChannelsNum);
        m_device->rate(streamAudioRate);
        m_player->outputDevice(m_device);

        for (int trk = 0; trk < 32; ++trk) {
            m_player->mapTrack_toChannel(trk, header->trk_midi_ch[trk]);
        }
        for (int trk = 0; trk < 6; ++trk) {
            m_device->assignFmDeviceToChannel(header->fm_midi_ch[trk]);
        }
        for (int trk = 0; trk < 8; ++trk) {
            m_device->assignPcmDeviceToChannel(header->pcm_midi_ch[trk]);
        }

        m_player->tempo(header->first_tempo + 30);

        {
            uint8_t instrument[] = {
                ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ',
                17, 33, 10, 17,
                25, 10, 57, 0,
                154, 152, 218, 216,
                15, 12, 7, 12,
                0, 5, 3, 5,
                38, 40, 70, 40,
                20,
                0xc0,
                0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
            };
            for (int n = 0; n < 128; ++n) {
                m_device->setFmInstrumentParameter(n, instrument);
            }
        }

        load_sidecar_binary(m_path, header->fm_file_name, ".fmb", m_fmbData, p_abort);
        if (m_fmbData.size() > 8) {
            for (size_t n = 0; n < (m_fmbData.size() - 8) / 48; ++n) {
                m_device->setFmInstrumentParameter(static_cast<int>(n), m_fmbData.data() + 8 + 48 * n);
            }
        }

        load_sidecar_binary(m_path, header->pcm_file_name, ".pmb", m_pmbData, p_abort);
        if (!m_pmbData.empty()) {
            m_device->setPcmInstrumentParameters(m_pmbData.data(), m_pmbData.size());
        }

        m_player->startPlaying(m_data.data() + 2048 + 6);
        memset(&m_device->pcm, 0, sizeof(m_device->pcm));
    }

    uint32_t render_samples(int16_t* dst, uint32_t numSamples) {
        if (m_player == nullptr || m_device == nullptr) {
            return 0;
        }

        auto& pcm = m_device->pcm;
        uint32_t done = 0;
        while (m_player->isPlaying() && done < numSamples) {
            if (pcm.read_pos < pcm.write_pos) {
                const uint32_t available = static_cast<uint32_t>(pcm.write_pos - pcm.read_pos);
                const uint32_t copyCount = (std::min)(numSamples - done, available);
                memcpy(dst + done * EUP_CHANNELS, pcm.buffer + pcm.read_pos * EUP_CHANNELS, copyCount * EUP_CHANNELS * sizeof(int16_t));
                done += copyCount;
                pcm.read_pos += static_cast<int>(copyCount);
            } else {
                pcm.write_pos = 0;
                pcm.read_pos = streamAudioSamplesBuffer;
                m_player->nextTick();
                pcm.read_pos = 0;
            }
        }
        return done;
    }

private:
    service_ptr_t<file> m_file;
    std::string m_path;
    std::vector<uint8_t> m_data;
    std::vector<uint8_t> m_fmbData;
    std::vector<uint8_t> m_pmbData;

    EUPPlayer* m_player = nullptr;
    EUP_TownsEmulator* m_device = nullptr;
    std::vector<int16_t> m_render;

    uint64_t m_currentSamples = 0;
    uint64_t m_maxSamples = 0;
    bool m_ended = false;
};

static input_factory_t<input_euphony> g_input_euphony_factory;

class euphony_file_types : public input_file_type {
public:
    unsigned get_count() override { return 1; }

    bool get_name(unsigned p_index, pfc::string_base& p_out) override {
        if (p_index != 0) {
            return false;
        }
        p_out = "FM TOWNS Euphony files";
        return true;
    }

    bool get_mask(unsigned p_index, pfc::string_base& p_out) override {
        if (p_index != 0) {
            return false;
        }
        p_out = "*.eup";
        return true;
    }

    bool is_associatable(unsigned) override { return true; }
};

static service_factory_single_t<euphony_file_types> g_euphony_file_types_factory;
} // namespace
