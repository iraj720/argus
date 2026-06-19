#include "core/sinks/hls_sink.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
#include <libavutil/channel_layout.h>
#include <libavutil/time.h>
}

#include <errno.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

namespace {

constexpr int kHlsSinkModeFlv = 1;
constexpr int kHlsSinkModePacket = 2;

}

void irs3_hls_sink_default_options(irs3_hls_sink_options *options);

static void sink_options_from_input(irs3_hls_sink *sink, const irs3_hls_sink_options *options) {
    irs3_hls_sink_options resolved;

    irs3_hls_sink_default_options(&resolved);
    if (options != NULL) {
        resolved = *options;
    }

    if (resolved.output_mode != IRS3_HLS_SINK_OUTPUT_LIVE) {
        resolved.output_mode = IRS3_HLS_SINK_OUTPUT_RECORD;
    }
    if (resolved.output_mode == IRS3_HLS_SINK_OUTPUT_LIVE) {
        if (resolved.playlist_size <= 0) {
            resolved.playlist_size = 3;
        }
        if (resolved.retain_extra_segments < 0) {
            resolved.retain_extra_segments = 3;
        }
    } else {
        resolved.playlist_size = 0;
        resolved.retain_extra_segments = 0;
    }

    sink->output_mode = (int)resolved.output_mode;
    sink->playlist_size = resolved.playlist_size;
    sink->retain_extra_segments = resolved.retain_extra_segments;
    sink->cleaner_deleted_until = -1;
}

static int mkdir_p(const char *path) {
    char tmp[1024];
    size_t len = strlen(path);
    if (len >= sizeof(tmp)) {
        return -1;
    }
    snprintf(tmp, sizeof(tmp), "%s", path);
    for (char *p = tmp + 1; *p; ++p) {
        if (*p == '/') {
            *p = '\0';
            if (mkdir(tmp, 0755) != 0 && errno != EEXIST) {
                return -1;
            }
            *p = '/';
        }
    }
    if (mkdir(tmp, 0755) != 0 && errno != EEXIST) {
        return -1;
    }
    return 0;
}

static void sanitize_component(char *dst, size_t dst_len, const char *src) {
    size_t offset = 0;
    for (const char *p = src; *p && offset + 1 < dst_len; ++p) {
        char c = *p;
        if ((c >= 'a' && c <= 'z') ||
            (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') ||
            c == '_' || c == '-' || c == '.') {
            dst[offset++] = c;
        } else {
            dst[offset++] = '_';
        }
    }
    dst[offset] = '\0';
}

static void sink_logf(irs3_hls_sink *sink, const char *fmt, ...) {
    FILE *fp;
    va_list args;

    fp = fopen(sink->stderr_path, "a");
    if (fp == NULL) {
        return;
    }
    va_start(args, fmt);
    vfprintf(fp, fmt, args);
    va_end(args);
    fclose(fp);
}

static void ffmpeg_error_string(int errnum, char *buf, size_t buf_len) {
    if (av_strerror(errnum, buf, buf_len) != 0) {
        snprintf(buf, buf_len, "ffmpeg error %d", errnum);
    }
}

void irs3_hls_sink_default_options(irs3_hls_sink_options *options) {
    if (options == NULL) {
        return;
    }
    options->output_mode = IRS3_HLS_SINK_OUTPUT_RECORD;
    options->playlist_size = 0;
    options->retain_extra_segments = 0;
}

static int sink_queue_ensure_capacity(irs3_hls_sink *sink, size_t needed) {
    uint8_t *next_buf;
    size_t unread_len;
    size_t next_cap;

    unread_len = sink->queue_len - sink->queue_off;
    if (sink->queue_off > 0 && unread_len > 0) {
        memmove(sink->queue_buf, sink->queue_buf + sink->queue_off, unread_len);
    }
    sink->queue_len = unread_len;
    sink->queue_off = 0;

    if (sink->queue_cap - sink->queue_len >= needed) {
        return 0;
    }

    next_cap = sink->queue_cap == 0 ? 32768 : sink->queue_cap;
    while (next_cap - sink->queue_len < needed) {
        next_cap *= 2;
    }
    next_buf = (uint8_t *)realloc(sink->queue_buf, next_cap);
    if (next_buf == NULL) {
        return -1;
    }
    sink->queue_buf = next_buf;
    sink->queue_cap = next_cap;
    return 0;
}

static int sink_queue_write(irs3_hls_sink *sink, const uint8_t *buf, size_t len) {
    if (len == 0) {
        return 0;
    }

    pthread_mutex_lock(&sink->mutex);
    if (sink->closed || (sink->worker_finished && sink->ffmpeg_exit_code != 0)) {
        pthread_mutex_unlock(&sink->mutex);
        return -1;
    }
    if (sink_queue_ensure_capacity(sink, len) != 0) {
        pthread_mutex_unlock(&sink->mutex);
        return -1;
    }
    memcpy(sink->queue_buf + sink->queue_len, buf, len);
    sink->queue_len += len;
    pthread_cond_signal(&sink->cond);
    pthread_mutex_unlock(&sink->mutex);
    return 0;
}

static int sink_store_blob(uint8_t **dst, size_t *dst_len, const uint8_t *src, size_t src_len) {
    uint8_t *next;

    free(*dst);
    *dst = NULL;
    *dst_len = 0;

    if (src == NULL || src_len == 0) {
        return 0;
    }

    next = (uint8_t *)malloc(src_len);
    if (next == NULL) {
        return -1;
    }
    memcpy(next, src, src_len);
    *dst = next;
    *dst_len = src_len;
    return 0;
}

static int sink_build_flv_tag_blob(
    uint8_t tag_type,
    uint32_t timestamp_ms,
    const uint8_t *payload,
    size_t payload_len,
    uint8_t **blob_out,
    size_t *blob_len_out
) {
    uint8_t *blob;
    uint32_t prev;

    if (blob_out == NULL || blob_len_out == NULL) {
        return -1;
    }

    blob = (uint8_t *)malloc(11 + payload_len + 4);
    if (blob == NULL) {
        return -1;
    }

    blob[0] = tag_type;
    blob[1] = (uint8_t)((payload_len >> 16) & 0xff);
    blob[2] = (uint8_t)((payload_len >> 8) & 0xff);
    blob[3] = (uint8_t)(payload_len & 0xff);
    blob[4] = (uint8_t)((timestamp_ms >> 16) & 0xff);
    blob[5] = (uint8_t)((timestamp_ms >> 8) & 0xff);
    blob[6] = (uint8_t)(timestamp_ms & 0xff);
    blob[7] = (uint8_t)((timestamp_ms >> 24) & 0xff);
    blob[8] = 0;
    blob[9] = 0;
    blob[10] = 0;
    if (payload_len > 0) {
        memcpy(blob + 11, payload, payload_len);
    }
    prev = (uint32_t)(11 + payload_len);
    blob[11 + payload_len + 0] = (uint8_t)((prev >> 24) & 0xff);
    blob[11 + payload_len + 1] = (uint8_t)((prev >> 16) & 0xff);
    blob[11 + payload_len + 2] = (uint8_t)((prev >> 8) & 0xff);
    blob[11 + payload_len + 3] = (uint8_t)(prev & 0xff);

    *blob_out = blob;
    *blob_len_out = 11 + payload_len + 4;
    return 0;
}

static int sink_write_blob(irs3_hls_sink *sink, const uint8_t *blob, size_t blob_len) {
    if (blob == NULL || blob_len == 0) {
        return 0;
    }
    return sink_queue_write(sink, blob, blob_len);
}

static void sink_request_cleaner_scan(irs3_hls_sink *sink) {
    if (sink == NULL || sink->output_mode != IRS3_HLS_SINK_OUTPUT_LIVE || !sink->cleaner_started) {
        return;
    }

    pthread_mutex_lock(&sink->cleaner_mutex);
    sink->cleaner_scan_requested = 1;
    pthread_cond_signal(&sink->cleaner_cond);
    pthread_mutex_unlock(&sink->cleaner_mutex);
}

static int sink_parse_media_sequence(irs3_hls_sink *sink, long *media_sequence_out) {
    FILE *fp;
    char line[512];

    if (media_sequence_out == NULL) {
        return -1;
    }

    fp = fopen(sink->manifest_path, "r");
    if (fp == NULL) {
        return -1;
    }

    while (fgets(line, sizeof(line), fp) != NULL) {
        const char *prefix = "#EXT-X-MEDIA-SEQUENCE:";
        size_t prefix_len = strlen(prefix);
        if (strncmp(line, prefix, prefix_len) == 0) {
            char *end = NULL;
            long value = strtol(line + prefix_len, &end, 10);
            fclose(fp);
            if (end == line + prefix_len) {
                return -1;
            }
            *media_sequence_out = value;
            return 0;
        }
    }

    fclose(fp);
    return -1;
}

static void sink_run_segment_cleanup(irs3_hls_sink *sink) {
    char segment_path[1200];
    long media_sequence;
    long delete_until;
    long segment_index;

    if (sink->output_mode != IRS3_HLS_SINK_OUTPUT_LIVE) {
        return;
    }
    if (sink_parse_media_sequence(sink, &media_sequence) != 0) {
        return;
    }

    delete_until = media_sequence - sink->retain_extra_segments - 1;
    if (delete_until <= sink->cleaner_deleted_until) {
        return;
    }

    for (segment_index = sink->cleaner_deleted_until + 1; segment_index <= delete_until; ++segment_index) {
        snprintf(segment_path, sizeof(segment_path), "%s/seg_%06ld.ts", sink->output_dir, segment_index);
        if (unlink(segment_path) != 0 && errno != ENOENT) {
            sink_logf(sink, "failed to remove stale segment %s: %s\n", segment_path, strerror(errno));
        }
    }
    sink->cleaner_deleted_until = delete_until;
}

static void *sink_cleaner_main(void *opaque) {
    irs3_hls_sink *sink;

    sink = (irs3_hls_sink *)opaque;
    pthread_mutex_lock(&sink->cleaner_mutex);
    sink->cleaner_running = 1;
    while (!sink->cleaner_closed) {
        while (!sink->cleaner_scan_requested && !sink->cleaner_closed) {
            pthread_cond_wait(&sink->cleaner_cond, &sink->cleaner_mutex);
        }
        sink->cleaner_scan_requested = 0;
        pthread_mutex_unlock(&sink->cleaner_mutex);
        sink_run_segment_cleanup(sink);
        pthread_mutex_lock(&sink->cleaner_mutex);
    }
    pthread_mutex_unlock(&sink->cleaner_mutex);
    sink_run_segment_cleanup(sink);
    return NULL;
}

static int sink_start_cleaner(irs3_hls_sink *sink) {
    if (sink->output_mode != IRS3_HLS_SINK_OUTPUT_LIVE) {
        return 0;
    }
    if (pthread_mutex_init(&sink->cleaner_mutex, NULL) != 0) {
        return -1;
    }
    if (pthread_cond_init(&sink->cleaner_cond, NULL) != 0) {
        pthread_mutex_destroy(&sink->cleaner_mutex);
        return -1;
    }
    if (pthread_create(&sink->cleaner_thread, NULL, sink_cleaner_main, sink) != 0) {
        pthread_cond_destroy(&sink->cleaner_cond);
        pthread_mutex_destroy(&sink->cleaner_mutex);
        return -1;
    }
    sink->cleaner_started = 1;
    return 0;
}

static void sink_stop_cleaner(irs3_hls_sink *sink) {
    if (!sink->cleaner_started) {
        return;
    }

    pthread_mutex_lock(&sink->cleaner_mutex);
    sink->cleaner_closed = 1;
    sink->cleaner_scan_requested = 1;
    pthread_cond_signal(&sink->cleaner_cond);
    pthread_mutex_unlock(&sink->cleaner_mutex);

    pthread_join(sink->cleaner_thread, NULL);
    pthread_cond_destroy(&sink->cleaner_cond);
    pthread_mutex_destroy(&sink->cleaner_mutex);
    sink->cleaner_started = 0;
}

static int sink_is_video_config_tag(const uint8_t *payload, size_t payload_len) {
    if (payload == NULL || payload_len < 2) {
        return 0;
    }
    return (payload[0] & 0x0f) == 7 && payload[1] == 0;
}

static int sink_is_video_keyframe_tag(const uint8_t *payload, size_t payload_len) {
    if (payload == NULL || payload_len < 2) {
        return 0;
    }
    return ((payload[0] >> 4) == 1) && ((payload[0] & 0x0f) == 7) && payload[1] == 1;
}

static int sink_is_audio_config_tag(const uint8_t *payload, size_t payload_len) {
    if (payload == NULL || payload_len < 2) {
        return 0;
    }
    return ((payload[0] >> 4) == 10) && payload[1] == 0;
}

static int sink_flush_startup_tags(irs3_hls_sink *sink) {
    if (sink_write_blob(sink, sink->startup_metadata_tag, sink->startup_metadata_tag_len) != 0) {
        return -1;
    }
    if (sink_write_blob(sink, sink->startup_audio_config_tag, sink->startup_audio_config_tag_len) != 0) {
        return -1;
    }
    if (sink_write_blob(sink, sink->startup_video_config_tag, sink->startup_video_config_tag_len) != 0) {
        return -1;
    }
    return 0;
}

static int sink_avio_read(void *opaque, uint8_t *buf, int buf_size) {
    irs3_hls_sink *sink;
    size_t available;
    size_t chunk_len;

    sink = (irs3_hls_sink *)opaque;
    pthread_mutex_lock(&sink->mutex);
    for (;;) {
        available = sink->queue_len - sink->queue_off;
        if (available > 0) {
            chunk_len = available;
            if ((size_t)buf_size < chunk_len) {
                chunk_len = (size_t)buf_size;
            }
            memcpy(buf, sink->queue_buf + sink->queue_off, chunk_len);
            sink->queue_off += chunk_len;
            if (sink->queue_off == sink->queue_len) {
                sink->queue_off = 0;
                sink->queue_len = 0;
            }
            pthread_mutex_unlock(&sink->mutex);
            return (int)chunk_len;
        }
        if (sink->closed) {
            pthread_mutex_unlock(&sink->mutex);
            return AVERROR_EOF;
        }
        pthread_cond_wait(&sink->cond, &sink->mutex);
    }
}

static void sink_rescale_packet(
    AVPacket *packet,
    const AVStream *input_stream,
    const AVStream *output_stream
) {
    if (packet->pts != AV_NOPTS_VALUE) {
        packet->pts = av_rescale_q_rnd(
            packet->pts,
            input_stream->time_base,
            output_stream->time_base,
            static_cast<AVRounding>(AV_ROUND_NEAR_INF | AV_ROUND_PASS_MINMAX)
        );
    }
    if (packet->dts != AV_NOPTS_VALUE) {
        packet->dts = av_rescale_q_rnd(
            packet->dts,
            input_stream->time_base,
            output_stream->time_base,
            static_cast<AVRounding>(AV_ROUND_NEAR_INF | AV_ROUND_PASS_MINMAX)
        );
    }
    if (packet->duration > 0) {
        packet->duration = (int)av_rescale_q(
            packet->duration,
            input_stream->time_base,
            output_stream->time_base
        );
    }
    packet->pos = -1;
}

static int64_t sink_normalize_timestamp(
    irs3_hls_sink *sink,
    size_t stream_index,
    int64_t source_timestamp,
    int64_t duration,
    int is_dts
) {
    int64_t normalized;
    int64_t first_timestamp;
    int64_t last_timestamp;

    if (!sink->packet_track_initialized[stream_index]) {
        sink->packet_track_initialized[stream_index] = 1;
        sink->packet_first_pts[stream_index] = source_timestamp;
        sink->packet_first_dts[stream_index] = source_timestamp;
        sink->packet_last_pts[stream_index] = 0;
        sink->packet_last_dts[stream_index] = 0;
        return 0;
    }

    first_timestamp = is_dts ? sink->packet_first_dts[stream_index] : sink->packet_first_pts[stream_index];
    normalized = source_timestamp - first_timestamp;
    last_timestamp = is_dts ? sink->packet_last_dts[stream_index] : sink->packet_last_pts[stream_index];
    if (normalized < last_timestamp) {
        normalized = duration > 0 ? last_timestamp + duration : last_timestamp;
    }
    return normalized;
}

static enum AVCodecID sink_codec_id(irs3_hls_sink_codec codec) {
    switch (codec) {
    case IRS3_HLS_SINK_CODEC_H264:
        return AV_CODEC_ID_H264;
    case IRS3_HLS_SINK_CODEC_AAC:
        return AV_CODEC_ID_AAC;
    case IRS3_HLS_SINK_CODEC_OPUS:
        return AV_CODEC_ID_OPUS;
    default:
        return AV_CODEC_ID_NONE;
    }
}

static int sink_open_packet_output(
    irs3_hls_sink *sink,
    const irs3_hls_sink_stream_config *streams,
    size_t stream_count,
    AVFormatContext **output_ctx_out
) {
    AVFormatContext *output_ctx;
    AVDictionary *options;
    char segment_pattern[1024];
    char list_size[32];
    size_t i;
    int ret;
    char errbuf[128];

    output_ctx = NULL;
    options = NULL;

    ret = avformat_alloc_output_context2(&output_ctx, NULL, "hls", sink->manifest_path);
    if (ret < 0 || output_ctx == NULL) {
        return ret < 0 ? ret : AVERROR_UNKNOWN;
    }

    for (i = 0; i < stream_count; ++i) {
        const irs3_hls_sink_stream_config *stream_cfg;
        AVStream *output_stream;
        enum AVCodecID codec_id;

        stream_cfg = &streams[i];
        codec_id = sink_codec_id(stream_cfg->codec);
        if (codec_id == AV_CODEC_ID_NONE) {
            sink_logf(sink, "unsupported packet-mode codec %d\n", (int)stream_cfg->codec);
            avformat_free_context(output_ctx);
            return AVERROR(EINVAL);
        }
        if (sink->output_mode == IRS3_HLS_SINK_OUTPUT_LIVE &&
            stream_cfg->codec != IRS3_HLS_SINK_CODEC_H264 &&
            stream_cfg->codec != IRS3_HLS_SINK_CODEC_AAC) {
            sink_logf(sink, "live packet-mode sink currently requires H264/AAC elementary streams\n");
            avformat_free_context(output_ctx);
            return AVERROR(EINVAL);
        }

        output_stream = avformat_new_stream(output_ctx, NULL);
        if (output_stream == NULL) {
            sink_logf(sink, "avformat_new_stream failed\n");
            avformat_free_context(output_ctx);
            return AVERROR(ENOMEM);
        }

        output_stream->time_base = AVRational{1, stream_cfg->clock_rate};
        output_stream->codecpar->codec_type =
            stream_cfg->kind == IRS3_HLS_SINK_STREAM_VIDEO ? AVMEDIA_TYPE_VIDEO : AVMEDIA_TYPE_AUDIO;
        output_stream->codecpar->codec_id = codec_id;
        output_stream->codecpar->codec_tag = 0;

        if (stream_cfg->kind == IRS3_HLS_SINK_STREAM_AUDIO) {
            output_stream->codecpar->sample_rate = stream_cfg->clock_rate;
            av_channel_layout_default(&output_stream->codecpar->ch_layout, stream_cfg->channels);
        } else {
            output_stream->codecpar->width = stream_cfg->width;
            output_stream->codecpar->height = stream_cfg->height;
        }

        if (stream_cfg->extradata_len > 0) {
            output_stream->codecpar->extradata = static_cast<uint8_t *>(
                av_mallocz(stream_cfg->extradata_len + AV_INPUT_BUFFER_PADDING_SIZE)
            );
            if (output_stream->codecpar->extradata == NULL) {
                sink_logf(sink, "failed to allocate packet-mode extradata\n");
                avformat_free_context(output_ctx);
                return AVERROR(ENOMEM);
            }
            memcpy(output_stream->codecpar->extradata, stream_cfg->extradata, stream_cfg->extradata_len);
            output_stream->codecpar->extradata_size = (int)stream_cfg->extradata_len;
        }

    }

    snprintf(segment_pattern, sizeof(segment_pattern), "%s/seg_%%06d.ts", sink->output_dir);
    av_dict_set(&options, "hls_time", "1", 0);
    if (sink->output_mode == IRS3_HLS_SINK_OUTPUT_LIVE) {
        snprintf(list_size, sizeof(list_size), "%d", sink->playlist_size);
        av_dict_set(&options, "hls_list_size", list_size, 0);
        av_dict_set(&options, "hls_segment_type", "mpegts", 0);
        av_dict_set(&options, "hls_segment_filename", segment_pattern, 0);
        av_dict_set(&options, "hls_flags", "independent_segments", 0);
    } else {
        av_dict_set(&options, "hls_list_size", "0", 0);
        av_dict_set(&options, "hls_segment_type", "fmp4", 0);
        av_dict_set(&options, "hls_fmp4_init_filename", "init.mp4", 0);
        av_dict_set(&options, "hls_flags", "independent_segments", 0);
    }

    if ((output_ctx->oformat->flags & AVFMT_NOFILE) == 0) {
        ret = avio_open(&output_ctx->pb, sink->manifest_path, AVIO_FLAG_WRITE);
        if (ret < 0) {
            ffmpeg_error_string(ret, errbuf, sizeof(errbuf));
            sink_logf(sink, "avio_open failed: %s\n", errbuf);
            av_dict_free(&options);
            avformat_free_context(output_ctx);
            return ret;
        }
    }

    ret = avformat_write_header(output_ctx, &options);
    av_dict_free(&options);
    if (ret < 0) {
        ffmpeg_error_string(ret, errbuf, sizeof(errbuf));
        sink_logf(sink, "avformat_write_header failed: %s\n", errbuf);
        if ((output_ctx->oformat->flags & AVFMT_NOFILE) == 0) {
            avio_closep(&output_ctx->pb);
        }
        avformat_free_context(output_ctx);
        return ret;
    }

    *output_ctx_out = output_ctx;
    return 0;
}

static int sink_open_output(irs3_hls_sink *sink, AVFormatContext *input_ctx, AVFormatContext **output_ctx_out) {
    AVFormatContext *output_ctx;
    AVDictionary *options;
    char segment_pattern[1024];
    char list_size[32];
    int ret;
    unsigned int i;

    output_ctx = NULL;
    options = NULL;
    snprintf(segment_pattern, sizeof(segment_pattern), "%s/seg_%%06d.ts", sink->output_dir);

    ret = avformat_alloc_output_context2(&output_ctx, NULL, "hls", sink->manifest_path);
    if (ret < 0 || output_ctx == NULL) {
        return ret < 0 ? ret : AVERROR_UNKNOWN;
    }

    for (i = 0; i < input_ctx->nb_streams; ++i) {
        AVStream *input_stream;
        AVStream *output_stream;

        input_stream = input_ctx->streams[i];
        output_stream = avformat_new_stream(output_ctx, NULL);
        if (output_stream == NULL) {
            avformat_free_context(output_ctx);
            return AVERROR(ENOMEM);
        }
        ret = avcodec_parameters_copy(output_stream->codecpar, input_stream->codecpar);
        if (ret < 0) {
            avformat_free_context(output_ctx);
            return ret;
        }
        output_stream->codecpar->codec_tag = 0;
        output_stream->time_base = input_stream->time_base;
    }

    snprintf(list_size, sizeof(list_size), "%d", sink->output_mode == IRS3_HLS_SINK_OUTPUT_LIVE ? sink->playlist_size : 0);
    av_dict_set(&options, "hls_time", "1", 0);
    av_dict_set(&options, "hls_list_size", list_size, 0);
    av_dict_set(&options, "hls_segment_filename", segment_pattern, 0);
    av_dict_set(&options, "hls_flags", "independent_segments", 0);

    if ((output_ctx->oformat->flags & AVFMT_NOFILE) == 0) {
        ret = avio_open(&output_ctx->pb, sink->manifest_path, AVIO_FLAG_WRITE);
        if (ret < 0) {
            av_dict_free(&options);
            avformat_free_context(output_ctx);
            return ret;
        }
    }

    ret = avformat_write_header(output_ctx, &options);
    av_dict_free(&options);
    if (ret < 0) {
        if ((output_ctx->oformat->flags & AVFMT_NOFILE) == 0) {
            avio_closep(&output_ctx->pb);
        }
        avformat_free_context(output_ctx);
        return ret;
    }

    *output_ctx_out = output_ctx;
    return 0;
}

static void *sink_worker_main(void *opaque) {
    irs3_hls_sink *sink;
    AVFormatContext *input_ctx;
    AVFormatContext *output_ctx;
    AVIOContext *avio_ctx;
    uint8_t *avio_buf;
    const AVInputFormat *input_format;
    AVPacket packet;
    int result;
    char errbuf[128];

    sink = (irs3_hls_sink *)opaque;
    input_ctx = NULL;
    output_ctx = NULL;
    avio_ctx = NULL;
    avio_buf = NULL;
    result = -1;

    avio_buf = (uint8_t *)av_malloc(32768);
    if (avio_buf == NULL) {
        sink_logf(sink, "failed to allocate avio buffer\n");
        goto done;
    }
    avio_ctx = avio_alloc_context(avio_buf, 32768, 0, sink, sink_avio_read, NULL, NULL);
    if (avio_ctx == NULL) {
        sink_logf(sink, "failed to allocate avio context\n");
        goto done;
    }

    input_ctx = avformat_alloc_context();
    if (input_ctx == NULL) {
        sink_logf(sink, "failed to allocate input format context\n");
        goto done;
    }
    input_ctx->pb = avio_ctx;
    input_ctx->flags |= AVFMT_FLAG_CUSTOM_IO;

    input_format = av_find_input_format("flv");
    if (input_format == NULL) {
        sink_logf(sink, "failed to resolve flv demuxer\n");
        goto done;
    }

    result = avformat_open_input(&input_ctx, NULL, input_format, NULL);
    if (result < 0) {
        ffmpeg_error_string(result, errbuf, sizeof(errbuf));
        sink_logf(sink, "avformat_open_input failed: %s\n", errbuf);
        goto done;
    }

    result = avformat_find_stream_info(input_ctx, NULL);
    if (result < 0) {
        ffmpeg_error_string(result, errbuf, sizeof(errbuf));
        sink_logf(sink, "avformat_find_stream_info failed: %s\n", errbuf);
        goto done;
    }

    result = sink_open_output(sink, input_ctx, &output_ctx);
    if (result < 0) {
        ffmpeg_error_string(result, errbuf, sizeof(errbuf));
        sink_logf(sink, "sink_open_output failed: %s\n", errbuf);
        goto done;
    }

    for (;;) {
        AVStream *input_stream;
        AVStream *output_stream;

        result = av_read_frame(input_ctx, &packet);
        if (result == AVERROR_EOF) {
            result = 0;
            break;
        }
        if (result < 0) {
            ffmpeg_error_string(result, errbuf, sizeof(errbuf));
            sink_logf(sink, "av_read_frame failed: %s\n", errbuf);
            break;
        }

        input_stream = input_ctx->streams[packet.stream_index];
        output_stream = output_ctx->streams[packet.stream_index];
        sink_rescale_packet(&packet, input_stream, output_stream);
        result = av_interleaved_write_frame(output_ctx, &packet);
        av_packet_unref(&packet);
        if (result < 0) {
            ffmpeg_error_string(result, errbuf, sizeof(errbuf));
            sink_logf(sink, "av_interleaved_write_frame failed: %s\n", errbuf);
            break;
        }
    }

    if (result == 0) {
        result = av_write_trailer(output_ctx);
        if (result < 0) {
            ffmpeg_error_string(result, errbuf, sizeof(errbuf));
            sink_logf(sink, "av_write_trailer failed: %s\n", errbuf);
        }
    }

done:
    if (output_ctx != NULL) {
        if ((output_ctx->oformat->flags & AVFMT_NOFILE) == 0) {
            avio_closep(&output_ctx->pb);
        }
        avformat_free_context(output_ctx);
    }
    if (input_ctx != NULL) {
        avformat_close_input(&input_ctx);
    }
    if (avio_ctx != NULL) {
        avio_context_free(&avio_ctx);
    } else if (avio_buf != NULL) {
        av_free(avio_buf);
    }

    pthread_mutex_lock(&sink->mutex);
    sink->ffmpeg_exit_code = result == 0 ? 0 : 1;
    sink->worker_finished = 1;
    sink->worker_running = 0;
    pthread_cond_broadcast(&sink->cond);
    pthread_mutex_unlock(&sink->mutex);
    return NULL;
}

static void write_stats_file(irs3_hls_sink *sink) {
    FILE *fp = fopen(sink->stats_path, "w");
    if (fp == NULL) {
        return;
    }
    fprintf(fp,
        "{\n"
        "  \"session\": \"%s\",\n"
        "  \"manifest\": \"%s\",\n"
        "  \"audio_packets\": %lu,\n"
        "  \"video_packets\": %lu,\n"
        "  \"data_packets\": %lu,\n"
        "  \"bytes_in\": %llu,\n"
        "  \"last_timestamp_ms\": %u,\n"
        "  \"ffmpeg_exit_code\": %d\n"
        "}\n",
        sink->session_name,
        sink->manifest_path,
        sink->audio_packets,
        sink->video_packets,
        sink->data_packets,
        sink->bytes_in,
        sink->last_timestamp_ms,
        sink->ffmpeg_exit_code
    );
    fclose(fp);
}

static int sink_init_common_paths(irs3_hls_sink *sink, const char *output_dir, const char *session_label) {
    if (output_dir == NULL || output_dir[0] == '\0') {
        return -1;
    }
    snprintf(sink->session_name, sizeof(sink->session_name), "%s", session_label != NULL ? session_label : "v2");
    snprintf(sink->output_dir, sizeof(sink->output_dir), "%s", output_dir);
    snprintf(sink->manifest_path, sizeof(sink->manifest_path), "%s/stream.m3u8", sink->output_dir);
    snprintf(sink->stats_path, sizeof(sink->stats_path), "%s/session.json", sink->output_dir);
    snprintf(sink->stderr_path, sizeof(sink->stderr_path), "%s/ffmpeg.stderr.log", sink->output_dir);
    return 0;
}

int irs3_hls_sink_init_at_dir_with_options(
    irs3_hls_sink *sink,
    const char *output_dir,
    const irs3_hls_sink_options *options
) {
    FILE *log_file;

    memset(sink, 0, sizeof(*sink));
    if (sink_init_common_paths(sink, output_dir, "v2-remux") != 0) {
        return -1;
    }
    sink->ffmpeg_exit_code = -1;
    sink->mode = kHlsSinkModeFlv;
    sink->packet_session_start_us = av_gettime_relative();
    sink_options_from_input(sink, options);

    if (mkdir_p(sink->output_dir) != 0) {
        return -1;
    }
    log_file = fopen(sink->stderr_path, "w");
    if (log_file != NULL) {
        fclose(log_file);
    }
    if (pthread_mutex_init(&sink->mutex, NULL) != 0) {
        return -1;
    }
    if (pthread_cond_init(&sink->cond, NULL) != 0) {
        pthread_mutex_destroy(&sink->mutex);
        return -1;
    }
    if (sink_start_cleaner(sink) != 0) {
        pthread_cond_destroy(&sink->cond);
        pthread_mutex_destroy(&sink->mutex);
        return -1;
    }
    if (pthread_create(&sink->worker_thread, NULL, sink_worker_main, sink) != 0) {
        sink_stop_cleaner(sink);
        pthread_cond_destroy(&sink->cond);
        pthread_mutex_destroy(&sink->mutex);
        return -1;
    }
    sink->worker_running = 1;
    sink->started = 1;

    {
        static const uint8_t header[] = {
            'F', 'L', 'V',
            0x01,
            0x05,
            0x00, 0x00, 0x00, 0x09,
            0x00, 0x00, 0x00, 0x00
        };
        if (sink_queue_write(sink, header, sizeof(header)) != 0) {
            irs3_hls_sink_close(sink);
            return -1;
        }
    }
    if (sink->worker_finished && sink->ffmpeg_exit_code != 0) {
        irs3_hls_sink_close(sink);
        return -1;
    }
    return 0;
}

int irs3_hls_sink_init_with_options(
    irs3_hls_sink *sink,
    const char *output_root,
    const char *app,
    const char *stream_name,
    unsigned long session_id,
    const irs3_hls_sink_options *options
) {
    char safe_app[128];
    char safe_stream[128];
    FILE *log_file;

    memset(sink, 0, sizeof(*sink));
    sanitize_component(safe_app, sizeof(safe_app), app);
    sanitize_component(safe_stream, sizeof(safe_stream), stream_name);
    snprintf(sink->session_name, sizeof(sink->session_name), "%s-%s-%06lu", safe_app, safe_stream, session_id);
    snprintf(sink->output_dir, sizeof(sink->output_dir), "%s/%s/%s/%s", output_root, safe_app, safe_stream, sink->session_name);
    snprintf(sink->manifest_path, sizeof(sink->manifest_path), "%s/stream.m3u8", sink->output_dir);
    snprintf(sink->stats_path, sizeof(sink->stats_path), "%s/session.json", sink->output_dir);
    snprintf(sink->stderr_path, sizeof(sink->stderr_path), "%s/ffmpeg.stderr.log", sink->output_dir);
    sink->ffmpeg_exit_code = -1;
    sink->mode = kHlsSinkModeFlv;
    sink->packet_session_start_us = av_gettime_relative();
    sink_options_from_input(sink, options);

    if (mkdir_p(sink->output_dir) != 0) {
        return -1;
    }
    log_file = fopen(sink->stderr_path, "w");
    if (log_file != NULL) {
        fclose(log_file);
    }
    if (pthread_mutex_init(&sink->mutex, NULL) != 0) {
        return -1;
    }
    if (pthread_cond_init(&sink->cond, NULL) != 0) {
        pthread_mutex_destroy(&sink->mutex);
        return -1;
    }
    if (sink_start_cleaner(sink) != 0) {
        pthread_cond_destroy(&sink->cond);
        pthread_mutex_destroy(&sink->mutex);
        return -1;
    }
    if (pthread_create(&sink->worker_thread, NULL, sink_worker_main, sink) != 0) {
        sink_stop_cleaner(sink);
        pthread_cond_destroy(&sink->cond);
        pthread_mutex_destroy(&sink->mutex);
        return -1;
    }
    sink->worker_running = 1;
    sink->started = 1;

    {
        static const uint8_t header[] = {
            'F', 'L', 'V',
            0x01,
            0x05,
            0x00, 0x00, 0x00, 0x09,
            0x00, 0x00, 0x00, 0x00
        };
        if (sink_queue_write(sink, header, sizeof(header)) != 0) {
            irs3_hls_sink_close(sink);
            return -1;
        }
    }
    if (sink->worker_finished && sink->ffmpeg_exit_code != 0) {
        irs3_hls_sink_close(sink);
        return -1;
    }
    return 0;
}

int irs3_hls_sink_init(
    irs3_hls_sink *sink,
    const char *output_root,
    const char *app,
    const char *stream_name,
    unsigned long session_id
) {
    return irs3_hls_sink_init_with_options(sink, output_root, app, stream_name, session_id, nullptr);
}

void irs3_hls_sink_note_media(
    irs3_hls_sink *sink,
    uint8_t message_type_id,
    uint32_t timestamp_ms,
    size_t payload_len
) {
    sink->bytes_in += (unsigned long long)payload_len;
    sink->last_timestamp_ms = timestamp_ms;
    if (message_type_id == 8) {
        sink->audio_packets++;
    } else if (message_type_id == 9) {
        sink->video_packets++;
    } else if (message_type_id == 18) {
        sink->data_packets++;
    }
}

int irs3_hls_sink_write_flv_tag(
    irs3_hls_sink *sink,
    uint8_t tag_type,
    uint32_t timestamp_ms,
    const uint8_t *payload,
    size_t payload_len
) {
    uint8_t *tag_blob;
    size_t tag_blob_len;
    int ret;

    tag_blob = NULL;
    tag_blob_len = 0;

    ret = sink_build_flv_tag_blob(tag_type, timestamp_ms, payload, payload_len, &tag_blob, &tag_blob_len);
    if (ret != 0) {
        return -1;
    }

    if (sink->flv_media_started) {
        ret = sink_write_blob(sink, tag_blob, tag_blob_len);
        free(tag_blob);
        if (ret == 0) {
            sink_request_cleaner_scan(sink);
        }
        return ret;
    }

    if (tag_type == 18) {
        ret = sink_store_blob(&sink->startup_metadata_tag, &sink->startup_metadata_tag_len, tag_blob, tag_blob_len);
        free(tag_blob);
        return ret;
    }

    if (tag_type == 8 && sink_is_audio_config_tag(payload, payload_len)) {
        ret = sink_store_blob(&sink->startup_audio_config_tag, &sink->startup_audio_config_tag_len, tag_blob, tag_blob_len);
        free(tag_blob);
        return ret;
    }

    if (tag_type == 9) {
        sink->flv_seen_video_tag = 1;
        if (sink_is_video_config_tag(payload, payload_len)) {
            ret = sink_store_blob(&sink->startup_video_config_tag, &sink->startup_video_config_tag_len, tag_blob, tag_blob_len);
            free(tag_blob);
            return ret;
        }
        if (sink_is_video_keyframe_tag(payload, payload_len)) {
            if (sink_flush_startup_tags(sink) != 0) {
                free(tag_blob);
                return -1;
            }
            sink->flv_media_started = 1;
            ret = sink_write_blob(sink, tag_blob, tag_blob_len);
            free(tag_blob);
            if (ret == 0) {
                sink_request_cleaner_scan(sink);
            }
            return ret;
        }
        free(tag_blob);
        return 0;
    }

    free(tag_blob);
    return 0;
}

int irs3_hls_sink_init_packet_mode_at_dir_with_options(
    irs3_hls_sink *sink,
    const char *output_dir,
    const irs3_hls_sink_options *options,
    const irs3_hls_sink_stream_config *streams,
    size_t stream_count
) {
    FILE *log_file;
    AVFormatContext *output_ctx;
    size_t i;
    int ret;

    if (stream_count == 0 || stream_count > (sizeof(sink->packet_stream_kinds) / sizeof(sink->packet_stream_kinds[0]))) {
        return -1;
    }

    memset(sink, 0, sizeof(*sink));
    if (sink_init_common_paths(sink, output_dir, "v2-remux") != 0) {
        return -1;
    }
    sink->ffmpeg_exit_code = -1;
    sink->mode = kHlsSinkModePacket;
    sink->packet_stream_count = (int)stream_count;
    sink->packet_session_start_us = av_gettime_relative();
    sink_options_from_input(sink, options);

    if (mkdir_p(sink->output_dir) != 0) {
        return -1;
    }
    log_file = fopen(sink->stderr_path, "w");
    if (log_file != NULL) {
        fclose(log_file);
    }
    if (pthread_mutex_init(&sink->mutex, NULL) != 0) {
        return -1;
    }
    if (pthread_cond_init(&sink->cond, NULL) != 0) {
        pthread_mutex_destroy(&sink->mutex);
        return -1;
    }
    if (sink_start_cleaner(sink) != 0) {
        pthread_cond_destroy(&sink->cond);
        pthread_mutex_destroy(&sink->mutex);
        return -1;
    }

    for (i = 0; i < stream_count; ++i) {
        sink->packet_stream_kinds[i] = (int)streams[i].kind;
        sink->packet_stream_clock_rates[i] = streams[i].clock_rate > 0 ? streams[i].clock_rate : 90000;
    }

    ret = sink_open_packet_output(sink, streams, stream_count, &output_ctx);
    if (ret < 0) {
        sink_stop_cleaner(sink);
        pthread_cond_destroy(&sink->cond);
        pthread_mutex_destroy(&sink->mutex);
        return -1;
    }

    sink->packet_output_ctx = output_ctx;
    sink->ffmpeg_exit_code = 0;
    sink->started = 1;
    return 0;
}

int irs3_hls_sink_init_packet_mode_with_options(
    irs3_hls_sink *sink,
    const char *output_root,
    const char *app,
    const char *stream_name,
    unsigned long session_id,
    const irs3_hls_sink_options *options,
    const irs3_hls_sink_stream_config *streams,
    size_t stream_count
) {
    char safe_app[128];
    char safe_stream[128];
    FILE *log_file;
    AVFormatContext *output_ctx;
    size_t i;
    int ret;

    if (stream_count == 0 || stream_count > (sizeof(sink->packet_stream_kinds) / sizeof(sink->packet_stream_kinds[0]))) {
        return -1;
    }

    memset(sink, 0, sizeof(*sink));
    sanitize_component(safe_app, sizeof(safe_app), app);
    sanitize_component(safe_stream, sizeof(safe_stream), stream_name);
    snprintf(sink->session_name, sizeof(sink->session_name), "%s-%s-%06lu", safe_app, safe_stream, session_id);
    snprintf(sink->output_dir, sizeof(sink->output_dir), "%s/%s/%s/%s", output_root, safe_app, safe_stream, sink->session_name);
    snprintf(sink->manifest_path, sizeof(sink->manifest_path), "%s/stream.m3u8", sink->output_dir);
    snprintf(sink->stats_path, sizeof(sink->stats_path), "%s/session.json", sink->output_dir);
    snprintf(sink->stderr_path, sizeof(sink->stderr_path), "%s/ffmpeg.stderr.log", sink->output_dir);
    sink->ffmpeg_exit_code = -1;
    sink->mode = kHlsSinkModePacket;
    sink->packet_stream_count = (int)stream_count;
    sink->packet_session_start_us = av_gettime_relative();
    sink_options_from_input(sink, options);

    if (mkdir_p(sink->output_dir) != 0) {
        return -1;
    }
    log_file = fopen(sink->stderr_path, "w");
    if (log_file != NULL) {
        fclose(log_file);
    }
    if (pthread_mutex_init(&sink->mutex, NULL) != 0) {
        return -1;
    }
    if (pthread_cond_init(&sink->cond, NULL) != 0) {
        pthread_mutex_destroy(&sink->mutex);
        return -1;
    }
    if (sink_start_cleaner(sink) != 0) {
        pthread_cond_destroy(&sink->cond);
        pthread_mutex_destroy(&sink->mutex);
        return -1;
    }

    for (i = 0; i < stream_count; ++i) {
        sink->packet_stream_kinds[i] = (int)streams[i].kind;
        sink->packet_stream_clock_rates[i] = streams[i].clock_rate > 0 ? streams[i].clock_rate : 90000;
    }

    ret = sink_open_packet_output(sink, streams, stream_count, &output_ctx);
    if (ret < 0) {
        sink_stop_cleaner(sink);
        pthread_cond_destroy(&sink->cond);
        pthread_mutex_destroy(&sink->mutex);
        return -1;
    }

    sink->packet_output_ctx = output_ctx;
    sink->ffmpeg_exit_code = 0;
    sink->started = 1;
    return 0;
}

int irs3_hls_sink_init_packet_mode(
    irs3_hls_sink *sink,
    const char *output_root,
    const char *app,
    const char *stream_name,
    unsigned long session_id,
    const irs3_hls_sink_stream_config *streams,
    size_t stream_count
) {
    return irs3_hls_sink_init_packet_mode_with_options(
        sink,
        output_root,
        app,
        stream_name,
        session_id,
        nullptr,
        streams,
        stream_count
    );
}

int irs3_hls_sink_write_packet(
    irs3_hls_sink *sink,
    size_t stream_index,
    int64_t pts,
    int64_t dts,
    int64_t duration,
    int key_frame,
    const uint8_t *payload,
    size_t payload_len
) {
    AVFormatContext *output_ctx;
    AVPacket packet;
    int ret;

    if (sink == NULL || sink->mode != kHlsSinkModePacket || sink->packet_output_ctx == NULL) {
        return -1;
    }
    if (stream_index >= (size_t)sink->packet_stream_count) {
        return -1;
    }

    pthread_mutex_lock(&sink->mutex);
    if (sink->closed) {
        pthread_mutex_unlock(&sink->mutex);
        return -1;
    }

    output_ctx = (AVFormatContext *)sink->packet_output_ctx;
    memset(&packet, 0, sizeof(packet));

    ret = av_new_packet(&packet, (int)payload_len);
    if (ret < 0) {
        pthread_mutex_unlock(&sink->mutex);
        return -1;
    }

    packet.stream_index = (int)stream_index;
    packet.pts = sink_normalize_timestamp(sink, stream_index, pts, duration, 0);
    packet.dts = sink_normalize_timestamp(sink, stream_index, dts, duration, 1);
    if (packet.dts > packet.pts) {
        packet.pts = packet.dts;
    }
    packet.duration = duration > 0 ? duration : 0;
    packet.pos = -1;
    packet.flags = key_frame ? AV_PKT_FLAG_KEY : 0;
    if (payload_len > 0) {
        memcpy(packet.data, payload, payload_len);
    }

    ret = av_interleaved_write_frame(output_ctx, &packet);
    av_packet_unref(&packet);
    if (ret < 0) {
        sink->ffmpeg_exit_code = 1;
        pthread_mutex_unlock(&sink->mutex);
        return -1;
    }

    sink->packet_last_pts[stream_index] = packet.pts;
    sink->packet_last_dts[stream_index] = packet.dts;

    sink->bytes_in += (unsigned long long)payload_len;
    sink->last_timestamp_ms = (uint32_t)(packet.pts >= 0 ? packet.pts : 0);
    if (sink->packet_stream_kinds[stream_index] == IRS3_HLS_SINK_STREAM_VIDEO) {
        sink->video_packets++;
    } else {
        sink->audio_packets++;
    }

    pthread_mutex_unlock(&sink->mutex);
    sink_request_cleaner_scan(sink);
    return 0;
}

int irs3_hls_sink_close(irs3_hls_sink *sink) {
    if (!sink->started) {
        write_stats_file(sink);
        return 0;
    }

    pthread_mutex_lock(&sink->mutex);
    sink->closed = 1;
    if (sink->mode == kHlsSinkModeFlv) {
        pthread_cond_broadcast(&sink->cond);
    } else if (sink->mode == kHlsSinkModePacket && sink->packet_output_ctx != NULL) {
        AVFormatContext *output_ctx = (AVFormatContext *)sink->packet_output_ctx;
        if (sink->ffmpeg_exit_code == 0 && av_write_trailer(output_ctx) < 0) {
            sink->ffmpeg_exit_code = 1;
        }
        if ((output_ctx->oformat->flags & AVFMT_NOFILE) == 0) {
            avio_closep(&output_ctx->pb);
        }
        avformat_free_context(output_ctx);
        sink->packet_output_ctx = NULL;
    }
    pthread_mutex_unlock(&sink->mutex);

    if (sink->mode == kHlsSinkModeFlv && (sink->worker_running || sink->worker_finished)) {
        pthread_join(sink->worker_thread, NULL);
    }
    sink_stop_cleaner(sink);
    pthread_cond_destroy(&sink->cond);
    pthread_mutex_destroy(&sink->mutex);
    free(sink->queue_buf);
    free(sink->startup_metadata_tag);
    free(sink->startup_audio_config_tag);
    free(sink->startup_video_config_tag);
    sink->queue_buf = NULL;
    sink->startup_metadata_tag = NULL;
    sink->startup_audio_config_tag = NULL;
    sink->startup_video_config_tag = NULL;
    write_stats_file(sink);
    return 0;
}
