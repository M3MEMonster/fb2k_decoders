#include "stdafx.h"
#include "subsong_db.h"
#include "guids.h"

namespace {

template<typename TBase>
class subsong_filter_wrapper : public TBase {
public:
    subsong_filter_wrapper(service_ptr_t<TBase> real, const char* path)
        : m_real(real)
    {
        auto& db = subsong_database::get();

        t_uint32 total = m_real->get_subsong_count();
        std::vector<uint32_t> all;
        all.reserve(total);
        for (t_uint32 i = 0; i < total; i++) {
            all.push_back(m_real->get_subsong(i));
        }

        m_enabled = db.get_enabled_subsongs(path, all);

        if (m_enabled.empty()) {
            m_enabled = all;
        }
    }

    t_uint32 get_subsong_count() override {
        return (t_uint32)m_enabled.size();
    }

    t_uint32 get_subsong(t_uint32 p_index) override {
        if (p_index < m_enabled.size()) return m_enabled[p_index];
        return 0;
    }

    void get_info(t_uint32 p_subsong, file_info& p_info, abort_callback& p_abort) override {
        m_real->get_info(p_subsong, p_info, p_abort);
    }

    t_filestats get_file_stats(abort_callback& p_abort) override {
        return m_real->get_file_stats(p_abort);
    }

protected:
    service_ptr_t<TBase> m_real;
    std::vector<uint32_t> m_enabled;
};

class subsong_filter_decoder : public subsong_filter_wrapper<input_decoder> {
public:
    using subsong_filter_wrapper::subsong_filter_wrapper;

    void initialize(t_uint32 p_subsong, unsigned p_flags, abort_callback& p_abort) override {
        m_real->initialize(p_subsong, p_flags, p_abort);
    }

    bool run(audio_chunk& p_chunk, abort_callback& p_abort) override {
        return m_real->run(p_chunk, p_abort);
    }

    void seek(double p_seconds, abort_callback& p_abort) override {
        m_real->seek(p_seconds, p_abort);
    }

    bool can_seek() override {
        return m_real->can_seek();
    }

    bool get_dynamic_info(file_info& p_out, double& p_timestamp_delta) override {
        return m_real->get_dynamic_info(p_out, p_timestamp_delta);
    }

    bool get_dynamic_info_track(file_info& p_out, double& p_timestamp_delta) override {
        return m_real->get_dynamic_info_track(p_out, p_timestamp_delta);
    }

    void on_idle(abort_callback& p_abort) override {
        m_real->on_idle(p_abort);
    }
};

class subsong_filter_info_reader : public subsong_filter_wrapper<input_info_reader> {
public:
    using subsong_filter_wrapper::subsong_filter_wrapper;
};

class subsong_redirect_entry : public input_entry {
public:
    bool is_our_content_type(const char*) override { return false; }

    bool is_our_path(const char* p_full_path, const char*) override {
        return subsong_database::get().has_exclusions(p_full_path);
    }

    void open_for_decoding(service_ptr_t<input_decoder>& p_instance,
                           service_ptr_t<file> p_filehint,
                           const char* p_path,
                           abort_callback& p_abort) override {
        input_decoder::ptr real;
        input_entry::g_open_for_decoding(real, p_filehint, p_path, p_abort, true);
        p_instance = new service_impl_t<subsong_filter_decoder>(real, p_path);
    }

    void open_for_info_read(service_ptr_t<input_info_reader>& p_instance,
                            service_ptr_t<file> p_filehint,
                            const char* p_path,
                            abort_callback& p_abort) override {
        input_info_reader::ptr real;
        input_entry::g_open_for_info_read(real, p_filehint, p_path, p_abort, true);
        p_instance = new service_impl_t<subsong_filter_info_reader>(real, p_path);
    }

    void open_for_info_write(service_ptr_t<input_info_writer>& p_instance,
                             service_ptr_t<file> p_filehint,
                             const char* p_path,
                             abort_callback& p_abort) override {
        input_entry::g_open_for_info_write(p_instance, p_filehint, p_path, p_abort, true);
    }

    void get_extended_data(service_ptr_t<file>, const playable_location&,
                           const GUID&, mem_block_container&, abort_callback&) override {}

    unsigned get_flags() override { return flag_redirect; }
};

static service_factory_single_t<subsong_redirect_entry> g_subsong_redirect;

} // namespace
