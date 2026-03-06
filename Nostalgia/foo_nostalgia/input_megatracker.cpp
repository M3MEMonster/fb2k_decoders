#include <foobar2000.h>
#include "cfg_vars.h"

#include <vector>
#include <string>
#include <cstring>
#include <cstdio>
#include <mutex>
#include <windows.h>

extern "C" {
#include "mtpng/mtptypes.h"
#include "mtpng/ntgs.h"
#include "mtpng/oss.h"
#include "mtpng/smus.h"
#include "mtpng/ssmt.h"

static MTPInputT* input_plugins[] = {
    &ssmt_plugin,
    &ntgs_plugin,
    &smus_plugin,
    (MTPInputT*)NULL
};

int frequency = 48000;
extern unsigned long totalsamples;
extern uint32 SPT;

} // extern "C"

namespace {

static std::wstring utf8ToWide(const char* str) {
    if (!str || !str[0]) return L"";
    int wlen = MultiByteToWideChar(CP_UTF8, 0, str, -1, nullptr, 0);
    if (wlen <= 0) return L"";
    std::wstring ws(wlen - 1, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, str, -1, &ws[0], wlen);
    return ws;
}

static std::string toNativePath(const char* fbPath) {
    if (pfc::strcmp_partial(fbPath, "file://") == 0) return std::string(fbPath + 7);
    return std::string(fbPath);
}

static size_t readHeaderFromPath(const char* fbPath, uint8_t* out, size_t bytes) {
    if (!fbPath || !out || bytes == 0) return 0;
    std::string native = toNativePath(fbPath);
    FILE* fp = nullptr;
    std::wstring wpath = utf8ToWide(native.c_str());
    if (!wpath.empty()) fp = _wfopen(wpath.c_str(), L"rb");
    if (!fp) fp = fopen(native.c_str(), "rb");
    if (!fp) return 0;
    size_t got = fread(out, 1, bytes, fp);
    fclose(fp);
    return got;
}

static std::mutex g_mt_mutex;

static const uint8_t* g_mt_memData = nullptr;
static size_t g_mt_memSize = 0;
static size_t g_mt_memPos = 0;
static std::string g_mt_songPath;

static FILE* g_mt_diskFile = nullptr;
static bool g_mt_usingMem = true;

class input_megatracker;
static input_megatracker* g_mt_activeInstance = nullptr;

static MTPInputT* detectFormatFromBytes(const uint8_t* data, size_t size)
{
    if (size < 32) return nullptr;

    if (data[4] == 'O' && data[5] == 'K')
        return &ssmt_plugin;
    if (data[0] == 'I' && data[1] == 'A' && data[2] == 'N')
        return &ssmt_plugin;
    if (size >= 16 && data[2] == 'F' && data[3] == 'T' && data[4] == 'A' &&
        data[6] == 'M' && data[7] == 'O' && data[8] == 'D' && data[15] == 'E')
        return &ntgs_plugin;
    if (data[0] == 'F' && data[1] == 'O' && data[2] == 'R' && data[3] == 'M' &&
        data[8] == 'S' && data[9] == 'M' && data[10] == 'U' && data[11] == 'S')
        return &smus_plugin;

    return nullptr;
}

static bool safe_gus_update() {
    __try {
        GUS_Update();
        return true;
    } __except(GetExceptionCode() == EXCEPTION_ACCESS_VIOLATION ?
               EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH) {
        return false;
    }
}

extern "C" {

int8 FILE_Open(char* fileName)
{
    if (g_mt_diskFile) { fclose(g_mt_diskFile); g_mt_diskFile = nullptr; }

    if (_stricmp(g_mt_songPath.c_str(), fileName) == 0)
    {
        g_mt_memPos = 0;
        g_mt_usingMem = true;
        return 1;
    }

    std::wstring wpath = utf8ToWide(fileName);
    if (!wpath.empty())
        g_mt_diskFile = _wfopen(wpath.c_str(), L"rb");
    if (!g_mt_diskFile)
        g_mt_diskFile = fopen(fileName, "rb");

    if (g_mt_diskFile) {
        g_mt_usingMem = false;
        return 1;
    }

    return 0;
}

uint32 FILE_GetLength(void)
{
    if (g_mt_usingMem) return (uint32)g_mt_memSize;
    if (g_mt_diskFile) {
        long cur = ftell(g_mt_diskFile);
        fseek(g_mt_diskFile, 0, SEEK_END);
        long len = ftell(g_mt_diskFile);
        fseek(g_mt_diskFile, cur, SEEK_SET);
        return (uint32)len;
    }
    return 0;
}

void FILE_Seek(uint32 position)
{
    if (g_mt_usingMem) {
        g_mt_memPos = position;
        if (g_mt_memPos > g_mt_memSize) g_mt_memPos = g_mt_memSize;
    } else if (g_mt_diskFile) {
        fseek(g_mt_diskFile, (long)position, SEEK_SET);
    }
}

void FILE_Seekrel(uint32 position)
{
    if (g_mt_usingMem) {
        g_mt_memPos += position;
        if (g_mt_memPos > g_mt_memSize) g_mt_memPos = g_mt_memSize;
    } else if (g_mt_diskFile) {
        fseek(g_mt_diskFile, (long)position, SEEK_CUR);
    }
}

uint32 FILE_GetPos(void)
{
    if (g_mt_usingMem) return (uint32)g_mt_memPos;
    if (g_mt_diskFile) return (uint32)ftell(g_mt_diskFile);
    return 0;
}

uint32 FILE_Read(uint32 length, uint8* where)
{
    if (g_mt_usingMem) {
        if (!g_mt_memData) return 0;
        uint32 avail = (uint32)(g_mt_memSize - g_mt_memPos);
        if (length > avail) length = avail;
        memcpy(where, g_mt_memData + g_mt_memPos, length);
        g_mt_memPos += length;
        return length;
    }
    if (g_mt_diskFile) {
        return (uint32)fread(where, 1, length, g_mt_diskFile);
    }
    return 0;
}

uint8 FILE_Readbyte(void)
{
    uint8 val = 0;
    FILE_Read(1, &val);
    return val;
}

uint16 FILE_Readword(void)
{
    uint16 val = 0;
    FILE_Read(2, (uint8*)&val);
    return val;
}

uint32 FILE_Readlong(void)
{
    uint32 val = 0;
    FILE_Read(4, (uint8*)&val);
    return val;
}

void FILE_Close(void)
{
    if (g_mt_diskFile) { fclose(g_mt_diskFile); g_mt_diskFile = nullptr; }
}

int8 FILE_FindCaseInsensitive(int8* basepath, int8* filename, int8* result)
{
    char fullpath[1024];
    snprintf(fullpath, sizeof(fullpath), "%s%s", (char*)basepath, (char*)filename);

    std::wstring wpath = utf8ToWide(fullpath);
    if (!wpath.empty()) {
        DWORD attr = GetFileAttributesW(wpath.c_str());
        if (attr != INVALID_FILE_ATTRIBUTES && !(attr & FILE_ATTRIBUTE_DIRECTORY)) {
            strcpy((char*)result, (char*)filename);
            return 1;
        }
    }

    DWORD attr = GetFileAttributesA(fullpath);
    if (attr != INVALID_FILE_ATTRIBUTES && !(attr & FILE_ATTRIBUTE_DIRECTORY)) {
        strcpy((char*)result, (char*)filename);
        return 1;
    }

    return 0;
}

} // extern "C"

class input_megatracker : public input_stubs {
public:
    void open(service_ptr_t<file> p_filehint, const char* p_path,
              t_input_open_reason p_reason, abort_callback& p_abort)
    {
        if (p_reason == input_open_info_write)
            throw exception_tagging_unsupported();

        m_file = p_filehint;
        input_open_file_helper(m_file, p_path, p_reason, p_abort);

        t_filesize fileSize = m_file->get_size(p_abort);
        if (fileSize == filesize_invalid || fileSize < 16 || fileSize > 64 * 1024 * 1024)
            throw exception_io_unsupported_format();

        m_fileData.resize(static_cast<size_t>(fileSize));
        m_file->read_object(m_fileData.data(), m_fileData.size(), p_abort);

        m_plugin = detectFormatFromBytes(m_fileData.data(), m_fileData.size());
        if (!m_plugin)
            throw exception_io_unsupported_format();

        if (m_plugin == &ssmt_plugin)
            m_formatName = "SoundSmith";
        else if (m_plugin == &ntgs_plugin)
            m_formatName = "NoiseTracker GS";
        else
            m_formatName = "Will Harvey SMUS";

        m_nativePath = toNativePath(p_path);
    }

    ~input_megatracker()
    {
        std::lock_guard<std::mutex> lock(g_mt_mutex);
        if (g_mt_activeInstance == this)
        {
            GUS_Exit();
            if (m_plugin) m_plugin->Close();
            if (g_mt_diskFile) { fclose(g_mt_diskFile); g_mt_diskFile = nullptr; }
            g_mt_activeInstance = nullptr;
        }
    }

    unsigned get_subsong_count() { return 1; }
    t_uint32 get_subsong(unsigned) { return 0; }

    void get_info(t_uint32, file_info& p_info, abort_callback&)
    {
        int dur = nostalgia_cfg::cfg_default_duration_sec.get();
        if (dur > 0) p_info.set_length(dur);

        p_info.info_set_int("samplerate", frequency);
        p_info.info_set_int("channels", 2);
        p_info.info_set_int("bitspersample", 16);
        p_info.info_set("encoding", "synthesized");
        p_info.info_set("codec", m_formatName.c_str());
    }

    t_filestats2 get_stats2(unsigned f, abort_callback& a) { return m_file->get_stats2_(f, a); }
    t_filestats get_file_stats(abort_callback& a) { return m_file->get_stats(a); }

    void decode_initialize(t_uint32, unsigned, abort_callback&)
    {
        std::lock_guard<std::mutex> lock(g_mt_mutex);

        if (g_mt_activeInstance && g_mt_activeInstance != this)
        {
            GUS_Exit();
            if (g_mt_activeInstance->m_plugin)
                g_mt_activeInstance->m_plugin->Close();
            if (g_mt_diskFile) { fclose(g_mt_diskFile); g_mt_diskFile = nullptr; }
            g_mt_activeInstance->m_decoding = false;
        }
        g_mt_activeInstance = this;

        g_mt_memData = m_fileData.data();
        g_mt_memSize = m_fileData.size();
        g_mt_memPos = 0;
        g_mt_songPath = m_nativePath;
        g_mt_usingMem = true;

        GUS_Init();
        int8 loaded = m_plugin->LoadFile((char*)m_nativePath.c_str());
        if (!loaded)
        {
            GUS_Exit();
            g_mt_activeInstance = nullptr;
            throw exception_io_unsupported_format();
        }

        m_channels = m_plugin->GetNumChannels();

        m_plugin->SetOldstyle(0);
        m_plugin->SetScroll(0);
        m_plugin->PlayStart();
        GUS_PlayStart();
        m_numSamples = 0;
        m_decoding = true;
        m_totalRendered = 0;
    }

    bool decode_run(audio_chunk& p_chunk, abort_callback&)
    {
        if (!m_decoding) return false;

        std::lock_guard<std::mutex> lock(g_mt_mutex);
        if (g_mt_activeInstance != this) { m_decoding = false; return false; }

        g_mt_memData = m_fileData.data();
        g_mt_memSize = m_fileData.size();

        static constexpr uint32_t kBlockSize = 2048;
        float buffer[kBlockSize * 2];
        uint32_t rendered = 0;

        while (rendered < kBlockSize)
        {
            if (m_numSamples == 0)
            {
                if (!m_plugin->GetSongStat())
                {
                    m_plugin->ResetSongStat();
                    break;
                }
                if (!safe_gus_update()) {
                    m_decoding = false;
                    return false;
                }
                m_numSamples = SPT;
            }

            uint32_t toRender = kBlockSize - rendered;
            if (toRender > m_numSamples) toRender = m_numSamples;

            int16_t* src = GUS_GetSamples();
            if (!src) { m_decoding = false; return false; }
            src += (SPT - m_numSamples) * 2;
            for (uint32_t i = 0; i < toRender * 2; i++)
                buffer[rendered * 2 + i] = src[i] / 32768.0f;

            rendered += toRender;
            m_numSamples -= toRender;
        }

        if (rendered == 0)
        {
            m_decoding = false;
            return false;
        }

        m_totalRendered += rendered;
        uint64_t maxFrames = (uint64_t)frequency * nostalgia_cfg::cfg_default_duration_sec.get();
        if (maxFrames > 0 && m_totalRendered > maxFrames)
        {
            m_decoding = false;
            return false;
        }

        p_chunk.set_data_32(buffer, rendered, 2, frequency);
        return true;
    }

    void decode_seek(double p_seconds, abort_callback& p_abort)
    {
        std::lock_guard<std::mutex> lock(g_mt_mutex);
        if (g_mt_activeInstance != this) return;

        GUS_Exit();
        m_plugin->Close();

        g_mt_memData = m_fileData.data();
        g_mt_memSize = m_fileData.size();
        g_mt_memPos = 0;
        g_mt_usingMem = true;

        GUS_Init();
        int8 loaded = m_plugin->LoadFile((char*)m_nativePath.c_str());
        if (!loaded) { m_decoding = false; return; }

        m_plugin->SetOldstyle(0);
        m_plugin->SetScroll(0);
        m_plugin->PlayStart();
        GUS_PlayStart();
        m_numSamples = 0;
        m_totalRendered = 0;
        m_decoding = true;

        uint64_t targetSamples = (uint64_t)(p_seconds * frequency);
        while (m_totalRendered < targetSamples)
        {
            p_abort.check();
            if (m_numSamples == 0)
            {
                if (!m_plugin->GetSongStat()) break;
                if (!safe_gus_update()) { m_decoding = false; return; }
                m_numSamples = SPT;
            }
            uint32_t toSkip = m_numSamples;
            uint64_t left = targetSamples - m_totalRendered;
            if (toSkip > left) toSkip = (uint32_t)left;
            m_totalRendered += toSkip;
            m_numSamples -= toSkip;
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
        if (!nostalgia_cfg::cfg_megatracker_enabled.get()) return false;
        if (!(p_extension == nullptr || p_extension[0] == '\0')) return false;

        uint8_t hdr[64] = {};
        size_t got = readHeaderFromPath(p_path, hdr, sizeof(hdr));
        if (got < 32) return false;
        return detectFormatFromBytes(hdr, got) != nullptr;
    }

    static const char* g_get_name() { return "MegaTracker decoder"; }
    static const GUID g_get_guid() {
        static const GUID guid = { 0xcc33dd44, 0xee55, 0x4ff6, { 0x00, 0x77, 0x11, 0x99, 0x22, 0xaa, 0x33, 0xbb } };
        return guid;
    }

private:
    service_ptr_t<file> m_file;
    std::vector<uint8_t> m_fileData;
    MTPInputT* m_plugin = nullptr;
    std::string m_formatName;
    std::string m_nativePath;
    int m_channels = 0;
    uint32_t m_numSamples = 0;
    uint64_t m_totalRendered = 0;
    bool m_decoding = false;
};

} // anonymous namespace

static input_factory_t<input_megatracker> g_input_megatracker_factory;
