#pragma once
#include "guids.h"

class subsong_database {
public:
    static subsong_database& get();

    void load();
    void save();

    bool has_exclusions(const char* path) const;
    std::set<uint32_t> get_excluded(const char* path) const;
    void set_excluded(const char* path, const std::set<uint32_t>& excluded);
    void clear_exclusions(const char* path);

    std::vector<uint32_t> get_enabled_subsongs(
        const char* path,
        const std::vector<uint32_t>& all_subsongs) const;

    size_t size() const;

private:
    subsong_database() = default;
    mutable pfc::readWriteLock m_lock;
    std::unordered_map<std::string, std::set<uint32_t>> m_exclusions;
    std::once_flag m_load_flag;

    void ensure_loaded();
    void load_internal();
    static std::string path_key(const char* path);
};
