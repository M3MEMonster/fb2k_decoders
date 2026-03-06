#include "stdafx.h"
#include "duration_db.h"
#include "guids.h"

class context_add_to_duration_mgr : public contextmenu_item_simple {
public:
    unsigned get_num_items() override { return 1; }

    GUID get_item_guid(unsigned) override { return guid_contextmenu_add_duration; }

    void get_item_name(unsigned, pfc::string_base& out) override {
        out = "Add to Duration Manager";
    }

    GUID get_parent() override { return contextmenu_groups::root; }

    bool get_item_description(unsigned, pfc::string_base& out) override {
        out = "Add selected tracks to the Duration Manager database";
        return true;
    }

    void context_command(unsigned, metadb_handle_list_cref items, const GUID&) override {
        auto& db = duration_database::get();

        for (size_t i = 0; i < items.get_count(); i++) {
            auto handle = items[i];
            if (!db.lookup_by_location(handle->get_path(), handle->get_subsong_index())) {
                db.add(handle);
            }
        }

        db.save();
    }
};

static contextmenu_item_factory_t<context_add_to_duration_mgr> g_context_factory;
