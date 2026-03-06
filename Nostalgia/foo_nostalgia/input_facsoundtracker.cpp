#include <foobar2000.h>
#include "cfg_vars.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include <windows.h>

extern "C" {
#include "3rdParty/fstr/adlibemu.h"
}

namespace {

static constexpr uint32_t kSampleRate = 44100;
static constexpr uint32_t kChannels = 1;
static constexpr uint32_t kFramesPerInterrupt = 630; // 44.1kHz / 70Hz
static constexpr size_t kHeaderBytes = 7;
static constexpr size_t kPayloadBytes = 0x4000;
static constexpr size_t kRowBytes = 12;
static constexpr size_t kPatternBytes = 16128;
static constexpr size_t kMaxRows = kPatternBytes / kRowBytes;

static constexpr uint8_t kMagic[kHeaderBytes] = { 0xFE, 0x00, 0x80, 0xFF, 0xBF, 0x00, 0x80 };
static constexpr std::array<uint16_t, 12> kNotes = {
    0x0159, 0x016D, 0x0183, 0x019A, 0x01B2, 0x01CC, 0x01E8, 0x0205, 0x0223, 0x0244, 0x0267, 0x028B
};
static constexpr std::array<uint8_t, 99> kRegisters = {
    0x20,0x23,0x40,0x43,0x60,0x63,0x80,0x83,0xA0,0xB0,0xC0,
    0x21,0x24,0x41,0x44,0x61,0x64,0x81,0x84,0xA1,0xB1,0xC1,
    0x22,0x25,0x42,0x45,0x62,0x65,0x82,0x85,0xA2,0xB2,0xC2,
    0x28,0x2B,0x48,0x4B,0x68,0x6B,0x88,0x8B,0xA3,0xB3,0xC3,
    0x29,0x2C,0x49,0x4C,0x69,0x6C,0x89,0x8C,0xA4,0xB4,0xC4,
    0x2A,0x2D,0x4A,0x4D,0x6A,0x6D,0x8A,0x8D,0xA5,0xB5,0xC5,
    0x30,0x33,0x50,0x53,0x70,0x73,0x90,0x93,0xA6,0xB6,0xC6,
    0x31,0x34,0x51,0x54,0x71,0x74,0x91,0x94,0xA7,0xB7,0xC7,
    0x32,0x35,0x52,0x55,0x72,0x75,0x92,0x95,0xA8,0xB8,0xC8
};

static constexpr std::array<uint8_t, 11> kSnareDef = { 0x04,0x0F,0x3F,0x02,0xF1,0xF7,0xFF,0xF7,0x59,0x01,0x92 };
static constexpr std::array<uint8_t, 11> kSnareDef2 = { 0x04,0x0F,0x3F,0x02,0xF1,0xF7,0xFF,0xF7,0x20,0x02,0x92 };
static constexpr std::array<uint8_t, 11> kBassDef = { 0x01,0x01,0x00,0x00,0xCF,0xCF,0x68,0x07,0xAB,0x09,0x92 };
static constexpr std::array<uint8_t, 11> kHihatDef = { 0x01,0x09,0x30,0x0A,0xF7,0xF6,0x77,0x77,0xFF,0x11,0x92 };
static constexpr std::array<uint8_t, 11> kHiopDef = { 0x00,0x0F,0x12,0x30,0xF1,0xF6,0x24,0xF8,0x58,0x18,0x92 };
static constexpr std::array<uint8_t, 11> kCrashDef = { 0x00,0x0F,0x00,0x30,0xF6,0xF6,0x24,0xF8,0x58,0x13,0x92 };
static constexpr std::array<uint8_t, 11> kTom1Def = { 0x01,0x03,0x00,0x00,0xF6,0xF2,0x27,0x28,0x58,0x14,0x92 };
static constexpr std::array<uint8_t, 11> kTom2Def = { 0x01,0x03,0x02,0x00,0xF6,0xF2,0x25,0x28,0x58,0x11,0x92 };

static std::wstring g_utf8_to_wide(const char* str) {
    if (!str || !str[0]) return L"";
    int wlen = MultiByteToWideChar(CP_UTF8, 0, str, -1, nullptr, 0);
    if (wlen <= 0) return L"";
    std::wstring ws(static_cast<size_t>(wlen) - 1, L'\0');
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

static std::string g_parse_text(const uint8_t* p, size_t bytes, bool trimLeft) {
    size_t begin = 0;
    size_t end = bytes;
    while (end > 0 && (p[end - 1] == 0 || p[end - 1] == ' ')) --end;
    if (trimLeft) {
        while (begin < end && (p[begin] == 0 || p[begin] == ' ')) ++begin;
    }
    return std::string(reinterpret_cast<const char*>(p + begin), reinterpret_cast<const char*>(p + end));
}

class input_facsoundtracker : public input_stubs {
public:
    void open(service_ptr_t<file> p_filehint, const char* p_path, t_input_open_reason p_reason, abort_callback& p_abort) {
        if (p_reason == input_open_info_write) throw exception_tagging_unsupported();

        m_file = p_filehint;
        input_open_file_helper(m_file, p_path, p_reason, p_abort);

        t_filesize fileSize = m_file->get_size(p_abort);
        if (fileSize == filesize_invalid || fileSize < (kHeaderBytes + kPayloadBytes)) {
            throw exception_io_unsupported_format();
        }

        m_fileData.resize(static_cast<size_t>(fileSize));
        m_file->read_object(m_fileData.data(), m_fileData.size(), p_abort);
        parse_song_or_throw();
    }

    unsigned get_subsong_count() { return 1; }
    t_uint32 get_subsong(unsigned) { return 0; }

    void get_info(t_uint32, file_info& p_info, abort_callback&) {
        if (m_lengthSeconds > 0.0) p_info.set_length(m_lengthSeconds);
        p_info.info_set_int("samplerate", kSampleRate);
        p_info.info_set_int("channels", kChannels);
        p_info.info_set_int("bitspersample", 16);
        p_info.info_set("encoding", "synthesized (OPL2)");
        p_info.info_set("codec", "FAC SoundTracker");
        if (!m_title.empty()) p_info.meta_set("title", m_title.c_str());
        if (!m_composer.empty()) p_info.meta_set("artist", m_composer.c_str());
    }

    t_filestats2 get_stats2(unsigned p_flags, abort_callback& p_abort) { return m_file->get_stats2_(p_flags, p_abort); }
    t_filestats get_file_stats(abort_callback& p_abort) { return m_file->get_stats(p_abort); }

    void decode_initialize(t_uint32, unsigned, abort_callback&) {
        initialize_playback_state();
    }

    bool decode_run(audio_chunk& p_chunk, abort_callback& p_abort) {
        if (!m_decoding) return false;

        static constexpr uint32_t kBlockFrames = 1024;
        m_renderBuffer.resize(kBlockFrames);
        uint32_t gotFrames = render_frames(m_renderBuffer.data(), kBlockFrames, p_abort);
        if (gotFrames == 0) return false;

        p_chunk.set_data_32(m_renderBuffer.data(), gotFrames, kChannels, kSampleRate);
        return true;
    }

    void decode_seek(double p_seconds, abort_callback& p_abort) {
        initialize_playback_state();

        if (p_seconds <= 0.0) return;
        uint64_t targetFrames = static_cast<uint64_t>(p_seconds * static_cast<double>(kSampleRate));
        std::array<float, 1024> scratch = {};
        while (m_decoding && m_renderedFrames < targetFrames) {
            p_abort.check();
            uint64_t remain = targetFrames - m_renderedFrames;
            uint32_t now = static_cast<uint32_t>(std::min<uint64_t>(scratch.size(), remain));
            if (render_frames(scratch.data(), now, p_abort) == 0) break;
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
        if (!nostalgia_cfg::cfg_facsoundtracker_enabled.get()) return false;
        if (p_extension == nullptr || stricmp_utf8(p_extension, "mus") != 0) return false;
        uint8_t hdr[kHeaderBytes] = {};
        if (!g_read_header(p_path, hdr, sizeof(hdr))) return true;
        return memcmp(hdr, kMagic, sizeof(kMagic)) == 0;
    }
    static const char* g_get_name() { return "FAC SoundTracker decoder"; }
    static const GUID g_get_guid() {
        static const GUID guid = { 0x786c60f1, 0x89ee, 0x44b1, { 0x9b, 0xaf, 0x6f, 0x55, 0x7b, 0xef, 0x2c, 0x2f } };
        return guid;
    }

private:
    void parse_song_or_throw() {
        if (m_fileData.size() < (kHeaderBytes + kPayloadBytes)) throw exception_io_unsupported_format();
        if (memcmp(m_fileData.data(), kMagic, kHeaderBytes) != 0) throw exception_io_unsupported_format();

        memcpy(m_payload.data(), m_fileData.data() + kHeaderBytes, kPayloadBytes);

        m_speedTicks = static_cast<uint16_t>(m_payload[16258]) + 1;
        int rows = (static_cast<int>(m_payload[16376]) + 1) << 4;
        rows = std::clamp(rows, 1, static_cast<int>(kMaxRows));
        m_lastLine = static_cast<uint16_t>(rows);
        m_lengthSeconds = (static_cast<double>(m_lastLine) * static_cast<double>(m_speedTicks)) / 70.0;

        m_title = g_parse_text(&m_payload[16294], 40, true);
        m_composer = g_parse_text(&m_payload[16335], 40, true);
    }

    void initialize_playback_state() {
        memset(&m_opl, 0, sizeof(m_opl));
        adlibinit(&m_opl, kSampleRate, kChannels, 2);

        m_decoding = true;
        m_endMusic = false;
        m_xx = 0;
        m_lastInc = 0;
        m_lastDrum = 0;
        m_canales = 6;
        m_drumsOff = false;
        m_samplesToInterrupt = kFramesPerInterrupt;
        m_renderedFrames = 0;
        m_chanOff.fill(0);
        m_drumOff.fill(0);

        init_sound();
        init_instruments();
        select_drum_system();
        set_drums_on();
    }

    void write_opl(uint8_t reg, uint8_t value) {
        adlib0(&m_opl, reg, value);
    }

    void init_sound() {
        write_opl(0x20, 0x01);
        write_opl(0x00, 0x08);
        for (uint8_t reg = 0x20; reg < 0x40; ++reg) write_opl(reg, 0x00);
        for (uint8_t reg = 0x40; reg < 0x56; ++reg) write_opl(reg, 0x3F);
        for (uint8_t reg = 0x56; reg < 0x60; ++reg) write_opl(reg, 0x00);
        for (uint8_t reg = 0x60; reg < 0x76; ++reg) write_opl(reg, 0xFF);
        for (uint8_t reg = 0x76; reg < 0x80; ++reg) write_opl(reg, 0x00);
        for (uint8_t reg = 0x80; reg < 0x96; ++reg) write_opl(reg, 0xFF);
        for (uint16_t reg = 0x96; reg < 0xF6; ++reg) write_opl(static_cast<uint8_t>(reg), 0x00);
    }

    void init_instruments() {
        for (size_t i = 0; i < kRegisters.size(); ++i) {
            write_opl(kRegisters[i], m_payload[16128 + i]);
        }
    }

    void set_drums_on() {
        m_canales = 6;
        m_drumsOff = false;
        write_opl(0xB6, 0x00);
        write_opl(0xB7, 0x00);
        write_opl(0xB8, 0x00);
    }

    void set_drums_off() {
        m_drumsOff = true;
        write_opl(0xBD, 0x00);
        for (size_t i = 66; i < 99; ++i) {
            write_opl(kRegisters[i], m_payload[16128 + i]);
        }
        m_canales = 9;
    }

    void select_drum_system() {
        uint8_t lastDrCode = 0;
        for (int row = static_cast<int>(m_lastLine) - 1; row >= 0; --row) {
            uint8_t d = m_payload[static_cast<size_t>(row) * kRowBytes + 11];
            if (d != 0) {
                lastDrCode = d;
                break;
            }
        }
        m_drumSystem2 = (lastDrCode > 0x0F);
    }

    void send_patch(size_t regOffset, const std::array<uint8_t, 11>& data) {
        for (size_t i = 0; i < data.size(); ++i) {
            const uint8_t value = data[i];
            if (value == 0x92) continue;
            write_opl(kRegisters[regOffset + i], value);
        }
    }

    void snare() { send_patch(77, kSnareDef); }
    void snare2() { send_patch(77, kSnareDef2); }
    void bassdrum() { send_patch(66, kBassDef); }
    void hihat() { send_patch(88, kHihatDef); }
    void hihatop() { send_patch(77, kHiopDef); }
    void crash() {
        write_opl(0x32, 0x3F);
        write_opl(0x35, 0x3F);
        send_patch(77, kCrashDef);
    }
    void tom1() { send_patch(88, kTom1Def); }
    void tom2() { send_patch(88, kTom2Def); }

    uint8_t run_drum_case(uint8_t code) {
        if (m_drumSystem2) {
            switch (code & 0x0F) {
            case 1: bassdrum(); crash(); return 0x11;
            case 7: hihat(); return 0x02;
            case 11:
            case 12: snare(); return 0x08;
            default: return 0;
            }
        }

        switch (code & 0x0F) {
        case 1: bassdrum(); return 0x10;
        case 2: snare(); return 0x08;
        case 3: bassdrum(); hihatop(); return 0x11;
        case 4: tom2(); return 0x04;
        case 5: bassdrum(); crash(); return 0x11;
        case 7: snare2(); return 0x08;
        case 9: tom1(); return 0x04;
        case 11:
        case 12: snare(); return 0x08;
        default: return 0;
        }
    }

    void drums(uint8_t drumData) {
        if (m_drumsOff) return;

        uint8_t code = drumData;
        if (m_drumSystem2) code >>= 4;
        uint8_t bl = run_drum_case(code);

        if ((bl & 0x10) && m_drumOff[0]) bl = static_cast<uint8_t>(bl & ~0x10);
        if ((bl & 0x0A) && m_drumOff[1]) bl = static_cast<uint8_t>(bl & ~0x0A);
        if ((bl & 0x05) && m_drumOff[2]) bl = static_cast<uint8_t>(bl & ~0x05);

        uint8_t ah = m_lastDrum;
        uint8_t inv = static_cast<uint8_t>(bl ^ 31u);
        ah = static_cast<uint8_t>(ah & inv);
        ah = static_cast<uint8_t>(ah | 0x20);
        write_opl(0xBD, ah);

        inv = static_cast<uint8_t>(inv ^ 31u);
        m_lastDrum = inv;
        ah = static_cast<uint8_t>(ah | inv | 0x20);
        write_opl(0xBD, ah);
    }

    void note_off(uint8_t channel) {
        if (channel >= 9 || m_chanOff[channel]) return;
        write_opl(static_cast<uint8_t>(0xB0 + channel), 0x00);
    }

    void note_on(uint8_t note, uint8_t channel) {
        if (channel >= 9 || m_chanOff[channel] || note == 0) return;
        uint8_t n = static_cast<uint8_t>(note - 1);
        uint8_t octave = static_cast<uint8_t>(n / 12);
        uint8_t index = static_cast<uint8_t>(n % 12);
        uint16_t fnum = kNotes[index];

        write_opl(static_cast<uint8_t>(0xA0 + channel), static_cast<uint8_t>(fnum & 0xFF));
        uint8_t bval = static_cast<uint8_t>(((octave + 2) << 2) | ((fnum >> 8) & 0xFF));
        bval = static_cast<uint8_t>(bval | 0x20);
        write_opl(static_cast<uint8_t>(0xB0 + channel), bval);
    }

    void play_row() {
        if (m_endMusic) return;
        if (m_xx >= m_lastLine) {
            m_endMusic = true;
            return;
        }

        const size_t rowOffset = static_cast<size_t>(m_xx) * kRowBytes;
        if ((rowOffset + 11) >= kPatternBytes) {
            m_endMusic = true;
            return;
        }

        for (uint8_t ch = 0; ch < m_canales; ++ch) {
            const uint8_t cmd = m_payload[rowOffset + ch];
            if (cmd == 0) continue;
            if (cmd == 64) {
                note_off(ch);
            } else if (cmd < 64) {
                note_off(ch);
                note_on(cmd, ch);
            }
        }

        drums(m_payload[rowOffset + 11]);

        ++m_xx;
        if (m_xx == m_lastLine) {
            m_endMusic = true;
        }
    }

    void on_interrupt() {
        ++m_lastInc;
        if (m_lastInc != m_speedTicks) return;
        m_lastInc = 0;
        play_row();
    }

    uint32_t render_frames(float* out, uint32_t frames, abort_callback& p_abort) {
        uint32_t produced = 0;
        while (produced < frames && m_decoding) {
            p_abort.check();

            const uint32_t now = std::min<uint32_t>(frames - produced, m_samplesToInterrupt);
            m_pcm16.resize(now);
            adlibgetsample(&m_opl, reinterpret_cast<unsigned char*>(m_pcm16.data()), static_cast<long>(now * sizeof(int16_t)));
            for (uint32_t i = 0; i < now; ++i) {
                out[produced + i] = static_cast<float>(m_pcm16[i]) / 32768.0f;
            }

            produced += now;
            m_renderedFrames += now;
            m_samplesToInterrupt -= now;

            if (m_samplesToInterrupt == 0) {
                on_interrupt();
                m_samplesToInterrupt = kFramesPerInterrupt;
                if (m_endMusic) {
                    m_decoding = false;
                    set_drums_off();
                    init_sound();
                }
            }
        }
        return produced;
    }

    service_ptr_t<file> m_file;
    std::vector<uint8_t> m_fileData;
    std::array<uint8_t, kPayloadBytes> m_payload = {};

    adlibemu_context m_opl = {};
    std::array<uint8_t, 9> m_chanOff = {};
    std::array<uint8_t, 3> m_drumOff = {};
    std::vector<int16_t> m_pcm16;
    std::vector<float> m_renderBuffer;

    std::string m_title;
    std::string m_composer;
    double m_lengthSeconds = 0.0;

    bool m_decoding = false;
    bool m_endMusic = false;
    bool m_drumsOff = false;
    bool m_drumSystem2 = false;
    uint8_t m_canales = 6;
    uint8_t m_lastDrum = 0;
    uint16_t m_speedTicks = 1;
    uint16_t m_lastLine = 1;
    uint16_t m_xx = 0;
    uint16_t m_lastInc = 0;
    uint32_t m_samplesToInterrupt = kFramesPerInterrupt;
    uint64_t m_renderedFrames = 0;
};

} // namespace

static input_factory_t<input_facsoundtracker> g_input_facsoundtracker_factory;
