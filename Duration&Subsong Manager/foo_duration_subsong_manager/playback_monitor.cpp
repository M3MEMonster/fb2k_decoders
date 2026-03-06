#include "stdafx.h"
#include "duration_db.h"

class duration_playback_monitor : public play_callback_static {
public:
    unsigned get_flags() override {
        return flag_on_playback_time | flag_on_playback_new_track | flag_on_playback_stop;
    }

    void on_playback_new_track(metadb_handle_ptr p_track) override {
        auto rec = duration_database::get().lookup_by_location(
            p_track->get_path(), p_track->get_subsong_index());
        if (rec && rec->custom_duration > 0) {
            m_stop_at = rec->custom_duration;
        } else {
            m_stop_at = -1;
        }
    }

    void on_playback_time(double p_time) override {
        if (m_stop_at > 0 && p_time >= m_stop_at) {
            m_stop_at = -1;
            fb2k::inMainThread([]{
                playback_control::get()->start(playback_control::track_command_next);
            });
        }
    }

    void on_playback_stop(play_control::t_stop_reason reason) override {
        m_stop_at = -1;
    }

    void on_playback_starting(play_control::t_track_command, bool) override {}
    void on_playback_seek(double) override {}
    void on_playback_pause(bool) override {}
    void on_playback_edited(metadb_handle_ptr) override {}
    void on_playback_dynamic_info(const file_info&) override {}
    void on_playback_dynamic_info_track(const file_info&) override {}
    void on_volume_change(float) override {}

private:
    double m_stop_at = -1;
};

static play_callback_static_factory_t<duration_playback_monitor> g_playback_monitor;
