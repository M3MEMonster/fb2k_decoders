#include <foobar2000.h>
#include "cfg_vars.h"

#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>
#include <windows.h>

namespace {

static constexpr uint32_t kMonotoneSampleRate = 44100;
static constexpr uint32_t kMonotoneChannels = 1;
static constexpr uint32_t kMaxTracks = 12;
static std::mutex g_monotone_mutex;

struct MonotoneSong {
    std::string title;
    std::string comment;
    uint8_t version = 0;
    uint8_t totalPatterns = 0;
    uint8_t totalTracks = 0;
    uint8_t cellSize = 0;
    std::array<uint8_t, 256> order{};
    std::vector<uint16_t> patternData;
};

struct MonotoneChannel {
    bool enabled = false;
    bool active = false;
    int freq[3] = { 0, 0, 0 };
    int portamento = 0;
    int arpCounter = 0;
    int arp = 0;
    int lastNote = 0;
    int portamentoToNote = 0;
    int vibrato = 0;
    int vibratoIndex = 0;
    int vibratoDepth = 1;
    int vibratoSpeed = 1;
};

struct MonotoneOutputChannel {
    bool enabled = false;
    float samplePos = 0.0f;
    float samplePosInc = 0.0f;
};

struct MonotoneState {
    std::array<MonotoneChannel, kMaxTracks> channel{};
    std::array<MonotoneOutputChannel, kMaxTracks> output{};
    int nextChannel = 0;
    int tempo = 4;
    int order = 0;
    int row = 0;
    uint64_t sampleCount = 0;
    int rowTick = 0;
};

static std::wstring g_utf8_to_wide(const char* str) {
    if (!str || !str[0]) return L"";
    int wlen = MultiByteToWideChar(CP_UTF8, 0, str, -1, nullptr, 0);
    if (wlen <= 0) return L"";
    std::wstring ws((size_t)wlen - 1, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, str, -1, ws.data(), wlen);
    return ws;
}

static bool g_read_header(const char* path, uint8_t* out, size_t bytes) {
    if (path == nullptr || out == nullptr || bytes == 0) return false;
    const char* nativePath = path;
    if (pfc::strcmp_partial(path, "file://") == 0) nativePath = path + 7;

    FILE* fp = nullptr;
    std::wstring wpath = g_utf8_to_wide(nativePath);
    if (!wpath.empty()) fp = _wfopen(wpath.c_str(), L"rb");
    if (!fp) fp = fopen(nativePath, "rb");
    if (!fp) return false;

    size_t got = fread(out, 1, bytes, fp);
    fclose(fp);
    return got == bytes;
}

static inline uint16_t g_read_u16_le(const uint8_t* p) {
    return static_cast<uint16_t>(p[0] | (static_cast<uint16_t>(p[1]) << 8));
}

static inline int g_note_index_clamped(int note) {
    int idx = note * 8;
    if (idx < 0) idx = 0;
    if (idx > 799) idx = 799;
    return idx;
}

static inline float g_wave_square(float p) {
    return p < 0.5f ? 0.5f : -0.5f;
}

class input_monotone;
static input_monotone* g_monotone_active_instance = nullptr;

class input_monotone : public input_stubs {
public:
    void open(service_ptr_t<file> p_filehint, const char* p_path,
        t_input_open_reason p_reason, abort_callback& p_abort) {
        if (p_reason == input_open_info_write) throw exception_tagging_unsupported();

        m_file = p_filehint;
        input_open_file_helper(m_file, p_path, p_reason, p_abort);

        t_filesize fileSize = m_file->get_size(p_abort);
        if (fileSize == filesize_invalid || fileSize < 352 || fileSize > 64 * 1024 * 1024) {
            throw exception_io_unsupported_format();
        }

        m_fileData.resize(static_cast<size_t>(fileSize));
        m_file->read_object(m_fileData.data(), m_fileData.size(), p_abort);
        parse_song_or_throw();
    }

    ~input_monotone() {
        std::lock_guard<std::mutex> lock(g_monotone_mutex);
        if (g_monotone_active_instance == this) {
            m_decoding = false;
            g_monotone_active_instance = nullptr;
        }
    }

    unsigned get_subsong_count() { return 1; }
    t_uint32 get_subsong(unsigned) { return 0; }

    void get_info(t_uint32, file_info& p_info, abort_callback&) {
        int dur = nostalgia_cfg::cfg_default_duration_sec.get();
        if (dur > 0) p_info.set_length(dur);

        p_info.info_set_int("samplerate", kMonotoneSampleRate);
        p_info.info_set_int("channels", kMonotoneChannels);
        p_info.info_set_int("bitspersample", 32);
        p_info.info_set("encoding", "synthesized");
        p_info.info_set("codec", "MONOTONE");

        if (!m_song.title.empty()) p_info.meta_set("title", m_song.title.c_str());
        if (!m_song.comment.empty()) p_info.meta_set("comment", m_song.comment.c_str());
    }

    t_filestats2 get_stats2(unsigned f, abort_callback& a) { return m_file->get_stats2_(f, a); }
    t_filestats get_file_stats(abort_callback& a) { return m_file->get_stats(a); }

    void decode_initialize(t_uint32, unsigned, abort_callback&) {
        std::lock_guard<std::mutex> lock(g_monotone_mutex);

        if (g_monotone_active_instance && g_monotone_active_instance != this) {
            g_monotone_active_instance->m_decoding = false;
        }
        g_monotone_active_instance = this;

        init_tables();
        reset_state();
        m_decoding = true;
    }

    bool decode_run(audio_chunk& p_chunk, abort_callback& p_abort) {
        if (!m_decoding) return false;

        std::lock_guard<std::mutex> lock(g_monotone_mutex);
        if (g_monotone_active_instance != this) {
            m_decoding = false;
            return false;
        }

        static constexpr uint32_t kBlockFrames = 1024;
        m_renderBuffer.resize(kBlockFrames);
        render_block(m_renderBuffer.data(), kBlockFrames, p_abort);

        m_renderedFrames += kBlockFrames;
        uint64_t maxFrames = static_cast<uint64_t>(kMonotoneSampleRate) * static_cast<uint64_t>(nostalgia_cfg::cfg_default_duration_sec.get());
        if (maxFrames > 0 && m_renderedFrames > maxFrames) {
            m_decoding = false;
            return false;
        }

        p_chunk.set_data_32(m_renderBuffer.data(), kBlockFrames, kMonotoneChannels, kMonotoneSampleRate);
        return true;
    }

    void decode_seek(double p_seconds, abort_callback& p_abort) {
        std::lock_guard<std::mutex> lock(g_monotone_mutex);
        if (g_monotone_active_instance != this) return;

        reset_state();
        m_decoding = true;

        uint64_t targetFrames = static_cast<uint64_t>(p_seconds * kMonotoneSampleRate);
        std::array<float, 1024> discard{};
        while (m_renderedFrames < targetFrames) {
            p_abort.check();
            uint64_t remain = targetFrames - m_renderedFrames;
            uint32_t now = static_cast<uint32_t>(pfc::min_t<uint64_t>(discard.size(), remain));
            render_block(discard.data(), now, p_abort);
            m_renderedFrames += now;
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
    static bool g_is_our_path(const char* p_path, const char* p_extension) {
        if (!nostalgia_cfg::cfg_monotone_enabled.get()) return false;
        if (p_extension == nullptr || stricmp_utf8(p_extension, "mon") != 0) return false;

        static const uint8_t magic[9] = { 8, 'M', 'O', 'N', 'O', 'T', 'O', 'N', 'E' };
        uint8_t hdr[sizeof(magic)] = {};
        if (!g_read_header(p_path, hdr, sizeof(hdr))) return true;
        return memcmp(hdr, magic, sizeof(magic)) == 0;
    }
    static const char* g_get_name() { return "MONOTONE decoder"; }
    static const GUID g_get_guid() {
        static const GUID guid = { 0x8c72bb31, 0x0d45, 0x4a4d, { 0x91, 0x0d, 0x7a, 0x3a, 0x2e, 0x66, 0x4e, 0x9b } };
        return guid;
    }

private:
    void parse_song_or_throw() {
        static const uint8_t magic[9] = { 8, 'M', 'O', 'N', 'O', 'T', 'O', 'N', 'E' };
        if (m_fileData.size() < 9 + 41 + 41 + 4 + 256) throw exception_io_unsupported_format();
        if (memcmp(m_fileData.data(), magic, sizeof(magic)) != 0) throw exception_io_unsupported_format();

        size_t pos = 9;
        auto parse_pascal41 = [&](std::string& out) {
            if (pos + 41 > m_fileData.size()) throw exception_io_unsupported_format();
            const uint8_t* p = m_fileData.data() + pos;
            uint8_t len = p[0];
            if (len > 40) len = 40;
            out.assign(reinterpret_cast<const char*>(p + 1), reinterpret_cast<const char*>(p + 1 + len));
            pos += 41;
        };

        MonotoneSong parsed;
        parse_pascal41(parsed.title);
        parse_pascal41(parsed.comment);

        if (pos + 4 > m_fileData.size()) throw exception_io_unsupported_format();
        parsed.version = m_fileData[pos + 0];
        parsed.totalPatterns = m_fileData[pos + 1];
        parsed.totalTracks = m_fileData[pos + 2];
        parsed.cellSize = m_fileData[pos + 3];
        pos += 4;

        if (parsed.version != 1 || parsed.cellSize != 2) throw exception_io_unsupported_format();
        if (parsed.totalTracks == 0 || parsed.totalTracks > kMaxTracks) throw exception_io_unsupported_format();
        if (parsed.totalPatterns == 0) throw exception_io_unsupported_format();

        if (pos + parsed.order.size() > m_fileData.size()) throw exception_io_unsupported_format();
        memcpy(parsed.order.data(), m_fileData.data() + pos, parsed.order.size());
        pos += parsed.order.size();

        const size_t totalCells = static_cast<size_t>(64) * parsed.totalPatterns * parsed.totalTracks;
        const size_t requiredBytes = totalCells * 2;
        if (pos + requiredBytes > m_fileData.size()) throw exception_io_unsupported_format();

        parsed.patternData.resize(totalCells);
        for (size_t i = 0; i < totalCells; ++i) {
            parsed.patternData[i] = g_read_u16_le(m_fileData.data() + pos + i * 2);
        }

        m_song = std::move(parsed);
    }

    void init_tables() {
        float temphz = 27.5f;
        constexpr int IBO = 12;
        constexpr int IBN = 8;
        constexpr float interval = 1.00724641222f;
        const int maxnote = 3 + (8 * IBO) + 1;

        m_notesHz.fill(0);
        m_vibTable.fill(0);
        m_notesHz[0] = 440;
        m_notesHz[1 * IBN] = static_cast<int>(std::floor(temphz + 0.5f));

        for (int i = (1 * IBN) - 1; i > 1; --i) {
            temphz /= interval;
            if (temphz < 19) temphz = 19;
            m_notesHz[i] = static_cast<int>(std::floor(temphz + 0.5f));
        }

        temphz = 27.5f;
        for (int i = (1 * IBN) + 1; i < maxnote * IBN; ++i) {
            temphz *= interval;
            m_notesHz[i] = static_cast<int>(std::floor(temphz + 0.5f));
        }

        for (int i = 0; i < 32; ++i) {
            m_vibTable[i] = static_cast<int>(std::floor(0.5 + 64 * std::sin(i * 3.14159265358979323846 / 32 * 2)));
        }
    }

    void reset_state() {
        m_state = {};
        m_state.tempo = 4;
        m_renderedFrames = 0;
        for (uint32_t i = 0; i < kMaxTracks; ++i) {
            m_state.output[i].enabled = i < m_song.totalTracks;
            m_state.channel[i].enabled = i < m_song.totalTracks;
            m_state.channel[i].vibratoDepth = 1;
            m_state.channel[i].vibratoSpeed = 1;
        }
    }

    void render_block(float* out, uint32_t frames, abort_callback& p_abort) {
        if (m_song.totalTracks == 0) {
            memset(out, 0, sizeof(float) * frames);
            return;
        }

        const int samplesPerTick = static_cast<int>(std::floor(kMonotoneSampleRate / 60.0));
        for (uint32_t i = 0; i < frames; ++i) {
            p_abort.check();
            if ((m_state.sampleCount % static_cast<uint64_t>(samplesPerTick)) == 0) {
                process_tick();
                assign_hardware_channels();
            }

            float s = 0.0f;
            for (uint32_t ch = 0; ch < kMaxTracks; ++ch) {
                if (!m_state.output[ch].enabled) continue;
                const float nextPos = m_state.output[ch].samplePos + m_state.output[ch].samplePosInc;
                m_state.output[ch].samplePos = nextPos - std::floor(nextPos);
                s += g_wave_square(m_state.output[ch].samplePos);
            }
            out[i] = s;
            ++m_state.sampleCount;
        }
    }

    void process_tick() {
        m_state.rowTick++;
        if (m_state.rowTick >= m_state.tempo) {
            m_state.rowTick = 0;
            process_row();
        }

        for (uint8_t j = 0; j < m_song.totalTracks; ++j) {
            auto& ch = m_state.channel[j];
            if (!ch.active) continue;

            if (ch.vibrato) {
                const int vib = m_vibTable[ch.vibratoIndex] * ch.vibratoDepth / 64;
                int idx = g_note_index_clamped(ch.lastNote);
                idx += vib;
                if (idx < 0) idx = 0;
                if (idx > 799) idx = 799;
                ch.freq[0] = m_notesHz[idx];
                ch.vibratoIndex = (ch.vibratoIndex + ch.vibratoSpeed) % 32;
            }

            if (ch.portamento && m_state.rowTick != 0) {
                ch.freq[0] += ch.portamento;
                if (ch.portamentoToNote != 0) {
                    const bool reachedUp = ch.portamento > 0 && ch.freq[0] >= ch.portamentoToNote;
                    const bool reachedDown = ch.portamento < 0 && ch.freq[0] <= ch.portamentoToNote;
                    if (reachedUp || reachedDown) {
                        ch.freq[0] = ch.portamentoToNote;
                        ch.portamentoToNote = 0;
                    }
                }
            }
        }
    }

    void process_row() {
        int patternJump = m_state.order + 1;
        int rowJump = 0;
        bool doJump = false;
        const uint8_t pattern = m_song.order[static_cast<size_t>(m_state.order) & 255];

        for (uint8_t j = 0; j < m_song.totalTracks; ++j) {
            const size_t idx = (static_cast<size_t>(pattern) * 64 + static_cast<size_t>(m_state.row)) * m_song.totalTracks + j;
            if (idx >= m_song.patternData.size()) continue;

            const uint16_t d = m_song.patternData[idx];
            uint32_t note = (d >> 9) & 127;
            const uint32_t effect = (d >> 6) & 7;
            const uint32_t effectdata = d & 63;
            const uint32_t effectdata1 = (d >> 3) & 7;
            const uint32_t effectdata2 = d & 7;

            auto& ch = m_state.channel[j];
            ch.portamento = 0;
            ch.arp = 0;
            ch.vibrato = 0;

            const int oldhz = ch.freq[0];

            if (note == 127) {
                ch.active = false;
                ch.freq[0] = ch.freq[1] = ch.freq[2] = 0;
                ch.portamento = 0;
                ch.lastNote = 0;
            } else if (note != 0) {
                ch.active = true;
                const int hz = m_notesHz[g_note_index_clamped(static_cast<int>(note))];
                ch.freq[0] = hz;
                ch.freq[1] = hz;
                ch.freq[2] = hz;
                ch.portamento = 0;
                ch.lastNote = static_cast<int>(note);
                ch.vibratoIndex = 0;
            } else {
                note = static_cast<uint32_t>(ch.lastNote);
            }

            switch (effect) {
            case 0x0:
                ch.freq[1] = m_notesHz[g_note_index_clamped(static_cast<int>(note + effectdata1))];
                ch.freq[2] = m_notesHz[g_note_index_clamped(static_cast<int>(note + effectdata2))];
                if (effectdata1 || effectdata2) ch.arp = 1;
                break;
            case 0x1:
                ch.portamento = static_cast<int>(effectdata);
                break;
            case 0x2:
                ch.portamento = -static_cast<int>(effectdata);
                break;
            case 0x3:
                ch.portamentoToNote = m_notesHz[g_note_index_clamped(static_cast<int>(note))];
                if (oldhz != ch.portamentoToNote) {
                    ch.freq[0] = oldhz;
                    ch.portamento = static_cast<int>(effectdata);
                    if (oldhz > ch.portamentoToNote) ch.portamento = -ch.portamento;
                } else {
                    ch.portamentoToNote = 0;
                }
                break;
            case 0x4:
                ch.vibrato = 1;
                if (effectdata2 != 0) ch.vibratoDepth = static_cast<int>(effectdata2);
                if (effectdata1 != 0) ch.vibratoSpeed = static_cast<int>(effectdata1);
                break;
            case 0x5:
                patternJump = static_cast<int>(effectdata);
                doJump = true;
                break;
            case 0x6:
                rowJump = static_cast<int>(effectdata);
                doJump = true;
                break;
            case 0x7:
                m_state.tempo = static_cast<int>(effectdata);
                if (m_state.tempo <= 0) m_state.tempo = 1;
                break;
            default:
                break;
            }
        }

        m_state.row++;
        if (doJump) {
            if (rowJump < 0) rowJump = 0;
            if (rowJump > 63) rowJump = 63;
            m_state.row = rowJump;
            m_state.order = patternJump;
        }

        if (m_state.row >= 64) {
            m_state.row = 0;
            m_state.order++;
            if (m_song.order[static_cast<size_t>(m_state.order) & 255] == 0xff) {
                m_state.order = 0;
            }
        }
    }

    void assign_hardware_channels() {
        for (uint32_t j = 0; j < kMaxTracks; ++j) {
            m_state.output[j].samplePosInc = 0.0f;
        }

        uint32_t gotit = 0;
        uint32_t tries = 0;
        while (gotit < m_song.totalTracks && tries < m_song.totalTracks) {
            auto& ch = m_state.channel[m_state.nextChannel];
            if (ch.active) {
                int hz = ch.freq[0];
                if (ch.arp) {
                    hz = ch.freq[ch.arpCounter % 3];
                    ch.arpCounter = (ch.arpCounter + 1) % 3;
                }
                if (hz > 0) {
                    m_state.output[gotit].samplePosInc = static_cast<float>(hz) / static_cast<float>(kMonotoneSampleRate);
                }
                gotit++;
            }

            m_state.nextChannel++;
            if (m_state.nextChannel >= static_cast<int>(m_song.totalTracks)) m_state.nextChannel = 0;
            tries++;
        }
    }

    service_ptr_t<file> m_file;
    std::vector<uint8_t> m_fileData;
    MonotoneSong m_song;
    MonotoneState m_state;
    std::array<int, 800> m_notesHz{};
    std::array<int, 32> m_vibTable{};
    std::vector<float> m_renderBuffer;
    uint64_t m_renderedFrames = 0;
    bool m_decoding = false;
};

} // anonymous namespace

static input_factory_t<input_monotone> g_input_monotone_factory;