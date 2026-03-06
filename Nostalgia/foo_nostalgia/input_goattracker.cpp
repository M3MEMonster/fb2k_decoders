#include <foobar2000.h>
#include "cfg_vars.h"

#include <vector>
#include <string>
#include <cstring>
#include <mutex>
#include <windows.h>

#define MAX_CHN 6
#define MAX_SONGS 32
#define MAX_SONGLEN 254
#define MAX_STR 32
#define MAX_FILENAME 256
#define MAX_PATHNAME 256
#define PLAY_BEGINNING 0x01
#define PLAY_STOP 0x04
#define EDIT_PATTERN 0
#define KEY_TRACKER 0
#define FORMAT_PRG 4

extern "C" {

void initchannels(void);
void clearsong(int cs, int cp, int ci, int cf, int cn);
void loadsong(void);
void stopsong(void);
void initsong(int num, int playmode);
void playroutine(void);
void sid_init(int speed, unsigned m, unsigned ntsc, unsigned interpolate, unsigned customclockrate, unsigned usefp);
void sid_uninit(void);
int sid_fillbuffer(short* ptr, int samples);

extern char songname[MAX_STR];
extern char authorname[MAX_STR];
extern char copyrightname[MAX_STR];
extern int songlen[MAX_SONGS][MAX_CHN];
extern int numchannels;
extern int songinit;
extern int esnum;

extern int editmode;
extern int recordmode;
extern int followplay;
extern int hexnybble;
extern int stepsize;
extern int autoadvance;
extern int defaultpatternlength;
extern int exitprogram;
extern unsigned keypreset;
extern unsigned playerversion;
extern int fileformat;
extern int zeropageadr;
extern int playeradr;
extern unsigned sidmodel;
extern unsigned multiplier;
extern unsigned adparam;
extern unsigned ntsc;
extern unsigned sidaddress;
extern unsigned finevibrato;
extern unsigned optimizepulse;
extern unsigned optimizerealtime;
extern unsigned interpolate;
extern unsigned residdelay;
extern char loadedsongfilename[MAX_FILENAME];
extern char songfilename[MAX_FILENAME];
extern char instrfilename[MAX_FILENAME];
extern char packedpath[MAX_PATHNAME];
extern char* programname;
extern char textbuffer[MAX_PATHNAME];
extern int stereosid;
extern int win_quitted;
extern int key;
extern int rawkey;
extern int shiftpressed;

} // extern "C" declarations

int editmode = EDIT_PATTERN;
int recordmode = 0;
int followplay = 0;
int hexnybble = -1;
int stepsize = 4;
int autoadvance = 0;
int defaultpatternlength = 64;
int exitprogram = 0;
unsigned keypreset = KEY_TRACKER;
unsigned playerversion = 0;
int fileformat = FORMAT_PRG;
int zeropageadr = 0xfc;
int playeradr = 0x1000;
unsigned sidmodel = 0;
unsigned multiplier = 1;
unsigned adparam = 0x0f00;
unsigned ntsc = 0;
unsigned sidaddress = 0xd400;
unsigned finevibrato = 1;
unsigned optimizepulse = 0;
unsigned optimizerealtime = 0;
unsigned interpolate = 0;
unsigned residdelay = 0;

char loadedsongfilename[MAX_FILENAME] = { 0 };
char songfilename[MAX_FILENAME] = "song.sng";
char instrfilename[MAX_FILENAME];
char packedpath[MAX_PATHNAME];
char* programname;
char textbuffer[MAX_PATHNAME];
int stereosid = 1;
int win_quitted = 0;
int key = 0;
int rawkey = 0;
int shiftpressed = 0;

namespace {

struct MemStream {
    const uint8_t* data;
    size_t size;
    size_t pos;
};

static std::mutex g_gt_mutex;
static const uint8_t* g_gt_fileData = nullptr;
static size_t g_gt_fileSize = 0;

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

class input_goattracker;
static input_goattracker* g_gt_activeInstance = nullptr;

} // anonymous namespace

extern "C" {

void* replayer_fopen(const char* filename, const char*)
{
    if (strcmp(filename, "song.sng") == 0 && g_gt_fileData)
    {
        auto* ms = new MemStream{ g_gt_fileData, g_gt_fileSize, 0 };
        return ms;
    }
    return nullptr;
}

int replayer_fread(void* buffer, size_t size, size_t count, void* stream)
{
    auto* ms = static_cast<MemStream*>(stream);
    if (!ms) return 0;
    size_t total = size * count;
    size_t avail = ms->size - ms->pos;
    if (total > avail) total = avail;
    memcpy(buffer, ms->data + ms->pos, total);
    ms->pos += total;
    return (int)(total / size);
}

int replayer_fwrite(void*, size_t, size_t, void*) { return 0; }

int replayer_fseek(void* stream, long offset, int origin)
{
    auto* ms = static_cast<MemStream*>(stream);
    if (!ms) return -1;
    switch (origin) {
    case 0: ms->pos = (size_t)offset; break;
    case 1: ms->pos += offset; break;
    case 2: ms->pos = ms->size + offset; break;
    }
    if (ms->pos > ms->size) ms->pos = ms->size;
    return 0;
}

void replayer_fclose(void* stream)
{
    delete static_cast<MemStream*>(stream);
}

int replayer_ftell(void* stream)
{
    auto* ms = static_cast<MemStream*>(stream);
    return ms ? (int)ms->pos : 0;
}

int replayer_fprintf(void* const, char const* const, ...) { return 0; }

void fwrite8(void*, unsigned) {}
void fwritele16(void*, unsigned) {}
unsigned fread8(void* file) { uint8_t buf = 0; replayer_fread(&buf, 1, 1, file); return buf; }

int io_open(char*) { return 0; }
int io_lseek(int, int, int) { return 0; }
void io_close(int) {}
unsigned io_read8(int) { return 0; }

void clearscreen(void) {}
void fliptoscreen(void) {}
void printtext(int, int, int, const char*) {}
void printtextc(int, int, const char*) {}
void printblankc(int, int, int, int) {}

void printmainscreen(void) {}
void resettime(void) {}
void incrementtime(void) {}

int fileselector(char*, char*, char*, char*, int) { return 0; }
void editstring(char*, int) {}

void sound_suspend(void) {}
void sound_flush(void) {}

void waitkeynoupdate(void) {}

} // extern "C" implementations

namespace {

static constexpr uint32_t kGoatSampleRate = 48000;

class input_goattracker : public input_stubs {
public:
    void open(service_ptr_t<file> p_filehint, const char* p_path,
              t_input_open_reason p_reason, abort_callback& p_abort)
    {
        if (p_reason == input_open_info_write)
            throw exception_tagging_unsupported();

        m_file = p_filehint;
        input_open_file_helper(m_file, p_path, p_reason, p_abort);

        t_filesize fileSize = m_file->get_size(p_abort);
        if (fileSize == filesize_invalid || fileSize > 64 * 1024 * 1024)
            throw exception_io_unsupported_format();

        m_fileData.resize(static_cast<size_t>(fileSize));
        m_file->read_object(m_fileData.data(), m_fileData.size(), p_abort);
    }

    ~input_goattracker()
    {
        std::lock_guard<std::mutex> lock(g_gt_mutex);
        if (g_gt_activeInstance == this)
        {
            stopsong();
            sid_uninit();
            g_gt_activeInstance = nullptr;
        }
    }

    unsigned get_subsong_count() { return m_numSongs > 0 ? m_numSongs : 1; }
    t_uint32 get_subsong(unsigned p_index) { return p_index; }

    void get_info(t_uint32 p_subsong, file_info& p_info, abort_callback& p_abort)
    {
        int dur = nostalgia_cfg::cfg_default_duration_sec.get();
        if (dur > 0) p_info.set_length(dur);

        p_info.info_set_int("samplerate", kGoatSampleRate);
        p_info.info_set_int("channels", 2);
        p_info.info_set_int("bitspersample", 16);
        p_info.info_set("encoding", "synthesized");
        p_info.info_set("codec", "GoatTracker SID");

        if (!m_title.empty())
            p_info.meta_set("title", m_title.c_str());
        if (!m_artist.empty())
            p_info.meta_set("artist", m_artist.c_str());
    }

    t_filestats2 get_stats2(unsigned f, abort_callback& a) { return m_file->get_stats2_(f, a); }
    t_filestats get_file_stats(abort_callback& a) { return m_file->get_stats(a); }

    void decode_initialize(t_uint32 p_subsong, unsigned p_flags, abort_callback& p_abort)
    {
        std::lock_guard<std::mutex> lock(g_gt_mutex);

        if (g_gt_activeInstance && g_gt_activeInstance != this)
        {
            stopsong();
            sid_uninit();
            g_gt_activeInstance->m_decoding = false;
        }
        g_gt_activeInstance = this;

        g_gt_fileData = m_fileData.data();
        g_gt_fileSize = m_fileData.size();

        multiplier = 1;
        finevibrato = 1;
        optimizepulse = 0;
        optimizerealtime = 0;
        adparam = 0x0f00;

        initchannels();
        clearsong(1, 1, 1, 1, 1);
        loadsong();

        if (strcmp(loadedsongfilename, songfilename) != 0)
        {
            g_gt_fileData = nullptr;
            g_gt_activeInstance = nullptr;
            throw exception_io_unsupported_format();
        }

        m_numSongs = 0;
        for (int c = 0; c < MAX_SONGS; c++)
        {
            if (songlen[c][0] && songlen[c][1] && songlen[c][2] &&
                songlen[c][3] && songlen[c][4] && songlen[c][5])
                m_numSongs++;
        }

        sid_init(kGoatSampleRate, 0, 0, 0, 0, 0);

        int subsongIndex = 0;
        uint32_t songCount = 0;
        for (subsongIndex = 0; subsongIndex < MAX_SONGS; subsongIndex++)
        {
            if (songlen[subsongIndex][0] && songlen[subsongIndex][1] &&
                songlen[subsongIndex][2] && songlen[subsongIndex][3] &&
                songlen[subsongIndex][4] && songlen[subsongIndex][5])
            {
                if (songCount == p_subsong) break;
                songCount++;
            }
        }
        initsong(subsongIndex, PLAY_BEGINNING);
        playroutine();

        m_title = songname;
        m_artist = authorname;

        g_gt_fileData = nullptr;
        m_remainingSamples = 0;
        m_decoding = true;
        m_totalRendered = 0;
        m_subsongIndex = subsongIndex;
    }

    bool decode_run(audio_chunk& p_chunk, abort_callback& p_abort)
    {
        if (!m_decoding) return false;

        std::lock_guard<std::mutex> lock(g_gt_mutex);
        if (g_gt_activeInstance != this) { m_decoding = false; return false; }

        static constexpr uint32_t kBlockSize = 1024;
        int16_t buffer[kBlockSize * 2];

        uint32_t rendered = 0;
        uint32_t remaining = kBlockSize;

        while (remaining > 0)
        {
            if (!m_remainingSamples)
            {
                playroutine();
                uint32_t tempo = ntsc ? 150 : 125;
                if (multiplier)
                    m_remainingSamples = ((kGoatSampleRate * 5) >> 1) / tempo / multiplier;
                else
                    m_remainingSamples = ((kGoatSampleRate * 5) >> 1) * 2 / tempo;

                if (songinit == PLAY_STOP)
                {
                    if (rendered > 0) break;
                    m_decoding = false;
                    return false;
                }
            }

            uint32_t toRender = remaining < m_remainingSamples ? remaining : m_remainingSamples;
            sid_fillbuffer(buffer + rendered * 2, toRender);
            rendered += toRender;
            remaining -= toRender;
            m_remainingSamples -= toRender;
        }

        if (rendered == 0)
        {
            m_decoding = false;
            return false;
        }

        m_totalRendered += rendered;
        uint64_t maxFrames = (uint64_t)kGoatSampleRate * nostalgia_cfg::cfg_default_duration_sec.get();
        if (maxFrames > 0 && m_totalRendered > maxFrames)
        {
            m_decoding = false;
            return false;
        }

        float floatBuf[kBlockSize * 2];
        for (uint32_t i = 0; i < rendered * 2; i++)
            floatBuf[i] = buffer[i] / 32768.0f;

        p_chunk.set_data_32(floatBuf, rendered, 2, kGoatSampleRate);
        return true;
    }

    void decode_seek(double p_seconds, abort_callback& p_abort)
    {
        std::lock_guard<std::mutex> lock(g_gt_mutex);
        if (g_gt_activeInstance != this) return;

        g_gt_fileData = m_fileData.data();
        g_gt_fileSize = m_fileData.size();
        stopsong();
        sid_uninit();
        sid_init(kGoatSampleRate, 0, 0, 0, 0, 0);
        initsong(m_subsongIndex, PLAY_BEGINNING);
        playroutine();
        g_gt_fileData = nullptr;
        m_remainingSamples = 0;
        m_totalRendered = 0;
        m_decoding = true;

        uint64_t targetSamples = (uint64_t)(p_seconds * kGoatSampleRate);
        int16_t discardBuf[1024 * 2];
        while (m_totalRendered < targetSamples)
        {
            p_abort.check();
            if (!m_remainingSamples)
            {
                playroutine();
                uint32_t tempo = ntsc ? 150 : 125;
                if (multiplier)
                    m_remainingSamples = ((kGoatSampleRate * 5) >> 1) / tempo / multiplier;
                else
                    m_remainingSamples = ((kGoatSampleRate * 5) >> 1) * 2 / tempo;
                if (songinit == PLAY_STOP) break;
            }
            uint32_t toRender = 1024;
            uint64_t left = targetSamples - m_totalRendered;
            if (toRender > left) toRender = (uint32_t)left;
            if (toRender > m_remainingSamples) toRender = m_remainingSamples;
            sid_fillbuffer(discardBuf, toRender);
            m_totalRendered += toRender;
            m_remainingSamples -= toRender;
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
        if (!nostalgia_cfg::cfg_goattracker_enabled.get()) return false;
        if (p_extension == nullptr || stricmp_utf8(p_extension, "sng") != 0) return false;

        uint8_t hdr[4] = {};
        if (!g_read_header(p_path, hdr, sizeof(hdr))) return true;
        return memcmp(hdr, "GTS", 3) == 0;
    }
    static const char* g_get_name() { return "GoatTracker decoder"; }
    static const GUID g_get_guid() {
        static const GUID guid = { 0xaa11bb22, 0xcc33, 0x4dd4, { 0xee, 0x55, 0xff, 0x66, 0x00, 0x77, 0x11, 0x88 } };
        return guid;
    }

private:
    service_ptr_t<file> m_file;
    std::vector<uint8_t> m_fileData;
    std::string m_title, m_artist;
    uint32_t m_numSongs = 0;
    uint32_t m_remainingSamples = 0;
    uint64_t m_totalRendered = 0;
    int m_subsongIndex = 0;
    bool m_decoding = false;
};

} // anonymous namespace

static input_factory_t<input_goattracker> g_input_goattracker_factory;
