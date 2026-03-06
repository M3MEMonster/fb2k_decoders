#include "stdafx.h"

#include "tfmx_config.h"

#include "3rdParty/libtfmxaudiodecoder-main/src/tfmxaudiodecoder.h"

#include <algorithm>
#include <cstring>
#include <cstdint>
#include <string>
#include <set>
#include <vector>

static constexpr unsigned TFMX_SAMPLE_RATE = 44100;
static constexpr unsigned TFMX_CHANNELS = 2;
static constexpr unsigned TFMX_BITS = 16;
static constexpr unsigned TFMX_FRAME_BYTES = TFMX_CHANNELS * (TFMX_BITS / 8);
static constexpr unsigned TFMX_BLOCK_SAMPLES = 1024;
static constexpr unsigned TFMX_BLOCK_BYTES = TFMX_BLOCK_SAMPLES * TFMX_FRAME_BYTES;

static void split_path_dir_file(const std::string& path, std::string& dir, std::string& file) {
    const size_t p1 = path.find_last_of('\\');
    const size_t p2 = path.find_last_of('/');
    const size_t pos = (p1 == std::string::npos) ? p2 : (p2 == std::string::npos ? p1 : (std::max)(p1, p2));
    if (pos == std::string::npos) {
        dir.clear();
        file = path;
    } else {
        dir = path.substr(0, pos + 1);
        file = path.substr(pos + 1);
    }
}

static std::string to_lower_ascii(std::string s) {
    for (char& c : s) if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
    return s;
}

static bool read_file_to_bytes(const char* pathUtf8, std::vector<uint8_t>& out, abort_callback& abort) {
    try {
        service_ptr_t<file> f;
        filesystem::g_open_read(f, pathUtf8, abort);
        const t_filesize fs = f->get_size(abort);
        if (fs == filesize_invalid || fs == 0 || fs > 256 * 1024 * 1024) return false;
        out.resize((size_t)fs);
        f->read_object(out.data(), out.size(), abort);
        return true;
    } catch (...) {
        return false;
    }
}

static std::vector<std::string> build_companion_candidates(const char* pathUtf8) {
    std::vector<std::string> out;
    if (!pathUtf8 || !pathUtf8[0]) return out;
    const std::string full(pathUtf8);
    std::string dir, file;
    split_path_dir_file(full, dir, file);
    if (file.empty()) return out;

    const std::string fileLower = to_lower_ascii(file);
    const size_t dot = file.find_last_of('.');
    std::string stem = file;
    std::string extLower;
    if (dot != std::string::npos && dot + 1 < file.size()) {
        stem = file.substr(0, dot);
        extLower = to_lower_ascii(file.substr(dot));
    }

    auto add = [&](const std::string& candidateFile) {
        out.push_back(dir + candidateFile);
    };

    if (extLower == ".tfx") add(stem + ".sam");
    if (extLower == ".mdat") add(stem + ".smpl");

    if (fileLower.rfind("mdat.", 0) == 0 && file.size() > 5) {
        add("smpl." + file.substr(5));
    }

    if (extLower == ".hip" || extLower == ".hipc" || extLower == ".hip7") {
        add(stem + ".mcmd");
        add(stem + ".samp");
        add("hipc.samp");
        add("smp.set");
    }
    if (extLower == ".mcmd") {
        add(stem + ".hip");
        add(stem + ".hipc");
        add(stem + ".hip7");
    }

    std::set<std::string> uniq;
    std::vector<std::string> dedup;
    for (const auto& p : out) {
        if (uniq.insert(to_lower_ascii(p)).second) dedup.push_back(p);
    }
    return dedup;
}

static bool is_tfmx_prefix_path(const char* path) {
    if (!path || !path[0]) return false;
    const char* fileName = path;
    const char* s1 = std::strrchr(path, '\\');
    const char* s2 = std::strrchr(path, '/');
    if (s1 || s2) fileName = ((s1 && s2) ? (s1 > s2 ? s1 : s2) : (s1 ? s1 : s2)) + 1;
    if (!fileName || !fileName[0]) return false;

    const char* dot = std::strchr(fileName, '.');
    if (!dot || dot == fileName) return false;
    const size_t len = (size_t)(dot - fileName);
    if (len == 0 || len >= 31) return false;

    char key[32] = {};
    std::memcpy(key, fileName, len);
    key[len] = '\0';

    // Sidecar file prefixes are not primary music entries.
    if (_stricmp(key, "smpl") == 0 || _stricmp(key, "sam") == 0 || _stricmp(key, "info") == 0) return false;
    return tfmx_config::is_format_enabled(key);
}

class input_tfmx : public input_stubs {
public:
    input_tfmx() = default;
    ~input_tfmx() {
        if (m_decoder) {
            tfmxdec_delete(m_decoder);
            m_decoder = nullptr;
        }
    }

    void open(service_ptr_t<file> p_filehint, const char* p_path, t_input_open_reason p_reason, abort_callback& p_abort) {
        if (p_reason == input_open_info_write) throw exception_tagging_unsupported();
        p_abort.check();

        if (m_decoder) {
            tfmxdec_delete(m_decoder);
            m_decoder = nullptr;
        }

        m_file = p_filehint;
        input_open_file_helper(m_file, p_path, p_reason, p_abort);
        m_path = p_path;

        m_decoder = tfmxdec_new();
        if (!m_decoder) throw exception_io_data();

        pfc::string8 displayPath;
        filesystem::g_get_display_path(p_path, displayPath);
        tfmxdec_set_path(m_decoder, displayPath.c_str());
        const t_filesize size = m_file->get_size(p_abort);
        if (size == filesize_invalid || size == 0 || size > 128 * 1024 * 1024) throw exception_io_data();
        std::vector<uint8_t> data((size_t)size);
        m_file->read_object(data.data(), data.size(), p_abort);
        if (!tfmxdec_init(m_decoder, data.data(), (uint32_t)data.size(), 0)) {
            bool inited = false;
            const auto companions = build_companion_candidates(displayPath.c_str());
            for (const auto& cp : companions) {
                std::vector<uint8_t> side;
                if (!read_file_to_bytes(cp.c_str(), side, p_abort) || side.empty()) continue;
                std::vector<uint8_t> merged;
                merged.reserve(data.size() + side.size());
                merged.insert(merged.end(), data.begin(), data.end());
                merged.insert(merged.end(), side.begin(), side.end());
                if (tfmxdec_init(m_decoder, merged.data(), (uint32_t)merged.size(), 0)) {
                    inited = true;
                    break;
                }
            }
            if (!inited) throw exception_io_data();
        }

        const int songs = tfmxdec_songs(m_decoder);
        m_subsongCount = songs > 0 ? (t_uint32)songs : 1;
        m_currentSubsong = 0;
    }

    unsigned get_subsong_count() {
        return m_subsongCount;
    }

    t_uint32 get_subsong(t_uint32 p_index) {
        return p_index;
    }

    void get_info(t_uint32 p_subsong, file_info& p_info, abort_callback&) {
        if (!m_decoder) return;

        if (p_subsong >= m_subsongCount) p_subsong = 0;
        tfmxdec_reinit(m_decoder, (int)p_subsong);

        p_info.info_set_int("samplerate", TFMX_SAMPLE_RATE);
        p_info.info_set_int("channels", TFMX_CHANNELS);
        p_info.info_set_bitrate_vbr(0);

        const char* fmt = tfmxdec_format_name(m_decoder);
        if (fmt && fmt[0]) p_info.info_set("codec", fmt);

        const char* artist = tfmxdec_get_artist(m_decoder);
        const char* title = tfmxdec_get_title(m_decoder);
        const char* game = tfmxdec_get_game(m_decoder);
        if (artist && artist[0]) p_info.meta_set("artist", artist);
        if (title && title[0]) p_info.meta_set("title", title);
        if (game && game[0]) p_info.meta_set("album", game);

        const uint32_t nativeMs = tfmxdec_duration(m_decoder);
        const unsigned fallback = tfmx_config::default_duration_seconds();
        const double shown = nativeMs > 0 ? (double)nativeMs / 1000.0 : (double)fallback;
        p_info.set_length(shown);
    }

    t_filestats2 get_stats2(uint32_t f, abort_callback& p_abort) {
        return m_file->get_stats2_(f, p_abort);
    }

    void decode_initialize(t_uint32 p_subsong, unsigned, abort_callback&) {
        if (!m_decoder) return;
        if (p_subsong >= m_subsongCount) p_subsong = 0;
        m_currentSubsong = p_subsong;

        tfmxdec_reinit(m_decoder, (int)p_subsong);
        tfmxdec_mixer_init(m_decoder, TFMX_SAMPLE_RATE, TFMX_BITS, TFMX_CHANNELS, 0, 75);
        tfmxdec_set_loop_mode(m_decoder, tfmx_config::play_indefinitely() ? 1 : 0);

        m_currentSamples = 0;
        const uint32_t nativeMs = tfmxdec_duration(m_decoder);
        if (nativeMs > 0) {
            m_maxSamples = (uint64_t)nativeMs * TFMX_SAMPLE_RATE / 1000;
        } else {
            m_maxSamples = (uint64_t)tfmx_config::default_duration_seconds() * TFMX_SAMPLE_RATE;
        }
    }

    bool decode_run(audio_chunk& p_chunk, abort_callback&) {
        if (!m_decoder) return false;

        if (!tfmx_config::play_indefinitely()) {
            if (m_maxSamples != 0 && m_currentSamples >= m_maxSamples) return false;
            if (tfmxdec_song_end(m_decoder)) return false;
        }

        unsigned frames = TFMX_BLOCK_SAMPLES;
        if (!tfmx_config::play_indefinitely() && m_maxSamples != 0) {
            const uint64_t remain = m_maxSamples - m_currentSamples;
            if (remain == 0) return false;
            frames = (unsigned)(std::min)(remain, (uint64_t)frames);
        }

        pfc::array_t<t_int16> pcm;
        pcm.set_size(frames * TFMX_CHANNELS);
        tfmxdec_buffer_fill(m_decoder, pcm.get_ptr(), frames * TFMX_FRAME_BYTES);
        p_chunk.set_data_fixedpoint(
            pcm.get_ptr(),
            frames * TFMX_CHANNELS * sizeof(t_int16),
            TFMX_SAMPLE_RATE,
            TFMX_CHANNELS,
            TFMX_BITS,
            audio_chunk::g_guess_channel_config(TFMX_CHANNELS));
        m_currentSamples += frames;
        return true;
    }

    bool decode_can_seek() {
        return true;
    }

    void decode_seek(double p_seconds, abort_callback& p_abort) {
        p_abort.check();
        if (!m_decoder) return;

        if (p_seconds < 0) p_seconds = 0;
        uint64_t target = (uint64_t)(p_seconds * TFMX_SAMPLE_RATE);
        if (!tfmx_config::play_indefinitely() && m_maxSamples != 0) target = (std::min)(target, m_maxSamples);
        const int32_t ms = (int32_t)(target * 1000 / TFMX_SAMPLE_RATE);
        tfmxdec_seek(m_decoder, ms);
        m_currentSamples = target;
    }

    bool decode_on_idle(abort_callback&) {
        return false;
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

    static bool g_is_our_path(const char* p_path, const char* p_extension) {
        if (p_extension && p_extension[0] && tfmx_config::is_format_enabled(p_extension)) return true;
        return is_tfmx_prefix_path(p_path);
    }

    static bool g_is_our_content_type(const char*) {
        return false;
    }

    static const char* g_get_name() {
        return "TFMX Decoder";
    }

    static GUID g_get_guid() {
        // {96D5645C-C584-401F-B4B0-4D41A17FD8A7}
        return { 0x96d5645c, 0xc584, 0x401f, { 0xb4, 0xb0, 0x4d, 0x41, 0xa1, 0x7f, 0xd8, 0xa7 } };
    }

private:
    service_ptr_t<file> m_file;
    pfc::string8 m_path;
    void* m_decoder = nullptr;
    t_uint32 m_subsongCount = 1;
    t_uint32 m_currentSubsong = 0;
    uint64_t m_currentSamples = 0;
    uint64_t m_maxSamples = 0;
};

static input_factory_t<input_tfmx> g_input_tfmx_factory;
