#include "stdafx.h"

#include "adplug/adplug.h"
#include "adplug/emuopl.h"
#include "adplug/kemuopl.h"
#include "adplug/nemuopl.h"
#include "adplug/wemuopl.h"
#include "adplug/surroundopl.h"
#include "adplug/players.h"
#include "adplug/database.h"
#include "adplug/version.h"
#include "adplug/libbinio/binstr.h"
#include "adplugdb.h"
#include "adlib_config.h"

#include <vector>
#include <string>
#include <memory>
#include <map>

static constexpr unsigned kChannels = 2;
static constexpr unsigned kBitsPerSample = 16;

static std::string fb2kPathToNative(const char* fb2kPath)
{
    pfc::string8 native;
    if (filesystem::g_get_native_path(fb2kPath, native))
        return std::string(native.get_ptr());

    if (_strnicmp(fb2kPath, "file://", 7) == 0)
        return std::string(fb2kPath + 7);

    return std::string(fb2kPath);
}

static std::string extractFilename(const std::string& path)
{
    auto slash = path.find_last_of("/\\");
    if (slash != std::string::npos)
        return path.substr(slash + 1);
    return path;
}

class Fb2kFileProvider : public CFileProvider
{
public:
    Fb2kFileProvider(const void* mainData, size_t mainSize,
                     const std::string& nativePath,
                     const char* fb2kPath)
        : m_mainData(static_cast<const uint8_t*>(mainData))
        , m_mainSize(mainSize)
        , m_nativePath(nativePath)
        , m_fb2kDir(extractFb2kDir(fb2kPath))
    {}

    binistream* open(std::string filename) const override
    {
        if (_stricmp(filename.c_str(), m_nativePath.c_str()) == 0)
        {
            return createMemStream(m_mainData, m_mainSize);
        }

        std::string reqName = extractFilename(filename);
        std::string mainName = extractFilename(m_nativePath);
        if (_stricmp(reqName.c_str(), mainName.c_str()) == 0)
        {
            return createMemStream(m_mainData, m_mainSize);
        }

        return openCompanionFile(reqName);
    }

    void close(binistream* stream) const override
    {
        if (stream) stream->seek(0, binio::Set);
    }

private:
    static std::string extractFb2kDir(const char* fb2kPath)
    {
        std::string p(fb2kPath);
        auto slash = p.find_last_of("/\\");
        if (slash != std::string::npos)
            return p.substr(0, slash + 1);
        return std::string();
    }

    binistream* createMemStream(const uint8_t* data, size_t size) const
    {
        auto stream = new binisstream(
            const_cast<uint8_t*>(data),
            static_cast<unsigned long>(size));
        m_streams.push_back(std::unique_ptr<binistream>(stream));
        return stream;
    }

    binistream* openCompanionFile(const std::string& basename) const
    {
        auto it = m_companionCache.find(basename);
        if (it != m_companionCache.end())
        {
            if (!it->second) return nullptr;
            return createMemStream(it->second->data(),
                                   it->second->size());
        }

        pfc::string8 companionUrl;
        companionUrl << m_fb2kDir.c_str() << basename.c_str();

        try
        {
            abort_callback_dummy abort;
            service_ptr_t<file> f;
            filesystem::g_open(f, companionUrl.get_ptr(),
                               filesystem::open_mode_read, abort);

            t_filesize fsize = f->get_size(abort);
            if (fsize == filesize_invalid || fsize > 4 * 1024 * 1024)
            {
                m_companionCache[basename] = nullptr;
                return nullptr;
            }

            auto buf = std::make_shared<std::vector<uint8_t>>(
                static_cast<size_t>(fsize));
            f->read_object(buf->data(), buf->size(), abort);
            m_companionCache[basename] = buf;

            return createMemStream(buf->data(), buf->size());
        }
        catch (...)
        {
            m_companionCache[basename] = nullptr;
            return nullptr;
        }
    }

    const uint8_t* m_mainData;
    size_t m_mainSize;
    std::string m_nativePath;
    std::string m_fb2kDir;

    mutable std::vector<std::unique_ptr<binistream>> m_streams;
    mutable std::map<std::string,
        std::shared_ptr<std::vector<uint8_t>>> m_companionCache;
};

static class AdPlugDatabaseInit
{
public:
    AdPlugDatabaseInit()
    {
        binisstream dbstream(const_cast<unsigned char*>(adplugdb), sizeof(adplugdb));
        m_db.load(dbstream);
        CAdPlug::set_database(&m_db);
    }

private:
    CAdPlugDatabase m_db;
} g_adplugDbInit;

static bool is_adlib_extension(const char* ext)
{
    for (auto& desc : CAdPlug::players)
    {
        for (unsigned i = 0; desc->get_extension(i); i++)
        {
            const char* pext = desc->get_extension(i);
            if (pext[0] == '.') pext++;
            if (_stricmp(ext, pext) == 0) return true;
        }
    }
    return false;
}

struct OplCore
{
    Copl* opl = nullptr;
    bool stereo = true;

    ~OplCore()
    {
        delete opl;
    }
};

// Creates a base OPL emulator for the given core index.
// coreIdx: 0=Harekiet's (CWemuopl), 1=Ken Silverman's (CKemuopl),
//          2=Jarek Burczynski's (CEmuopl), 3=Nuked OPL3 (CNemuopl)
static Copl* MakeBaseOpl(int coreIdx, unsigned sampleRate, bool stereo)
{
    switch (coreIdx)
    {
        case 1:  return new CKemuopl(sampleRate, true, stereo);
        case 2:  return new CEmuopl (sampleRate, true, stereo);
        case 3:  return new CNemuopl(sampleRate);
        default: return new CWemuopl(sampleRate, true, stereo);
    }
}

static OplCore* CreateOplCore(bool stereo)
{
    auto core       = new OplCore;
    core->stereo    = stereo;

    const unsigned sampleRate  = GetAdLibSampleRate();
    const int      coreIdx     = (int)cfg_adlib_core;
    const int      eqIdx       = (int)cfg_adlib_equalizer;
    const bool     useSurround = (bool)cfg_adlib_surround;

    if (useSurround)
    {
        // Wrap two OPL instances in CSurroundopl for the stereo harmonic effect.
        // CSurroundopl copies the COPLprops structs and takes ownership of the opl
        // pointers inside them, so stack allocation of the wrappers is fine.
        COPLprops a, b;
        a.opl      = MakeBaseOpl(coreIdx, sampleRate, true);
        a.use16bit = true;
        a.stereo   = true;
        b.opl      = MakeBaseOpl(coreIdx, sampleRate, true);
        b.use16bit = true;
        b.stereo   = true;

        auto* surround = new CSurroundopl(&a, &b, true);

        // Equalizer: ESS FM keeps the default FREQ_OFFSET=128 harmonic;
        // None sets offset to 0 (stereo doubling without frequency transposition).
        if (eqIdx != 0)
            surround->set_offset(0.0);

        core->opl = surround;
    }
    else
    {
        core->opl = MakeBaseOpl(coreIdx, sampleRate, stereo);
    }

    return core;
}

class input_adlib : public input_stubs
{
public:
    void open(service_ptr_t<file> p_filehint, const char* p_path, t_input_open_reason p_reason, abort_callback& p_abort)
    {
        if (p_reason == input_open_info_write)
            throw exception_tagging_unsupported();

        m_sampleRate = GetAdLibSampleRate();

        m_file = p_filehint;
        input_open_file_helper(m_file, p_path, p_reason, p_abort);

        m_path = p_path;
        m_nativePath = fb2kPathToNative(p_path);

        t_filesize size = m_file->get_size(p_abort);
        if (size == filesize_invalid || size < 8 || size > 1024 * 1024)
            throw exception_io_unsupported_format();

        m_fileData.resize(static_cast<size_t>(size));
        m_file->read_object(m_fileData.data(), m_fileData.size(), p_abort);

        auto core = CreateOplCore(true);
        Fb2kFileProvider fp(m_fileData.data(), m_fileData.size(),
                            m_nativePath, p_path);
        m_player = CAdPlug::factory(m_nativePath, core->opl,
                                    CAdPlug::players, fp);

        if (!m_player)
        {
            delete core;
            throw exception_io_unsupported_format();
        }

        m_core = core;
    }

    ~input_adlib()
    {
        delete m_player;
        delete m_core;
    }

    unsigned get_subsong_count()
    {
        return m_player ? m_player->getsubsongs() : 1;
    }

    t_uint32 get_subsong(unsigned p_index)
    {
        return p_index;
    }

    void get_info(t_uint32 p_subsong, file_info& p_info, abort_callback& p_abort)
    {
        if (!m_player) return;

        unsigned long durationMs = m_player->songlength(p_subsong);
        if (durationMs > 0)
            p_info.set_length(static_cast<double>(durationMs) / 1000.0);

        p_info.info_set_int("samplerate", m_sampleRate);
        p_info.info_set_int("channels", kChannels);
        p_info.info_set_int("bitspersample", kBitsPerSample);
        p_info.info_set("encoding", "synthesized");

        std::string type = m_player->gettype();
        if (!type.empty())
            p_info.info_set("codec", type.c_str());
        else
            p_info.info_set("codec", "AdPlug / OPL");

        if (!type.empty())
            p_info.info_set("adlib_format", type.c_str());

        std::string title = m_player->gettitle();
        if (!title.empty())
            p_info.meta_set("title", title.c_str());

        std::string author = m_player->getauthor();
        if (!author.empty())
            p_info.meta_set("artist", author.c_str());

        std::string desc = m_player->getdesc();
        if (!desc.empty())
            p_info.meta_set("comment", desc.c_str());
    }

    t_filestats2 get_stats2(unsigned f, abort_callback& a)
    {
        return m_file->get_stats2_(f, a);
    }

    void decode_initialize(t_uint32 p_subsong, unsigned p_flags, abort_callback& p_abort)
    {
        m_currentSubsong = p_subsong;
        m_player->rewind(p_subsong);
        m_remainingSamples = 0;
        m_hasEnded = false;
        m_isStuck = 0;
        m_samplesPlayed = 0;

        unsigned long durationMs = m_player->songlength(p_subsong);
        m_durationSamples = (durationMs > 0)
            ? static_cast<uint64_t>(durationMs) * m_sampleRate / 1000
            : 0;

        const std::string path = m_path;
        const t_uint32 subsong = p_subsong;
        fb2k::inMainThread([path, subsong]()
        {
            metadb_handle_ptr handle = metadb::get()->handle_create(path.c_str(), subsong);
            metadb_handle_list items;
            items.add_item(handle);
            metadb_io_v2::get()->load_info_async(
                items,
                metadb_io::load_info_force,
                core_api::get_main_window(),
                metadb_io_v2::op_flag_silent | metadb_io_v2::op_flag_no_errors,
                nullptr);
        });
    }

    bool decode_run(audio_chunk& p_chunk, abort_callback& p_abort)
    {
        enum { kBlockSize = 1024 };

        if (m_hasEnded)
        {
            m_hasEnded = false;
            return false;
        }

        if (m_durationSamples > 0 && m_samplesPlayed >= m_durationSamples)
            return false;

        int16_t buffer[kBlockSize * kChannels];
        unsigned totalSamples = kBlockSize;

        if (m_durationSamples > 0)
        {
            uint64_t remaining = m_durationSamples - m_samplesPlayed;
            if (remaining < totalSamples)
                totalSamples = static_cast<unsigned>(remaining);
            if (totalSamples == 0)
                return false;
        }

        unsigned produced = 0;

        while (produced < totalSamples)
        {
            unsigned needed = totalSamples - produced;

            if (m_remainingSamples > 0)
            {
                unsigned toRender = (needed < m_remainingSamples) ? needed : m_remainingSamples;
                m_core->opl->update(buffer + produced * kChannels, toRender);
                m_remainingSamples -= toRender;
                produced += toRender;
            }
            else if (m_player->update())
            {
                float refresh = m_player->getrefresh();
                if (refresh > 0)
                    m_remainingSamples = static_cast<unsigned>(m_sampleRate / refresh);
                else
                    m_remainingSamples = m_sampleRate / 70;
                m_isStuck = 0;
            }
            else if (m_isStuck == 0)
            {
                m_isStuck = 1;
                m_player->rewind(m_currentSubsong);
            }
            else
            {
                m_hasEnded = true;
                break;
            }
        }

        if (produced == 0) return false;

        m_samplesPlayed += produced;

        p_chunk.set_data_fixedpoint(
            buffer,
            produced * kChannels * (kBitsPerSample / 8),
            m_sampleRate,
            kChannels,
            kBitsPerSample,
            audio_chunk::g_guess_channel_config(kChannels)
        );

        return true;
    }

    void decode_seek(double p_seconds, abort_callback& p_abort)
    {
        m_player->rewind(m_currentSubsong);
        m_remainingSamples = 0;
        m_isStuck = 0;
        m_hasEnded = false;

        unsigned targetMs = static_cast<unsigned>(p_seconds * 1000.0);
        m_samplesPlayed = static_cast<uint64_t>(targetMs) * m_sampleRate / 1000;

        unsigned posMs = 0;

        while (posMs < targetMs)
        {
            if (!m_player->update()) break;
            float refresh = m_player->getrefresh();
            if (refresh > 0)
                posMs += static_cast<unsigned>(1000.0 / refresh);
            else
                break;
        }

        float refresh = m_player->getrefresh();
        if (refresh > 0)
            m_remainingSamples = static_cast<unsigned>(m_sampleRate / refresh);
    }

    bool decode_can_seek() { return true; }

    void retag_set_info(t_uint32 p_subsong, const file_info& p_info, abort_callback& p_abort)
    {
        throw exception_tagging_unsupported();
    }

    void retag_commit(abort_callback& p_abort)
    {
        throw exception_tagging_unsupported();
    }

    void remove_tags(abort_callback& p_abort)
    {
        throw exception_tagging_unsupported();
    }

    static bool g_is_our_content_type(const char* p_content_type) { return false; }

    static bool g_is_our_path(const char* p_path, const char* p_extension)
    {
        return is_adlib_extension(p_extension);
    }

    static const char* g_get_name() { return "AdPlug OPL decoder"; }

    static GUID g_get_guid()
    {
        static const GUID guid = { 0xa7f3c8b1, 0x2d4e, 0x4f6a, { 0x9b, 0x0c, 0x1e, 0x2d, 0x3f, 0x4a, 0x5b, 0x6c } };
        return guid;
    }

private:
    service_ptr_t<file> m_file;
    std::string m_path;
    std::string m_nativePath;
    std::vector<uint8_t> m_fileData;

    CPlayer* m_player = nullptr;
    OplCore* m_core = nullptr;

    unsigned m_sampleRate = 49716;
    unsigned m_remainingSamples = 0;
    bool m_hasEnded = false;
    unsigned m_isStuck = 0;
    t_uint32 m_currentSubsong = 0;
    uint64_t m_samplesPlayed = 0;
    uint64_t m_durationSamples = 0;
};

static input_factory_t<input_adlib> g_input_adlib_factory;

DECLARE_FILE_TYPE("AdLib music files",
    "*.HSC;*.SNG;*.A2M;*.A2T;*.AMD;*.BAM;*.CMF;*.D00;*.DFM;*.HSP;*.KSM;*.MAD;*.MKJ;*.CFF;*.DMO;*.S3M;"
    "*.DTM;*.MTK;*.MTR;*.RAD;*.RAW;*.SAT;*.SA2;*.XAD;*.LDS;*.M;*.ROL;*.XSM;*.DRO;*.IMF;*.WLF;*.ADLIB;"
    "*.MUS;*.MDY;*.IMS;*.MDI;*.MID;*.SCI;*.LAA;*.ADL;*.BMF;*.PIS;*.MSC;*.RIX;*.MKF;*.JBM;*.GOT;*.SOP;"
    "*.VGM;*.VGZ;*.HSQ;*.SQX;*.SDB;*.AGD;*.HA2;*.XMS;*.PLX"
);
