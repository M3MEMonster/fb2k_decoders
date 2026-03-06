#include "stdafx.h"

#include "kss_config.h"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

extern "C" {
#include "3rdParty/libkss/src/kssplay.h"
}

namespace {
static constexpr unsigned KSS_SAMPLE_RATE = 48000;
static constexpr unsigned KSS_CHANNELS = 2;
static constexpr unsigned KSS_BITS = 16;
static constexpr unsigned KSS_BLOCK_SAMPLES = 1024;

class input_kss : public input_stubs {
public:
    ~input_kss() {
        cleanup();
    }

    void open(service_ptr_t<file> p_filehint, const char* p_path, t_input_open_reason p_reason, abort_callback& p_abort) {
        if (p_reason == input_open_info_write) throw exception_tagging_unsupported();
        m_file = p_filehint;
        input_open_file_helper(m_file, p_path, p_reason, p_abort);
        m_path = p_path ? p_path : "";

        const t_filesize size = m_file->get_size(p_abort);
        if (size == filesize_invalid || size < 8 || size > 128 * 1024 * 1024) throw exception_io_unsupported_format();
        m_data.resize(static_cast<size_t>(size));
        m_file->read_object(m_data.data(), m_data.size(), p_abort);

        create_kss();
        if (m_kss == nullptr) throw exception_io_unsupported_format();
        m_title = KSS_get_title(m_kss) ? KSS_get_title(m_kss) : "";
    }

    unsigned get_subsong_count() {
        if (!m_kss) return 1;
        const int count = int(m_kss->trk_max) - int(m_kss->trk_min) + 1;
        return count > 0 ? (unsigned)count : 1u;
    }

    t_uint32 get_subsong(unsigned p_index) { return p_index; }

    void get_info(t_uint32, file_info& p_info, abort_callback&) {
        p_info.info_set_int("samplerate", KSS_SAMPLE_RATE);
        p_info.info_set_int("channels", KSS_CHANNELS);
        p_info.info_set_int("bitspersample", KSS_BITS);
        p_info.info_set("encoding", "synthesized");
        p_info.info_set("codec", "LIBKSS");
        if (!m_title.empty()) p_info.meta_set("title", m_title.c_str());
        p_info.set_length((double)kss_config::default_duration_seconds());
    }

    t_filestats2 get_stats2(uint32_t f, abort_callback& p_abort) {
        return m_file->get_stats2_(f, p_abort);
    }

    void decode_initialize(t_uint32 p_subsong, unsigned, abort_callback&) {
        ensure_player();
        if (!m_player || !m_kss) return;
        m_subsongIndex = p_subsong;
        const unsigned song = (unsigned)m_kss->trk_min + p_subsong;
        m_song = song;
        KSSPLAY_reset(m_player, song, 0);
        KSSPLAY_set_device_quality(m_player, KSS_DEVICE_PSG, 1);
        KSSPLAY_set_device_quality(m_player, KSS_DEVICE_SCC, 1);
        KSSPLAY_set_device_quality(m_player, KSS_DEVICE_OPL, 1);
        KSSPLAY_set_device_quality(m_player, KSS_DEVICE_OPLL, 1);
        m_player->opll_stereo = 1;
        KSSPLAY_set_silent_limit(m_player, 5000);

        m_render.resize(KSS_BLOCK_SAMPLES * KSS_CHANNELS);
        m_currentSamples = 0;
        if (kss_config::play_indefinitely()) m_maxSamples = 0;
        else m_maxSamples = (uint64_t)kss_config::default_duration_seconds() * KSS_SAMPLE_RATE;
    }

    bool decode_run(audio_chunk& p_chunk, abort_callback& p_abort) {
        p_abort.check();
        if (!m_player) return false;
        if (m_maxSamples != 0 && m_currentSamples >= m_maxSamples) return false;

        KSSPLAY_calc(m_player, m_render.data(), KSS_BLOCK_SAMPLES);
        uint32_t emit = KSS_BLOCK_SAMPLES;
        if (m_maxSamples != 0) {
            const uint64_t remain = m_maxSamples - m_currentSamples;
            emit = (uint32_t)(std::min)(remain, (uint64_t)KSS_BLOCK_SAMPLES);
        }
        if (emit == 0) return false;

        p_chunk.set_data_fixedpoint(
            m_render.data(),
            emit * KSS_CHANNELS * sizeof(int16_t),
            KSS_SAMPLE_RATE,
            KSS_CHANNELS,
            KSS_BITS,
            audio_chunk::channel_config_stereo);

        m_currentSamples += emit;

        if (kss_config::play_indefinitely()) {
            if (KSSPLAY_get_stop_flag(m_player)) {
                KSSPLAY_reset(m_player, m_song, 0);
            }
        }
        return true;
    }

    bool decode_can_seek() { return true; }
    void decode_seek(double p_seconds, abort_callback& p_abort) {
        p_abort.check();
        if (!m_player || !m_kss) return;

        if (p_seconds < 0.0) p_seconds = 0.0;
        uint64_t targetSamples = static_cast<uint64_t>(p_seconds * KSS_SAMPLE_RATE);
        if (m_maxSamples != 0) {
            targetSamples = (std::min)(targetSamples, m_maxSamples);
        }

        // Reset and fast-forward silently to requested position.
        const unsigned song = (unsigned)m_kss->trk_min + m_subsongIndex;
        m_song = song;
        KSSPLAY_reset(m_player, song, 0);

        uint64_t remain = targetSamples;
        while (remain > 0) {
            p_abort.check();
            const uint32_t step = static_cast<uint32_t>((std::min)(remain, static_cast<uint64_t>(KSS_BLOCK_SAMPLES)));
            KSSPLAY_calc_silent(m_player, step);
            remain -= step;
            if (kss_config::play_indefinitely()) {
                if (KSSPLAY_get_stop_flag(m_player)) {
                    KSSPLAY_reset(m_player, m_song, 0);
                }
            } else if (KSSPLAY_get_stop_flag(m_player)) {
                break;
            }
        }

        m_currentSamples = targetSamples - remain;
    }

    void retag_set_info(t_uint32, const file_info&, abort_callback&) { throw exception_tagging_unsupported(); }
    void retag_commit(abort_callback&) { throw exception_tagging_unsupported(); }
    void remove_tags(abort_callback&) { throw exception_tagging_unsupported(); }

    static bool g_is_our_content_type(const char*) { return false; }
    static bool g_is_our_path(const char*, const char* ext) {
        return kss_config::is_format_enabled(ext);
    }
    static const char* g_get_name() { return "LIBKSS Decoder"; }
    static const GUID g_get_guid() {
        // {E8AB8AB0-79AE-43F6-9098-4531D9BA8D40}
        static const GUID guid = { 0xe8ab8ab0, 0x79ae, 0x43f6, { 0x90, 0x98, 0x45, 0x31, 0xd9, 0xba, 0x8d, 0x40 } };
        return guid;
    }

private:
    static void load_sidecar(void* userData, const char* name, const uint8_t** buffer, size_t* size) {
        auto* This = reinterpret_cast<input_kss*>(userData);
        if (!name || !buffer || !size) return;
        try {
            std::filesystem::path p = std::filesystem::u8path(This->m_path);
            p = p.parent_path() / name;
            std::string utf8;
            auto u8 = p.u8string();
            utf8.assign(u8.begin(), u8.end());
            service_ptr_t<file> f;
            abort_callback_dummy abort;
            filesystem::g_open_read(f, utf8.c_str(), abort);
            const t_filesize fs = f->get_size(abort);
            if (fs == filesize_invalid || fs == 0 || fs > 16 * 1024 * 1024) return;
            This->m_sidecar.resize((size_t)fs);
            f->read_object(This->m_sidecar.data(), This->m_sidecar.size(), abort);
            *buffer = This->m_sidecar.data();
            *size = This->m_sidecar.size();
        } catch (...) {}
    }

    void cleanup() {
        if (m_player) { KSSPLAY_delete(m_player); m_player = nullptr; }
        if (m_kss) { KSS_delete(m_kss); m_kss = nullptr; }
    }

    void create_kss() {
        cleanup();
        m_kss = KSS_bin2kss(m_data.data(), (uint32_t)m_data.size(), m_path.c_str(), load_sidecar, this);
    }

    void ensure_player() {
        if (!m_kss) return;
        if (!m_player) {
            m_player = KSSPLAY_new(KSS_SAMPLE_RATE, KSS_CHANNELS, KSS_BITS);
            if (m_player) KSSPLAY_set_data(m_player, m_kss);
        }
    }

private:
    service_ptr_t<file> m_file;
    std::string m_path;
    std::string m_title;
    std::vector<uint8_t> m_data, m_sidecar;
    KSS* m_kss = nullptr;
    KSSPLAY* m_player = nullptr;
    std::vector<int16_t> m_render;
    uint64_t m_currentSamples = 0, m_maxSamples = 0;
    unsigned m_song = 0;
    t_uint32 m_subsongIndex = 0;
};

static input_factory_t<input_kss> g_input_kss_factory;

class kss_file_types : public input_file_type {
public:
    unsigned get_count() override { return 1; }
    bool get_name(unsigned idx, pfc::string_base& out) override {
        if (idx) return false;
        out = "KSS music files (LIBKSS)";
        return true;
    }
    bool get_mask(unsigned idx, pfc::string_base& out) override {
        if (idx) return false;
        out = "*.kss;*.mgs;*.bgm;*.opx;*.mpk;*.mbm";
        return true;
    }
    bool is_associatable(unsigned) override { return true; }
};

static service_factory_single_t<kss_file_types> g_kss_file_types;
} // namespace
