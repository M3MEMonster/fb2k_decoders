#include "stdafx.h"

#include <IO/Stream.h>          // our stub, also provides MemoryStream
#include "OrganyaDecoder.h"     // Organya::Song, Load, Render, etc.

#include <mutex>
#include <cstring>

namespace {

const unsigned ORG_SAMPLE_RATE = 44100;
const unsigned ORG_CHANNELS = 2;
const unsigned ORG_DEFAULT_LOOP_COUNT = 2;
const unsigned ORG_DECODE_CHUNK_SIZE = 1024;

// One-time initialization of the Organya drum/wave tables.
static std::once_flag s_init_flag;
static void ensure_initialized() {
	std::call_once(s_init_flag, []() {
		Organya::initialize();
	});
}

// ---------------------------------------------------------------------------
// foobar2000 input class
// ---------------------------------------------------------------------------

class input_org : public input_stubs {
public:
	input_org() = default;

	~input_org() {
		if (m_song) Organya::Unload(m_song);
	}

	void open(service_ptr_t<file> p_filehint, const char * p_path, t_input_open_reason p_reason, abort_callback & p_abort) {
		if (p_reason == input_open_info_write) throw exception_tagging_unsupported();

		m_file = p_filehint;
		input_open_file_helper(m_file, p_path, p_reason, p_abort);

		// Read the entire .org file into memory
		t_filesize size = m_file->get_size(p_abort);
		if (size == filesize_invalid || size == 0)
			throw exception_io_data("Invalid Organya file");

		m_file->seek(0, p_abort);
		m_file_data.set_size((t_size)size);
		m_file->read_object(m_file_data.get_ptr(), (t_size)size, p_abort);

		// Ensure global drum/wavetable data is initialized
		ensure_initialized();

		// Create a memory stream and load the song
		MemoryStream stream(m_file_data.get_ptr(), m_file_data.get_size());
		m_song = Organya::Load(&stream, ORG_SAMPLE_RATE);
		if (!m_song)
			throw exception_io_data("Failed to parse Organya file");

		m_duration_ms = Organya::GetDuration(m_song);
		m_version = Organya::GetVersion(m_song);
	}

	void get_info(file_info & p_info, abort_callback & p_abort) {
		p_info.set_length((double)m_duration_ms * m_loop_count / 1000.0);

		p_info.info_set_int("samplerate", ORG_SAMPLE_RATE);
		p_info.info_set_int("channels", ORG_CHANNELS);
		p_info.info_set_int("bitspersample", 32);
		p_info.info_set("encoding", "synthesized");

		if (m_version == 1)      p_info.info_set("codec", "Organya (Org-01)");
		else if (m_version == 2) p_info.info_set("codec", "Organya (Org-02)");
		else                     p_info.info_set("codec", "Organya (Org-03)");
	}

	t_filestats2 get_stats2(unsigned f, abort_callback & a) {
		return m_file->get_stats2_(f, a);
	}

	t_filestats get_file_stats(abort_callback & p_abort) {
		return m_file->get_stats(p_abort);
	}

	void decode_initialize(unsigned p_flags, abort_callback & p_abort) {
		bool no_looping = (p_flags & input_flag_no_looping) != 0;
		m_max_loops = no_looping ? 1 : m_loop_count;

		m_loops_done = 0;
		m_buffer.clear();
		m_buffer_pos = 0;
		m_done = false;

		Organya::Reset(m_song);
	}

	bool decode_run(audio_chunk & p_chunk, abort_callback & p_abort) {
		if (m_done) return false;

		// Accumulate at least one chunk's worth of frames
		enum { frames_wanted = ORG_DECODE_CHUNK_SIZE };
		float out_buf[frames_wanted * ORG_CHANNELS];
		unsigned frames_written = 0;

		while (frames_written < frames_wanted) {
			// Serve from internal buffer first
			size_t buf_frames = m_buffer.size() / ORG_CHANNELS;
			if (m_buffer_pos < buf_frames) {
				size_t avail = buf_frames - m_buffer_pos;
				size_t need = frames_wanted - frames_written;
				size_t n = (avail < need) ? avail : need;

				memcpy(out_buf + frames_written * ORG_CHANNELS,
				       m_buffer.data() + m_buffer_pos * ORG_CHANNELS,
				       n * ORG_CHANNELS * sizeof(float));
				m_buffer_pos += n;
				frames_written += (unsigned)n;
				continue;
			}

			// Need more data from the decoder
			auto samples = Organya::Render(m_song);
			if (samples.empty()) {
				// Loop boundary reached
				m_loops_done++;
				if (m_loops_done >= m_max_loops) {
					m_done = true;
					break;
				}
				// Continue into the next loop (Render handles the seek-back internally)
				continue;
			}

			m_buffer = std::move(samples);
			m_buffer_pos = 0;
		}

		if (frames_written == 0) return false;

		p_chunk.set_data_32(out_buf, frames_written, ORG_CHANNELS, ORG_SAMPLE_RATE);
		return true;
	}

	void decode_seek(double p_seconds, abort_callback & p_abort) {
		// Reset and skip forward by rendering and discarding.
		Organya::Reset(m_song);
		m_loops_done = 0;
		m_buffer.clear();
		m_buffer_pos = 0;
		m_done = false;

		uint64_t target_frames = (uint64_t)(p_seconds * ORG_SAMPLE_RATE);
		uint64_t frames_skipped = 0;

		while (frames_skipped < target_frames) {
			size_t buf_frames = m_buffer.size() / ORG_CHANNELS;
			if (m_buffer_pos < buf_frames) {
				size_t avail = buf_frames - m_buffer_pos;
				uint64_t need = target_frames - frames_skipped;
				size_t n = (avail < (size_t)need) ? avail : (size_t)need;
				m_buffer_pos += n;
				frames_skipped += n;
				continue;
			}

			auto samples = Organya::Render(m_song);
			if (samples.empty()) {
				m_loops_done++;
				if (m_loops_done >= m_max_loops) {
					m_done = true;
					break;
				}
				continue;
			}
			m_buffer = std::move(samples);
			m_buffer_pos = 0;
		}
	}

	bool decode_can_seek() { return true; }

	void retag(const file_info & p_info, abort_callback & p_abort) {
		throw exception_tagging_unsupported();
	}

	void remove_tags(abort_callback &) {
		throw exception_tagging_unsupported();
	}

	static bool g_is_our_content_type(const char * p_content_type) { return false; }

	static bool g_is_our_path(const char * p_path, const char * p_extension) {
		return stricmp_utf8(p_extension, "org") == 0;
	}

	static const char * g_get_name() { return "Organya decoder"; }

	static const GUID g_get_guid() {
		// {B2E7F3A1-4C89-4D56-8E12-A3B5C7D9E0F2}
		static const GUID guid = { 0xb2e7f3a1, 0x4c89, 0x4d56, { 0x8e, 0x12, 0xa3, 0xb5, 0xc7, 0xd9, 0xe0, 0xf2 } };
		return guid;
	}

private:
	Organya::Song* m_song = nullptr;

	service_ptr_t<file> m_file;
	pfc::array_t<t_uint8> m_file_data;

	uint32_t m_duration_ms = 0;
	int32_t m_version = 0;
	unsigned m_loop_count = ORG_DEFAULT_LOOP_COUNT;

	// Decode state
	std::vector<float> m_buffer;    // current beat's rendered samples
	size_t m_buffer_pos = 0;        // read position in m_buffer (in frames)
	unsigned m_loops_done = 0;
	unsigned m_max_loops = ORG_DEFAULT_LOOP_COUNT;
	bool m_done = false;
};

static input_singletrack_factory_t<input_org> g_input_org_factory;

DECLARE_FILE_TYPE("Organya music files", "*.ORG");

} // anonymous namespace
