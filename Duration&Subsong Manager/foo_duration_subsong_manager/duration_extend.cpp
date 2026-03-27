#include "stdafx.h"
#include "duration_db.h"
#include "guids.h"

namespace {

class duration_extend_decoder : public input_decoder {
public:
    duration_extend_decoder(input_decoder::ptr real, const char* path)
        : m_real(real), m_path(path) {}

    // input_info_reader methods - delegate to real decoder

    t_uint32 get_subsong_count() override {
        return m_real->get_subsong_count();
    }

    t_uint32 get_subsong(t_uint32 p_index) override {
        return m_real->get_subsong(p_index);
    }

    void get_info(t_uint32 p_subsong, file_info& p_info, abort_callback& p_abort) override {
        m_real->get_info(p_subsong, p_info, p_abort);
    }

    t_filestats get_file_stats(abort_callback& p_abort) override {
        return m_real->get_file_stats(p_abort);
    }

    // input_decoder methods

    void initialize(t_uint32 p_subsong, unsigned p_flags, abort_callback& p_abort) override {
        m_target_duration = -1;
        m_elapsed_samples = 0;
        m_srate = 0;

        // Look up custom duration for this file+subsong
        auto rec = duration_database::get().lookup_by_location(m_path.c_str(), p_subsong);
        if (rec && rec->custom_duration > 0) {
            m_target_duration = rec->custom_duration;
            // Always strip no_looping for Duration Manager tracks so decoders
            // that honor this flag don't stop early due to loop policy.
            p_flags &= ~(unsigned)input_flag_no_looping;
        }

        m_real->initialize(p_subsong, p_flags, p_abort);
    }

    bool run(audio_chunk& p_chunk, abort_callback& p_abort) override {
        if (m_target_duration <= 0) {
            // No extension needed, pass through
            return m_real->run(p_chunk, p_abort);
        }

        // Check if we've already reached target duration
        if (m_srate > 0) {
            double elapsed = (double)m_elapsed_samples / (double)m_srate;
            if (elapsed >= m_target_duration) {
                return false;
            }
        }

        const auto process_chunk = [this](audio_chunk& chunk) -> bool {
            // Remember audio format for elapsed-time accounting.
            m_srate = chunk.get_sample_rate();
            m_elapsed_samples += chunk.get_sample_count();

            // Trim chunk if it exceeds target duration.
            double elapsed = (double)m_elapsed_samples / (double)m_srate;
            if (elapsed > m_target_duration) {
                double excess = elapsed - m_target_duration;
                t_size excess_samples = (t_size)(excess * m_srate);
                t_size current = chunk.get_sample_count();
                if (excess_samples > 0 && excess_samples < current) {
                    chunk.set_sample_count(current - excess_samples);
                    m_elapsed_samples -= excess_samples;
                } else if (excess_samples >= current) {
                    m_elapsed_samples -= current;
                    chunk.set_sample_count(0);
                }
            }

            if (chunk.get_sample_count() == 0) {
                return false;
            }
            return true;
        };

        if (m_real->run(p_chunk, p_abort)) {
            return process_chunk(p_chunk);
        }

        // Soft-EOF recovery: some decoders (notably UADE) may return a transient
        // EOF due to an internal song-end flag, then continue output on next run().
        // Retry a few times before treating EOF as terminal.
        constexpr unsigned kSoftEofRetries = 3;
        for (unsigned i = 0; i < kSoftEofRetries; ++i) {
            m_real->on_idle(p_abort);
            if (m_real->run(p_chunk, p_abort)) {
                return process_chunk(p_chunk);
            }
        }

        // Terminal EOF.
        return false;
    }

    void seek(double p_seconds, abort_callback& p_abort) override {
        if (m_srate > 0) {
            m_elapsed_samples = (t_uint64)(p_seconds * m_srate);
        } else {
            m_elapsed_samples = 0;
        }
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

private:
    input_decoder::ptr m_real;
    pfc::string8 m_path;

    double m_target_duration = -1;
    t_uint64 m_elapsed_samples = 0;
    unsigned m_srate = 0;
};

class duration_extend_entry : public input_entry {
public:
    bool is_our_content_type(const char*) override { return false; }

    bool is_our_path(const char* p_full_path, const char*) override {
        return duration_database::get().has_path(p_full_path);
    }

    void open_for_decoding(service_ptr_t<input_decoder>& p_instance,
                           service_ptr_t<file> p_filehint,
                           const char* p_path,
                           abort_callback& p_abort) override {
        input_decoder::ptr real;
        input_entry::g_open_for_decoding(real, p_filehint, p_path, p_abort, true);
        p_instance = new service_impl_t<duration_extend_decoder>(real, p_path);
    }

    void open_for_info_read(service_ptr_t<input_info_reader>& p_instance,
                            service_ptr_t<file> p_filehint,
                            const char* p_path,
                            abort_callback& p_abort) override {
        input_entry::g_open_for_info_read(p_instance, p_filehint, p_path, p_abort, true);
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

static service_factory_single_t<duration_extend_entry> g_duration_extend;

} // namespace
