#include <foobar2000.h>

#include <mutex>
#include <vector>
#include <string>
#include <algorithm>
#include <cstring>

extern "C" {
#include "3rdParty/Quartet/zingzong/src/zingzong.h"
}

extern "C" zz_vfs_dri_t zz_ice_vfs(void);

namespace {

static constexpr uint32_t kSampleRate = 48000;
static constexpr int kNumChannels = 2;
static constexpr int kBlockSize = 1024;

static std::mutex g_quartetMutex;
static const uint8_t* g_activeFileData = nullptr;
static size_t g_activeFileSize = 0;
static std::string g_activeFilePath;

struct FbVfsHandle : public vfs_s {
    std::string uri;
    const uint8_t* data = nullptr;
    size_t dataSize = 0;
    size_t pos = 0;
    std::vector<uint8_t> ownedData;
    bool valid = false;
};

static zz_err_t fb_vfs_reg(zz_vfs_dri_t) { return 0; }
static zz_err_t fb_vfs_unreg(zz_vfs_dri_t) { return 0; }
static zz_u16_t fb_vfs_ismine(const char* uri) { return (!!*uri) << 10; }

static zz_vfs_t fb_vfs_create(const char*, va_list);
static void fb_vfs_destroy(zz_vfs_t);
static const char* fb_vfs_uri(zz_vfs_t);
static zz_err_t fb_vfs_open(zz_vfs_t);
static zz_err_t fb_vfs_close(zz_vfs_t);
static zz_u32_t fb_vfs_read(zz_vfs_t, void*, zz_u32_t);
static zz_u32_t fb_vfs_tell(zz_vfs_t);
static zz_u32_t fb_vfs_size(zz_vfs_t);
static zz_err_t fb_vfs_seek(zz_vfs_t, zz_u32_t, zz_u8_t);

static struct zz_vfs_dri_s g_fbDriver = {
    "foobar2000",
    fb_vfs_reg, fb_vfs_unreg, fb_vfs_ismine,
    fb_vfs_create, fb_vfs_destroy, fb_vfs_uri,
    fb_vfs_open, fb_vfs_close, fb_vfs_read,
    fb_vfs_tell, fb_vfs_size, fb_vfs_seek
};

static zz_vfs_t fb_vfs_create(const char* uri, va_list) {
    auto* fh = new FbVfsHandle();
    std::memset(static_cast<vfs_s*>(fh), 0, sizeof(vfs_s));
    fh->dri = &g_fbDriver;
    fh->uri = uri;

    if (g_activeFilePath == uri) {
        fh->data = g_activeFileData;
        fh->dataSize = g_activeFileSize;
        fh->valid = true;
    } else {
        try {
            abort_callback_dummy abort;
            service_ptr_t<file> f;
            filesystem::g_open(f, uri, filesystem::open_mode_read, abort);
            t_filesize sz = f->get_size(abort);
            if (sz > 0 && sz < 128 * 1024 * 1024) {
                fh->ownedData.resize(static_cast<size_t>(sz));
                f->read_object(fh->ownedData.data(), static_cast<t_size>(sz), abort);
                fh->data = fh->ownedData.data();
                fh->dataSize = fh->ownedData.size();
                fh->valid = true;
            }
        } catch (...) {
            int wlen = MultiByteToWideChar(CP_UTF8, 0, uri, -1, nullptr, 0);
            if (wlen > 0) {
                std::vector<wchar_t> wpath(wlen);
                MultiByteToWideChar(CP_UTF8, 0, uri, -1, wpath.data(), wlen);
                FILE* fp = _wfopen(wpath.data(), L"rb");
                if (fp) {
                    _fseeki64(fp, 0, SEEK_END);
                    long long sz = _ftelli64(fp);
                    _fseeki64(fp, 0, SEEK_SET);
                    if (sz > 0 && sz < 128LL * 1024 * 1024) {
                        fh->ownedData.resize(static_cast<size_t>(sz));
                        fread(fh->ownedData.data(), 1, static_cast<size_t>(sz), fp);
                        fh->data = fh->ownedData.data();
                        fh->dataSize = fh->ownedData.size();
                        fh->valid = true;
                    }
                    fclose(fp);
                }
            }
        }
    }

    if (!fh->valid) {
        delete fh;
        return nullptr;
    }
    return fh;
}

static void fb_vfs_destroy(zz_vfs_t vfs) {
    delete reinterpret_cast<FbVfsHandle*>(vfs);
}

static const char* fb_vfs_uri(zz_vfs_t vfs) {
    return reinterpret_cast<FbVfsHandle*>(vfs)->uri.c_str();
}

static zz_err_t fb_vfs_open(zz_vfs_t vfs) {
    auto* fh = reinterpret_cast<FbVfsHandle*>(vfs);
    fh->pos = 0;
    return fh->valid ? 0 : -1;
}

static zz_err_t fb_vfs_close(zz_vfs_t) { return 0; }

static zz_u32_t fb_vfs_read(zz_vfs_t vfs, void* ptr, zz_u32_t n) {
    auto* fh = reinterpret_cast<FbVfsHandle*>(vfs);
    fh->err = 0;
    if (fh->pos >= fh->dataSize) return 0;
    size_t avail = fh->dataSize - fh->pos;
    if (n > avail) n = static_cast<zz_u32_t>(avail);
    std::memcpy(ptr, fh->data + fh->pos, n);
    fh->pos += n;
    return n;
}

static zz_u32_t fb_vfs_tell(zz_vfs_t vfs) {
    auto* fh = reinterpret_cast<FbVfsHandle*>(vfs);
    fh->err = 0;
    return static_cast<zz_u32_t>(fh->pos);
}

static zz_u32_t fb_vfs_size(zz_vfs_t vfs) {
    auto* fh = reinterpret_cast<FbVfsHandle*>(vfs);
    fh->err = 0;
    return static_cast<zz_u32_t>(fh->dataSize);
}

static zz_err_t fb_vfs_seek(zz_vfs_t vfs, zz_u32_t offset, zz_u8_t whence) {
    auto* fh = reinterpret_cast<FbVfsHandle*>(vfs);
    fh->err = 0;
    int64_t newPos;
    switch (whence) {
    case ZZ_SEEK_SET: newPos = offset; break;
    case ZZ_SEEK_CUR: newPos = static_cast<int64_t>(fh->pos) + offset; break;
    case ZZ_SEEK_END: newPos = static_cast<int64_t>(fh->dataSize) + offset; break;
    default: return -1;
    }
    if (newPos < 0 || newPos > static_cast<int64_t>(fh->dataSize)) return -1;
    fh->pos = static_cast<size_t>(newPos);
    return 0;
}

static zz_err_t load_player(zz_play_t& player,
                            const uint8_t* fileData, size_t fileSize,
                            const char* filePath, zz_u8_t* pFormat,
                            zz_info_t* pInfo)
{
    g_activeFileData = fileData;
    g_activeFileSize = fileSize;
    g_activeFilePath = filePath;

    zz_vfs_add(&g_fbDriver);
    zz_vfs_add(zz_ice_vfs());

    zz_err_t err = zz_new(&player);
    if (err == 0)
        err = zz_load(player, filePath, nullptr, pFormat);
    if (err == 0)
        err = zz_init(player, 0, ZZ_EOF);
    if (err == 0)
        err = zz_setup(player, ZZ_MIXER_DEF, kSampleRate);
    if (err == 0 && pInfo)
        zz_info(player, pInfo);

    zz_vfs_del(zz_ice_vfs());
    zz_vfs_del(&g_fbDriver);

    if (err != 0 && player) {
        zz_del(&player);
        player = nullptr;
    }
    return err;
}

class input_quartet : public input_stubs {
public:
    input_quartet() = default;

    ~input_quartet() {
        if (m_player)
            zz_del(&m_player);
    }

    void open(service_ptr_t<file> p_filehint, const char* p_path,
              t_input_open_reason p_reason, abort_callback& p_abort)
    {
        if (p_reason == input_open_info_write)
            throw exception_tagging_unsupported();

        m_file = p_filehint;
        input_open_file_helper(m_file, p_path, p_reason, p_abort);
        m_filePath = p_path;

        t_filesize fsize = m_file->get_size(p_abort);
        if (fsize < 8 || fsize > 128 * 1024 * 1024)
            throw exception_io_unsupported_format();

        m_fileData.resize(static_cast<size_t>(fsize));
        m_file->seek(0, p_abort);
        m_file->read_object(m_fileData.data(), static_cast<t_size>(fsize), p_abort);

        std::lock_guard<std::mutex> lock(g_quartetMutex);
        std::memset(&m_info, 0, sizeof(m_info));

        zz_err_t err = load_player(m_player, m_fileData.data(), m_fileData.size(),
                                   m_filePath.c_str(), &m_format, &m_info);
        if (err != 0)
            throw exception_io_data("Failed to load Quartet file");
    }

    unsigned get_subsong_count() { return 1; }
    t_uint32 get_subsong(unsigned) { return 0; }

    void get_info(t_uint32, file_info& p_info, abort_callback&) {
        if (m_info.len.ms > 0)
            p_info.set_length(m_info.len.ms / 1000.0);

        p_info.info_set_int("samplerate", kSampleRate);
        p_info.info_set_int("channels", kNumChannels);
        p_info.info_set_int("bitspersample", 16);
        p_info.info_set("encoding", "synthesized");

        std::string codec = "Quartet ";
        codec += m_info.fmt.str;
        p_info.info_set("codec", codec.c_str());

        if (m_info.tag.title && m_info.tag.title[0])
            p_info.meta_set("title", m_info.tag.title);
        if (m_info.tag.artist && m_info.tag.artist[0])
            p_info.meta_set("artist", m_info.tag.artist);
        if (m_info.tag.album && m_info.tag.album[0])
            p_info.meta_set("album", m_info.tag.album);
    }

    t_filestats2 get_stats2(unsigned f, abort_callback& a) {
        return m_file->get_stats2_(f, a);
    }

    t_filestats get_file_stats(abort_callback& p_abort) {
        return m_file->get_stats(p_abort);
    }

    void decode_initialize(t_uint32, unsigned, abort_callback&) {
        m_decoding = true;
    }

    bool decode_run(audio_chunk& p_chunk, abort_callback&) {
        if (!m_decoding || !m_player) return false;

        int16_t pcmBuffer[kBlockSize * kNumChannels];
        auto ret = zz_play(m_player, pcmBuffer, kBlockSize);

        if (ret <= 0) {
            m_decoding = false;
            return false;
        }

        p_chunk.set_data_fixedpoint(
            pcmBuffer,
            ret * kNumChannels * sizeof(int16_t),
            kSampleRate, kNumChannels, 16,
            audio_chunk::channel_config_stereo);
        return true;
    }

    void decode_seek(double p_seconds, abort_callback&) {
        if (!m_player) return;

        uint32_t targetMs = static_cast<uint32_t>(p_seconds * 1000.0);
        uint32_t currentMs = zz_position(m_player);

        if (currentMs == ZZ_EOF || currentMs > targetMs) {
            std::lock_guard<std::mutex> lock(g_quartetMutex);
            zz_del(&m_player);
            m_player = nullptr;

            zz_err_t err = load_player(m_player, m_fileData.data(), m_fileData.size(),
                                       m_filePath.c_str(), &m_format, nullptr);
            if (err != 0) {
                m_decoding = false;
                return;
            }
            currentMs = 0;
        }

        uint64_t samplesToSkip = static_cast<uint64_t>(targetMs - currentMs) * kSampleRate / 1000;
        while (samplesToSkip > 0) {
            zz_i16_t n = static_cast<zz_i16_t>((std::min)(static_cast<uint64_t>(kBlockSize), samplesToSkip));
            auto ret = zz_play(m_player, nullptr, n);
            if (ret <= 0) break;
            samplesToSkip -= ret;
        }

        m_decoding = true;
    }

    bool decode_can_seek() { return true; }
    bool decode_get_dynamic_info(file_info&, double&) { return false; }
    bool decode_get_dynamic_info_track(file_info&, double&) { return false; }
    void decode_on_idle(abort_callback&) {}

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
        return stricmp_utf8(p_extension, "4v") == 0
            || stricmp_utf8(p_extension, "qts") == 0;
    }
    static const char* g_get_name() { return "Quartet decoder"; }
    static const GUID g_get_guid() {
        static const GUID guid = {
            0x8b4c6d2e, 0xf3a5, 0x4b7c,
            { 0x9d, 0xae, 0x1f, 0x2a, 0x3b, 0x4c, 0x5d, 0x6e }
        };
        return guid;
    }

private:
    service_ptr_t<file> m_file;
    std::string m_filePath;
    std::vector<uint8_t> m_fileData;

    zz_play_t m_player = nullptr;
    zz_u8_t m_format = 0;
    zz_info_t m_info = {};
    bool m_decoding = false;
};

} // anonymous namespace

static input_factory_t<input_quartet> g_input_quartet_factory;
