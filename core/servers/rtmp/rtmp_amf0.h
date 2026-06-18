#ifndef ARGUS_CORE_SERVERS_RTMP_AMF0_H
#define ARGUS_CORE_SERVERS_RTMP_AMF0_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct irs3_amf0_reader {
    const uint8_t *data;
    size_t len;
    size_t pos;
} irs3_amf0_reader;

typedef struct irs3_amf0_property {
    char name[64];
    int type;
    double number_value;
    int boolean_value;
    char string_value[256];
} irs3_amf0_property;

typedef struct irs3_amf0_command {
    char name[64];
    double transaction_id;
    char app[256];
    char stream_name[256];
    char publish_type[64];
} irs3_amf0_command;

void irs3_amf0_reader_init(irs3_amf0_reader *reader, const uint8_t *data, size_t len);
int irs3_amf0_parse_command(irs3_amf0_reader *reader, irs3_amf0_command *command);

size_t irs3_amf0_write_string(uint8_t *dst, const char *value);
size_t irs3_amf0_write_number(uint8_t *dst, double value);
size_t irs3_amf0_write_boolean(uint8_t *dst, int value);
size_t irs3_amf0_write_null(uint8_t *dst);
size_t irs3_amf0_write_object_start(uint8_t *dst);
size_t irs3_amf0_write_named_string(uint8_t *dst, const char *name, const char *value);
size_t irs3_amf0_write_named_number(uint8_t *dst, const char *name, double value);
size_t irs3_amf0_write_named_boolean(uint8_t *dst, const char *name, int value);
size_t irs3_amf0_write_object_end(uint8_t *dst);

#ifdef __cplusplus
}
#endif

#endif
