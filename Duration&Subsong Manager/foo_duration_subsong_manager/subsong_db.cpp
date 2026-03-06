#include "stdafx.h"
#include "subsong_db.h"

static const char* CONFIGSTORE_KEY = "dsmgr.subsong_db";

subsong_database& subsong_database::get() {
    static subsong_database instance;
    return instance;
}

std::string subsong_database::path_key(const char* path) {
    pfc::string8 lower;
    lower.convert_to_lower_ascii(path);
    return std::string(lower.c_str());
}

void subsong_database::ensure_loaded() {
    std::call_once(m_load_flag, [this]() {
        load_internal();
    });
}

void subsong_database::load_internal() {
    PFC_INSYNC_WRITE(m_lock);
    m_exclusions.clear();

    try {
        auto api = fb2k::configStore::get();
        auto blob = api->getConfigBlob(CONFIGSTORE_KEY, fb2k::memBlock::empty());
        if (!blob.is_valid() || blob->size() == 0) return;

        stream_reader_formatter_simple_ref<> reader(blob->data(), blob->size());
        uint32_t path_count;
        reader >> path_count;

        for (uint32_t i = 0; i < path_count; i++) {
            pfc::string8 path;
            uint32_t excl_count;
            reader >> path >> excl_count;

            std::set<uint32_t> excluded;
            for (uint32_t j = 0; j < excl_count; j++) {
                uint32_t id;
                reader >> id;
                excluded.insert(id);
            }

            if (!excluded.empty()) {
                m_exclusions[path_key(path.c_str())] = std::move(excluded);
            }
        }
    } catch (...) {
    }
}

void subsong_database::load() {
    load_internal();
}

void subsong_database::save() {
    PFC_INSYNC_READ(m_lock);

    try {
        stream_writer_formatter_simple<> writer;
        writer << (uint32_t)m_exclusions.size();

        for (auto& [key, excluded] : m_exclusions) {
            pfc::string8 path(key.c_str());
            writer << path << (uint32_t)excluded.size();
            for (uint32_t id : excluded) {
                writer << id;
            }
        }

        auto api = fb2k::configStore::get();
        api->setConfigBlob(CONFIGSTORE_KEY, writer.m_buffer.get_ptr(), writer.m_buffer.get_size());
    } catch (...) {
    }
}

bool subsong_database::has_exclusions(const char* path) const {
    const_cast<subsong_database*>(this)->ensure_loaded();
    PFC_INSYNC_READ(m_lock);

    auto it = m_exclusions.find(path_key(path));
    return it != m_exclusions.end() && !it->second.empty();
}

std::set<uint32_t> subsong_database::get_excluded(const char* path) const {
    const_cast<subsong_database*>(this)->ensure_loaded();
    PFC_INSYNC_READ(m_lock);

    auto it = m_exclusions.find(path_key(path));
    if (it != m_exclusions.end()) return it->second;
    return {};
}

void subsong_database::set_excluded(const char* path, const std::set<uint32_t>& excluded) {
    ensure_loaded();
    PFC_INSYNC_WRITE(m_lock);

    if (excluded.empty()) {
        m_exclusions.erase(path_key(path));
    } else {
        m_exclusions[path_key(path)] = excluded;
    }
}

void subsong_database::clear_exclusions(const char* path) {
    ensure_loaded();
    PFC_INSYNC_WRITE(m_lock);

    m_exclusions.erase(path_key(path));
}

std::vector<uint32_t> subsong_database::get_enabled_subsongs(
    const char* path,
    const std::vector<uint32_t>& all_subsongs) const
{
    auto excluded = get_excluded(path);
    if (excluded.empty()) return all_subsongs;

    std::vector<uint32_t> enabled;
    for (uint32_t id : all_subsongs) {
        if (excluded.find(id) == excluded.end()) {
            enabled.push_back(id);
        }
    }
    return enabled;
}

size_t subsong_database::size() const {
    const_cast<subsong_database*>(this)->ensure_loaded();
    PFC_INSYNC_READ(m_lock);
    return m_exclusions.size();
}
