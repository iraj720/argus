#ifndef ARGUS_CORE_SINKS_HLS_SINK_H
#define ARGUS_CORE_SINKS_HLS_SINK_H

#include <stddef.h>
#include <pthread.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct irs3_hls_sink {
    char session_name[64];
    char output_dir[1024];
    char manifest_path[1024];
    char stats_path[1024];
    char stderr_path[1024];
    int started;
    int ffmpeg_exit_code;
    int output_mode;
    int playlist_size;
    int retain_extra_segments;
    unsigned long audio_packets;
    unsigned long video_packets;
    unsigned long data_packets;
    unsigned long long bytes_in;
    uint32_t last_timestamp_ms;
    pthread_t worker_thread;
    pthread_mutex_t mutex;
    pthread_cond_t cond;
    int mode;
    int packet_stream_count;
    int packet_stream_kinds[8];
    int packet_stream_clock_rates[8];
    int64_t packet_session_start_us;
    int packet_track_initialized[8];
    int64_t packet_first_pts[8];
    int64_t packet_first_dts[8];
    int64_t packet_last_pts[8];
    int64_t packet_last_dts[8];
    void *packet_output_ctx;
    uint8_t *queue_buf;
    size_t queue_cap;
    size_t queue_len;
    size_t queue_off;
    uint8_t *startup_metadata_tag;
    size_t startup_metadata_tag_len;
    uint8_t *startup_audio_config_tag;
    size_t startup_audio_config_tag_len;
    uint8_t *startup_video_config_tag;
    size_t startup_video_config_tag_len;
    int flv_media_started;
    int flv_seen_video_tag;
    int worker_running;
    int worker_finished;
    int closed;
    pthread_t cleaner_thread;
    pthread_mutex_t cleaner_mutex;
    pthread_cond_t cleaner_cond;
    int cleaner_started;
    int cleaner_running;
    int cleaner_closed;
    int cleaner_scan_requested;
    long cleaner_deleted_until;
} irs3_hls_sink;

typedef enum irs3_hls_sink_output_mode {
    IRS3_HLS_SINK_OUTPUT_RECORD = 0,
    IRS3_HLS_SINK_OUTPUT_LIVE = 1,
} irs3_hls_sink_output_mode;

typedef enum irs3_hls_sink_stream_kind {
    IRS3_HLS_SINK_STREAM_VIDEO = 0,
    IRS3_HLS_SINK_STREAM_AUDIO = 1,
} irs3_hls_sink_stream_kind;

typedef enum irs3_hls_sink_codec {
    IRS3_HLS_SINK_CODEC_H264 = 0,
    IRS3_HLS_SINK_CODEC_AAC = 1,
    IRS3_HLS_SINK_CODEC_OPUS = 2,
} irs3_hls_sink_codec;

typedef struct irs3_hls_sink_options {
    irs3_hls_sink_output_mode output_mode;
    int playlist_size;
    int retain_extra_segments;
} irs3_hls_sink_options;

typedef struct irs3_hls_sink_stream_config {
    irs3_hls_sink_stream_kind kind;
    irs3_hls_sink_codec codec;
    int clock_rate;
    int channels;
    int width;
    int height;
    const uint8_t *extradata;
    size_t extradata_len;
} irs3_hls_sink_stream_config;

void irs3_hls_sink_default_options(irs3_hls_sink_options *options);

int irs3_hls_sink_init_with_options(
    irs3_hls_sink *sink,
    const char *output_root,
    const char *app,
    const char *stream_name,
    unsigned long session_id,
    const irs3_hls_sink_options *options
);

int irs3_hls_sink_init_at_dir_with_options(
    irs3_hls_sink *sink,
    const char *output_dir,
    const irs3_hls_sink_options *options
);

int irs3_hls_sink_init(
    irs3_hls_sink *sink,
    const char *output_root,
    const char *app,
    const char *stream_name,
    unsigned long session_id
);

int irs3_hls_sink_write_flv_tag(
    irs3_hls_sink *sink,
    uint8_t tag_type,
    uint32_t timestamp_ms,
    const uint8_t *payload,
    size_t payload_len
);

int irs3_hls_sink_init_packet_mode(
    irs3_hls_sink *sink,
    const char *output_root,
    const char *app,
    const char *stream_name,
    unsigned long session_id,
    const irs3_hls_sink_stream_config *streams,
    size_t stream_count
);

int irs3_hls_sink_init_packet_mode_with_options(
    irs3_hls_sink *sink,
    const char *output_root,
    const char *app,
    const char *stream_name,
    unsigned long session_id,
    const irs3_hls_sink_options *options,
    const irs3_hls_sink_stream_config *streams,
    size_t stream_count
);

int irs3_hls_sink_init_packet_mode_at_dir_with_options(
    irs3_hls_sink *sink,
    const char *output_dir,
    const irs3_hls_sink_options *options,
    const irs3_hls_sink_stream_config *streams,
    size_t stream_count
);

int irs3_hls_sink_write_packet(
    irs3_hls_sink *sink,
    size_t stream_index,
    int64_t pts,
    int64_t dts,
    int64_t duration,
    int key_frame,
    const uint8_t *payload,
    size_t payload_len
);

void irs3_hls_sink_note_media(
    irs3_hls_sink *sink,
    uint8_t message_type_id,
    uint32_t timestamp_ms,
    size_t payload_len
);

int irs3_hls_sink_close(irs3_hls_sink *sink);

#ifdef __cplusplus
}
#endif

#endif
