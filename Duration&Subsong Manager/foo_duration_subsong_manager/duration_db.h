#pragma once
#include "guids.h"

struct duration_record {
    pfc::string8 hash;
    pfc::string8 file_name;
    pfc::string8 song_name;
    uint32_t subsong_index = 0;
    double custom_duration = 0;
    pfc::string8 full_path;
};

class duration_database {
public:
    static duration_database& get();

    void load();
    void save();

    void add(const metadb_handle_ptr& handle);
    void remove(const char* hash);
    void remove_multiple(const std::vector<std::string>& hashes);
    void update_duration(const char* hash, double new_duration);

    const duration_record* lookup_by_hash(const char* hash) const;
    const duration_record* lookup_by_location(const char* path, uint32_t subsong) const;

    std::vector<const duration_record*> get_all() const;
    std::vector<const duration_record*> search(const char* query) const;

    static pfc::string8 compute_hash(const char* path, uint32_t subsong);
    static pfc::string8 make_location_key(const char* path, uint32_t subsong);

    bool has_entries() const;
    bool has_path(const char* path) const;
    size_t size() const;

private:
    duration_database() = default;
    mutable pfc::readWriteLock m_lock;
    std::unordered_map<std::string, duration_record> m_records;
    std::unordered_map<std::string, std::string> m_location_to_hash;
    std::once_flag m_load_flag;

    void ensure_loaded();
    void load_internal();
};
