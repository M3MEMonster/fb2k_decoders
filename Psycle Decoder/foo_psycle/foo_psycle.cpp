#include <foobar2000.h>

#include <psycle/host/AudioDriver.hpp>
#include <psycle/host/Configuration.hpp>
#include <psycle/host/machineloader.hpp>
#include <psycle/host/Player.hpp>
#include <psycle/host/ProgressDialog.hpp>
#include <psycle/host/Song.hpp>
#include <psycle/host/Version.hpp>
#include <psycle/plugins/druttis/blwtbl/blwtbl.h>

#include <algorithm>
#include <thread>
#include <mutex>

#pragma comment(lib, "winmm.lib")

namespace {

static constexpr uint32_t kSampleRate = 44100;
static constexpr int kNumChannels = 2;

static std::mutex g_psycleMutex;
static class input_psycle* g_activeOwner = nullptr;
static bool g_engineInitialized = false;

static void evict_active_owner_locked();

struct FoobarRiffFile : public psycle::host::RiffFile
{
    bool Open(std::string const&) override { return true; }
    bool Create(std::string const&, bool) override { return false; }
    bool Close() override { return true; }

    bool Expect(const void* pData, std::size_t numBytes) override {
        try { return m_file->read(const_cast<void*>(pData), numBytes, *m_abort) == numBytes; }
        catch (...) { return false; }
    }

    int Seek(std::size_t offset) override {
        try { m_file->seek(offset, *m_abort); return 1; }
        catch (...) { return 0; }
    }

    int Skip(std::size_t numBytes) override {
        try {
            auto pos = m_file->get_position(*m_abort);
            m_file->seek(pos + numBytes, *m_abort);
            return 1;
        } catch (...) { return 0; }
    }

    bool Eof() override {
        try { return m_file->get_position(*m_abort) >= m_file->get_size(*m_abort); }
        catch (...) { return true; }
    }

    std::size_t FileSize() override {
        try { return static_cast<std::size_t>(m_file->get_size(*m_abort)); }
        catch (...) { return 0; }
    }

    std::size_t GetPos() override {
        try { return static_cast<std::size_t>(m_file->get_position(*m_abort)); }
        catch (...) { return 0; }
    }

    FILE* GetFile() override { return nullptr; }

    bool ReadInternal(void* pData, std::size_t numBytes) override {
        try { return m_file->read(pData, numBytes, *m_abort) == numBytes; }
        catch (...) { return false; }
    }

    bool WriteInternal(void const*, std::size_t) override { return false; }

    service_ptr_t<file> m_file;
    abort_callback* m_abort;
};

class input_psycle : public input_stubs {
public:
    input_psycle() = default;

    ~input_psycle() {
        std::lock_guard<std::mutex> lock(g_psycleMutex);
        if (g_activeOwner == this) {
            shutdown_engine_locked();
            g_activeOwner = nullptr;
        }
    }

    void open(service_ptr_t<file> p_filehint, const char* p_path, t_input_open_reason p_reason, abort_callback& p_abort) {
        if (p_reason == input_open_info_write)
            throw exception_tagging_unsupported();

        m_file = p_filehint;
        input_open_file_helper(m_file, p_path, p_reason, p_abort);

        char header[8] = {};
        m_file->read_object(header, 8, p_abort);
        m_isPsycle3 = (memcmp(header, "PSY3SONG", 8) == 0);
        bool isPsycle2 = !m_isPsycle3 && (memcmp(header, "PSY2SONG", 8) == 0);
        if (!m_isPsycle3 && !isPsycle2)
            throw exception_io_unsupported_format();

        std::lock_guard<std::mutex> lock(g_psycleMutex);

        evict_active_owner_locked();

        if (!g_engineInitialized) {
            psycle::plugins::druttis::InitLibrary();
            psycle::host::Global::pLogCallback = [](const char*, const char*) {};
            psycle::host::Global::configuration().LoadNewBlitz(false);
            psycle::host::Global::configuration().SetNumThreads(
                (std::max)(2u, std::thread::hardware_concurrency() - 2));
            psycle::host::Global::configuration().RefreshSettings();
            psycle::host::Global::configuration().SetPluginDir("");
            psycle::host::Global::configuration().SetVst64Dir("");
            psycle::host::Global::configuration().SetVst32Dir("");
            psycle::host::Global::configuration().SetLadspaDir("");
            psycle::host::Global::configuration().SetLuaDir("");
            psycle::host::Global::machineload().ReScan();
            g_engineInitialized = true;
        }

        psycle::plugins::druttis::UpdateWaveforms(kSampleRate);

        psycle::host::CProgressDialog dlg(nullptr, false);
        FoobarRiffFile riffFile;
        riffFile.m_file = m_file;
        riffFile.m_abort = &p_abort;
        m_file->seek(0, p_abort);

        if (!psycle::host::Global::song().Load(&riffFile, dlg))
            throw exception_io_data("Failed to load PSY file");

        g_activeOwner = this;
        m_loaded = true;

        psycle::host::Global::player()._recording = true;
        m_clipboardMem.clear();
        m_clipboardMem.push_back(reinterpret_cast<char*>(&m_clipboardMemSize));
        m_clipboardMem.push_back(nullptr);
        psycle::host::Global::player().StartRecording(
            "", 32, kSampleRate, psycle::host::no_mode, true, false, 0, 0, &m_clipboardMem);

        m_numSubsongs = psycle::host::Global::player().NumSubsongs(
            psycle::host::Global::song());

        auto& song = psycle::host::Global::song();
        m_songName = song.name;
        m_songAuthor = song.author;
        m_songComments = song.comments;
        m_songTracks = song.SONGTRACKS;

        m_durations.resize(m_numSubsongs);
        for (int i = 0; i < m_numSubsongs; i++) {
            int seq = -1, pos = -1, time = -1, linecount = -1;
            int startPos = psycle::host::Global::player().NumSubsongs(
                psycle::host::Global::song(), i);
            psycle::host::Global::player().CalcPosition(
                psycle::host::Global::song(), seq, pos, time, linecount, false, startPos);
            m_durations[i] = (time > 0) ? time / 1000.0 : 0.0;
        }
    }

    unsigned get_subsong_count() {
        return static_cast<unsigned>(m_numSubsongs);
    }

    t_uint32 get_subsong(unsigned p_index) {
        return static_cast<t_uint32>(p_index);
    }

    void get_info(t_uint32 p_subsong, file_info& p_info, abort_callback& p_abort) {
        if (p_subsong < m_durations.size() && m_durations[p_subsong] > 0)
            p_info.set_length(m_durations[p_subsong]);

        p_info.info_set_int("samplerate", kSampleRate);
        p_info.info_set_int("channels", kNumChannels);
        p_info.info_set_int("bitspersample", 32);
        p_info.info_set("encoding", "synthesized");
        p_info.info_set("codec", m_isPsycle3 ? "Psycle v3" : "Psycle v2");

        char buf[16];
        sprintf(buf, "%d", m_songTracks);
        p_info.info_set("psycle_tracks", buf);

        if (!m_songName.empty())
            p_info.meta_set("title", m_songName.c_str());
        if (!m_songAuthor.empty())
            p_info.meta_set("artist", m_songAuthor.c_str());
        if (!m_songComments.empty())
            p_info.meta_set("comment", m_songComments.c_str());
    }

    t_filestats2 get_stats2(unsigned f, abort_callback& a) {
        return m_file->get_stats2_(f, a);
    }

    t_filestats get_file_stats(abort_callback& p_abort) {
        return m_file->get_stats(p_abort);
    }

    void decode_initialize(t_uint32 p_subsong, unsigned p_flags, abort_callback& p_abort) {
        std::lock_guard<std::mutex> lock(g_psycleMutex);
        if (g_activeOwner != this) {
            m_decoding = false;
            return;
        }
        m_subsongIndex = p_subsong;
        int startPos = psycle::host::Global::player().NumSubsongs(
            psycle::host::Global::song(), m_subsongIndex);
        psycle::host::Global::player().Start(startPos, 0);
        m_decoding = true;
    }

    bool decode_run(audio_chunk& p_chunk, abort_callback& p_abort) {
        if (!m_decoding) return false;

        std::lock_guard<std::mutex> lock(g_psycleMutex);
        if (g_activeOwner != this) {
            m_decoding = false;
            return false;
        }

        enum { kBlockSize = 1024 };
        float outputBuffer[kBlockSize * kNumChannels];

        m_clipboardMemSize = 0;
        m_clipboardMem[1] = reinterpret_cast<char*>(outputBuffer);
        psycle::host::Global::player().Work(kBlockSize);

        auto samplesProduced = static_cast<unsigned>(
            psycle::host::Global::player().sampleOffset);
        if (samplesProduced == 0) {
            m_decoding = false;
            return false;
        }

        p_chunk.set_data_32(outputBuffer, samplesProduced, kNumChannels, kSampleRate);
        return true;
    }

    void decode_seek(double p_seconds, abort_callback& p_abort) {
        std::lock_guard<std::mutex> lock(g_psycleMutex);
        if (g_activeOwner != this) return;
        int seekMs = static_cast<int>(p_seconds * 1000.0);
        if (seekMs <= 0) {
            int startPos = psycle::host::Global::player().NumSubsongs(
                psycle::host::Global::song(), m_subsongIndex);
            psycle::host::Global::player().Start(startPos, 0);
        } else {
            psycle::host::Global::player().SeekToPosition(
                psycle::host::Global::song(), seekMs, false);
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
    static bool g_is_our_path(const char*, const char* p_extension) {
        return stricmp_utf8(p_extension, "psy") == 0;
    }
    static const char* g_get_name() { return "Psycle decoder"; }
    static const GUID g_get_guid() {
        static const GUID guid = {
            0x7a3b5c1d, 0xe2f4, 0x4a6b,
            { 0x8c, 0x9d, 0x0e, 0x1f, 0x2a, 0x3b, 0x4c, 0x5d }
        };
        return guid;
    }

    void shutdown_engine_locked() {
        psycle::host::Global::player().Stop();
        psycle::host::Global::song().Reset();
        psycle::host::Global::player().~Player();
        new (&psycle::host::Global::player()) psycle::host::Player;
        m_loaded = false;
        m_decoding = false;
    }

private:
    service_ptr_t<file> m_file;
    bool m_loaded = false;
    bool m_decoding = false;
    bool m_isPsycle3 = false;
    uint32_t m_subsongIndex = 0;
    int m_numSubsongs = 1;
    int m_songTracks = 0;
    std::string m_songName;
    std::string m_songAuthor;
    std::string m_songComments;
    std::vector<double> m_durations;
    std::vector<char*> m_clipboardMem;
    uint32_t m_clipboardMemSize = 0;
};

static void evict_active_owner_locked() {
    if (g_activeOwner) {
        g_activeOwner->shutdown_engine_locked();
        g_activeOwner = nullptr;
    }
}

} // anonymous namespace

static input_factory_t<input_psycle> g_input_psycle_factory;
