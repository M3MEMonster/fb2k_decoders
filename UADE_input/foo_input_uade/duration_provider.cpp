#include "stdafx.h"
#include "uade_duration_api.h"
#include "../../AdPlug/Utils/duration_database_provider.h"

DURATION_DATABASE_PROVIDER_DEFINE_GUID();

#include <cstdio>

namespace {

class uade_duration_provider : public duration_database_provider {
public:
    void get_provider_name(pfc::string_base & out) override {
        out = "UADE";
    }

    t_size get_entry_count() override {
        return m_cache_valid ? m_cache.size() : refresh_cache();
    }

    void get_entry(t_size index, duration_db_entry & out) override {
        ensure_cache();
        if (index >= m_cache.size()) return;
        fill_entry(m_cache[index], out);
    }

    void find_entries_by_hash(const char * hash, pfc::list_base_t<duration_db_entry> & out) override {
        ensure_cache();
        for (auto& e : m_cache) {
            char hex[32];
            _snprintf_s(hex, sizeof(hex), _TRUNCATE, "%016llx",
                        (unsigned long long)e.hash);
            if (strcmp(hex, hash) == 0) {
                duration_db_entry dbe;
                fill_entry(e, dbe);
                out.add_item(dbe);
            }
        }
    }

    void find_entries_by_name(const char * name_fragment, pfc::list_base_t<duration_db_entry> & out) override {
        ensure_cache();
        std::string frag(name_fragment);
        for (auto& c : frag) c = static_cast<char>(tolower(c));

        for (auto& e : m_cache) {
            std::string name_lc = e.name;
            for (auto& c : name_lc) c = static_cast<char>(tolower(c));
            if (name_lc.find(frag) != std::string::npos) {
                duration_db_entry dbe;
                fill_entry(e, dbe);
                out.add_item(dbe);
            }
        }
    }

private:
    void fill_entry(const uade_flat_entry& src, duration_db_entry& dst) {
        char hex[32];
        _snprintf_s(hex, sizeof(hex), _TRUNCATE, "%016llx",
                    (unsigned long long)src.hash);
        dst.hash = hex;
        dst.name = src.name.c_str();
        dst.subsong = src.subsong;
        dst.duration_sec = src.duration;
        dst.provider_name = "UADE";
    }

    void ensure_cache() {
        if (!m_cache_valid) refresh_cache();
    }

    t_size refresh_cache() {
        m_cache = uade_get_all_duration_entries();
        m_cache_valid = true;
        return m_cache.size();
    }

    std::vector<uade_flat_entry> m_cache;
    bool m_cache_valid = false;
};

static service_factory_single_t<uade_duration_provider> g_uade_provider;

} // anonymous namespace
