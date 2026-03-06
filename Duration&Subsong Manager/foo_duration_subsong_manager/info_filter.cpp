#include "stdafx.h"
#include "duration_db.h"
#include "guids.h"

class duration_info_filter : public input_info_filter {
public:
    void filter_info_read(const playable_location& loc, file_info& info, abort_callback&) override {
        auto rec = duration_database::get().lookup_by_location(loc.get_path(), loc.get_subsong());
        if (rec && rec->custom_duration > 0) {
            info.set_length(rec->custom_duration);
        }
    }

    bool filter_info_write(const playable_location&, file_info&, abort_callback&) override {
        return true;
    }

    void on_info_remove(const char*, abort_callback&) override {}

    GUID get_guid() override { return guid_info_filter; }

    GUID get_preferences_guid() override { return guid_prefs_duration; }

    const char* get_name() override { return "Duration Manager"; }

    bool supports_fallback() override { return false; }

    bool write_fallback(const playable_location&, const file_info&, abort_callback&) override {
        return false;
    }

    void remove_tags_fallback(const char*, abort_callback&) override {}
};

static service_factory_single_t<duration_info_filter> g_duration_info_filter;
