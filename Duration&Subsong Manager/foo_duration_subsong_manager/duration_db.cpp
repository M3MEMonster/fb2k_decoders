#include "stdafx.h"
#include "duration_db.h"

static const char* CONFIGSTORE_KEY = "dsmgr.duration_db";

duration_database& duration_database::get() {
    static duration_database instance;
    return instance;
}

pfc::string8 duration_database::compute_hash(const char* path, uint32_t subsong) {
    // Primary key is content hash + subsong so moved files can be matched.
    pfc::string8 contentHash;
    bool contentOk = false;
    try {
        service_ptr_t<file> f;
        abort_callback_dummy abort;
        filesystem::g_open_read(f, path, abort);

        auto hasher = hasher_md5::get();
        hasher_md5_state state;
        hasher->initialize(state);

        const size_t kChunk = 1 << 20;
        std::vector<uint8_t> buf(kChunk);
        for (;;) {
            t_size done = f->read(buf.data(), (t_size)buf.size(), abort);
            if (done == 0) break;
            hasher->process(state, buf.data(), done);
        }
        contentHash = hasher->get_result(state).asString();
        contentOk = true;
    } catch (...) {
        contentOk = false;
    }

    auto hasher = hasher_md5::get();
    hasher_md5_state finalState;
    hasher->initialize(finalState);
    if (contentOk) {
        hasher->process(finalState, contentHash.c_str(), contentHash.length());
    } else {
        // Fallback for inaccessible files.
        hasher->process(finalState, path, strlen(path));
    }
    hasher->process(finalState, "\0", 1);
    hasher->process(finalState, &subsong, sizeof(subsong));
    return hasher->get_result(finalState).asString();
}

pfc::string8 duration_database::make_location_key(const char* path, uint32_t subsong) {
    pfc::string8 key;
    key << path << "|" << pfc::format_uint(subsong);
    return key;
}

void duration_database::ensure_loaded() {
    std::call_once(m_load_flag, [this]() {
        load_internal();
    });
}

void duration_database::load_internal() {
    PFC_INSYNC_WRITE(m_lock);
    m_records.clear();
    m_location_to_hash.clear();

    try {
        auto api = fb2k::configStore::get();
        auto blob = api->getConfigBlob(CONFIGSTORE_KEY, fb2k::memBlock::empty());
        if (!blob.is_valid() || blob->size() == 0) return;

        stream_reader_formatter_simple_ref<> reader(blob->data(), blob->size());
        uint32_t count;
        reader >> count;

        for (uint32_t i = 0; i < count; i++) {
            duration_record rec;
            pfc::string8 hash, file_name, song_name, full_path;
            uint32_t subsong_index;
            double dur;

            reader >> hash >> file_name >> song_name >> subsong_index >> dur >> full_path;

            rec.hash = hash;
            rec.file_name = file_name;
            rec.song_name = song_name;
            rec.subsong_index = subsong_index;
            rec.custom_duration = dur;
            rec.full_path = full_path;
            rec.hash = compute_hash(rec.full_path.c_str(), rec.subsong_index);

            if (rec.song_name.is_empty() && !rec.file_name.is_empty()) {
                const char* dot = strrchr(rec.file_name.c_str(), '.');
                if (dot) rec.song_name.set_string(rec.file_name.c_str(), dot - rec.file_name.c_str());
                else rec.song_name = rec.file_name;
            }

            std::string key(rec.hash.c_str());
            pfc::string8 loc_key = make_location_key(rec.full_path.c_str(), rec.subsong_index);
            m_location_to_hash[std::string(loc_key.c_str())] = key;
            m_records[key] = std::move(rec);
        }
    } catch (...) {
    }
}

void duration_database::load() {
    load_internal();
}

void duration_database::save() {
    PFC_INSYNC_READ(m_lock);

    try {
        stream_writer_formatter_simple<> writer;
        writer << (uint32_t)m_records.size();

        for (auto& [key, rec] : m_records) {
            writer << rec.hash << rec.file_name << rec.song_name
                   << rec.subsong_index << rec.custom_duration << rec.full_path;
        }

        auto api = fb2k::configStore::get();
        api->setConfigBlob(CONFIGSTORE_KEY, writer.m_buffer.get_ptr(), writer.m_buffer.get_size());
    } catch (...) {
    }
}

void duration_database::add(const metadb_handle_ptr& handle) {
    pfc::string8 path = handle->get_path();
    uint32_t subsong = handle->get_subsong_index();
    pfc::string8 hash = compute_hash(path.c_str(), subsong);

    pfc::string8 file_name;
    {
        const char* p = path.c_str();
        const char* last_slash = strrchr(p, '\\');
        const char* last_fslash = strrchr(p, '/');
        const char* sep = last_slash > last_fslash ? last_slash : last_fslash;
        file_name = sep ? (sep + 1) : p;
    }

    pfc::string8 song_name;
    double duration = 0;
    {
        auto info_ref = handle->get_info_ref();
        if (info_ref.is_valid()) {
            const char* title = info_ref->info().meta_get("title", 0);
            if (title) song_name = title;
            duration = info_ref->info().get_length();
        }
        if (song_name.is_empty()) {
            pfc::string8 tmp(file_name);
            const char* dot = strrchr(tmp.c_str(), '.');
            if (dot) song_name.set_string(tmp.c_str(), dot - tmp.c_str());
            else song_name = tmp;
        }
    }

    ensure_loaded();
    PFC_INSYNC_WRITE(m_lock);

    std::string skey(hash.c_str());
    if (m_records.count(skey)) return;

    duration_record rec;
    rec.hash = hash;
    rec.file_name = file_name;
    rec.song_name = song_name;
    rec.subsong_index = subsong;
    rec.custom_duration = duration;
    rec.full_path = path;

    pfc::string8 loc_key = make_location_key(path.c_str(), subsong);
    m_location_to_hash[std::string(loc_key.c_str())] = skey;
    m_records[skey] = std::move(rec);
}

void duration_database::remove(const char* hash) {
    ensure_loaded();
    PFC_INSYNC_WRITE(m_lock);

    auto it = m_records.find(std::string(hash));
    if (it != m_records.end()) {
        pfc::string8 loc_key = make_location_key(it->second.full_path.c_str(), it->second.subsong_index);
        m_location_to_hash.erase(std::string(loc_key.c_str()));
        m_records.erase(it);
    }
}

void duration_database::remove_multiple(const std::vector<std::string>& hashes) {
    ensure_loaded();
    PFC_INSYNC_WRITE(m_lock);

    for (auto& hash : hashes) {
        auto it = m_records.find(hash);
        if (it != m_records.end()) {
            pfc::string8 loc_key = make_location_key(it->second.full_path.c_str(), it->second.subsong_index);
            m_location_to_hash.erase(std::string(loc_key.c_str()));
            m_records.erase(it);
        }
    }
}

void duration_database::update_duration(const char* hash, double new_duration) {
    ensure_loaded();
    PFC_INSYNC_WRITE(m_lock);

    auto it = m_records.find(std::string(hash));
    if (it != m_records.end()) {
        it->second.custom_duration = new_duration;
    }
}

const duration_record* duration_database::lookup_by_hash(const char* hash) const {
    const_cast<duration_database*>(this)->ensure_loaded();
    PFC_INSYNC_READ(m_lock);

    auto it = m_records.find(std::string(hash));
    return (it != m_records.end()) ? &it->second : nullptr;
}

const duration_record* duration_database::lookup_by_location(const char* path, uint32_t subsong) const {
    auto* db = const_cast<duration_database*>(this);
    db->ensure_loaded();

    pfc::string8 loc_key = make_location_key(path, subsong);
    {
        PFC_INSYNC_READ(db->m_lock);
        auto it = db->m_location_to_hash.find(std::string(loc_key.c_str()));
        if (it != db->m_location_to_hash.end()) {
            auto rec_it = db->m_records.find(it->second);
            if (rec_it != db->m_records.end()) return &rec_it->second;
        }
    }

    // Moved/renamed file fallback: resolve by content hash + subsong and cache new location.
    pfc::string8 content_key = compute_hash(path, subsong);
    std::string skey(content_key.c_str());
    PFC_INSYNC_WRITE(db->m_lock);
    auto rec_it = db->m_records.find(skey);
    if (rec_it == db->m_records.end()) return nullptr;

    rec_it->second.full_path = path;
    db->m_location_to_hash[std::string(loc_key.c_str())] = skey;
    return &rec_it->second;
}

std::vector<const duration_record*> duration_database::get_all() const {
    const_cast<duration_database*>(this)->ensure_loaded();
    PFC_INSYNC_READ(m_lock);

    std::vector<const duration_record*> result;
    result.reserve(m_records.size());
    for (auto& [key, rec] : m_records) {
        result.push_back(&rec);
    }
    return result;
}

std::vector<const duration_record*> duration_database::search(const char* query) const {
    const_cast<duration_database*>(this)->ensure_loaded();
    PFC_INSYNC_READ(m_lock);

    std::vector<const duration_record*> result;

    if (!query || !*query) {
        result.reserve(m_records.size());
        for (auto& [key, rec] : m_records) {
            result.push_back(&rec);
        }
        return result;
    }

    pfc::string8 lquery;
    lquery.convert_to_lower_ascii(query);

    for (auto& [key, rec] : m_records) {
        pfc::string8 lname;
        lname.convert_to_lower_ascii(rec.file_name.c_str());
        pfc::string8 lsong;
        lsong.convert_to_lower_ascii(rec.song_name.c_str());

        if (strstr(lname.c_str(), lquery.c_str()) || strstr(lsong.c_str(), lquery.c_str())) {
            result.push_back(&rec);
        }
    }
    return result;
}

bool duration_database::has_entries() const {
    const_cast<duration_database*>(this)->ensure_loaded();
    PFC_INSYNC_READ(m_lock);
    return !m_records.empty();
}

size_t duration_database::size() const {
    const_cast<duration_database*>(this)->ensure_loaded();
    PFC_INSYNC_READ(m_lock);
    return m_records.size();
}
