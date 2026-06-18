#include "core/servers/rtmp/rtmp_amf0.h"

#include <arpa/inet.h>
#include <stdio.h>
#include <string.h>

#define IRS3_AMF0_NUMBER 0x00
#define IRS3_AMF0_BOOLEAN 0x01
#define IRS3_AMF0_STRING 0x02
#define IRS3_AMF0_OBJECT 0x03
#define IRS3_AMF0_NULL 0x05
#define IRS3_AMF0_OBJECT_END 0x09

static int amf0_read_byte(irs3_amf0_reader *reader, uint8_t *value) {
    if (reader->pos >= reader->len) {
        return -1;
    }
    *value = reader->data[reader->pos++];
    return 0;
}

static int amf0_read_be16(irs3_amf0_reader *reader, uint16_t *value) {
    if ((reader->len - reader->pos) < 2) {
        return -1;
    }
    *value = (uint16_t)((reader->data[reader->pos] << 8) | reader->data[reader->pos + 1]);
    reader->pos += 2;
    return 0;
}

static int amf0_read_string_raw(irs3_amf0_reader *reader, char *dst, size_t dst_len) {
    uint16_t len = 0;
    if (amf0_read_be16(reader, &len) != 0) {
        return -1;
    }
    if ((reader->len - reader->pos) < len) {
        return -1;
    }
    size_t copy_len = len;
    if (copy_len >= dst_len) {
        copy_len = dst_len - 1;
    }
    memcpy(dst, reader->data + reader->pos, copy_len);
    dst[copy_len] = '\0';
    reader->pos += len;
    return 0;
}

static int amf0_read_number_raw(irs3_amf0_reader *reader, double *value) {
    union {
        uint64_t u64;
        double d;
    } conv;
    if ((reader->len - reader->pos) < 8) {
        return -1;
    }
    conv.u64 = ((uint64_t)reader->data[reader->pos] << 56) |
               ((uint64_t)reader->data[reader->pos + 1] << 48) |
               ((uint64_t)reader->data[reader->pos + 2] << 40) |
               ((uint64_t)reader->data[reader->pos + 3] << 32) |
               ((uint64_t)reader->data[reader->pos + 4] << 24) |
               ((uint64_t)reader->data[reader->pos + 5] << 16) |
               ((uint64_t)reader->data[reader->pos + 6] << 8) |
               ((uint64_t)reader->data[reader->pos + 7]);
    reader->pos += 8;
    *value = conv.d;
    return 0;
}

static void amf0_skip_value(irs3_amf0_reader *reader);

static void amf0_read_object(irs3_amf0_reader *reader, irs3_amf0_command *command) {
    while ((reader->len - reader->pos) >= 3) {
        if (reader->data[reader->pos] == 0x00 &&
            reader->data[reader->pos + 1] == 0x00 &&
            reader->data[reader->pos + 2] == IRS3_AMF0_OBJECT_END) {
            reader->pos += 3;
            return;
        }

        char key[64];
        uint8_t type = 0;
        if (amf0_read_string_raw(reader, key, sizeof(key)) != 0) {
            return;
        }
        if (amf0_read_byte(reader, &type) != 0) {
            return;
        }

        if (type == IRS3_AMF0_STRING) {
            char value[256];
            if (amf0_read_string_raw(reader, value, sizeof(value)) != 0) {
                return;
            }
            if (strcmp(key, "app") == 0) {
                snprintf(command->app, sizeof(command->app), "%s", value);
            }
        } else if (type == IRS3_AMF0_NUMBER) {
            double ignored = 0;
            if (amf0_read_number_raw(reader, &ignored) != 0) {
                return;
            }
        } else if (type == IRS3_AMF0_BOOLEAN) {
            uint8_t ignored = 0;
            if (amf0_read_byte(reader, &ignored) != 0) {
                return;
            }
        } else if (type == IRS3_AMF0_OBJECT) {
            amf0_read_object(reader, command);
        } else if (type == IRS3_AMF0_NULL) {
        } else {
            amf0_skip_value(reader);
        }
    }
}

static void amf0_skip_value(irs3_amf0_reader *reader) {
    if (reader->pos == 0) {
        return;
    }
    uint8_t type = reader->data[reader->pos - 1];
    if (type == IRS3_AMF0_STRING) {
        char scratch[256];
        (void)amf0_read_string_raw(reader, scratch, sizeof(scratch));
    } else if (type == IRS3_AMF0_NUMBER) {
        double ignored = 0;
        (void)amf0_read_number_raw(reader, &ignored);
    } else if (type == IRS3_AMF0_BOOLEAN) {
        uint8_t ignored = 0;
        (void)amf0_read_byte(reader, &ignored);
    } else if (type == IRS3_AMF0_OBJECT) {
        irs3_amf0_command ignored;
        memset(&ignored, 0, sizeof(ignored));
        amf0_read_object(reader, &ignored);
    }
}

void irs3_amf0_reader_init(irs3_amf0_reader *reader, const uint8_t *data, size_t len) {
    reader->data = data;
    reader->len = len;
    reader->pos = 0;
}

int irs3_amf0_parse_command(irs3_amf0_reader *reader, irs3_amf0_command *command) {
    uint8_t type = 0;
    memset(command, 0, sizeof(*command));

    if (amf0_read_byte(reader, &type) != 0 || type != IRS3_AMF0_STRING) {
        return -1;
    }
    if (amf0_read_string_raw(reader, command->name, sizeof(command->name)) != 0) {
        return -1;
    }

    if (amf0_read_byte(reader, &type) != 0 || type != IRS3_AMF0_NUMBER) {
        return -1;
    }
    if (amf0_read_number_raw(reader, &command->transaction_id) != 0) {
        return -1;
    }

    if (reader->pos >= reader->len) {
        return 0;
    }

    if (amf0_read_byte(reader, &type) != 0) {
        return -1;
    }
    if (type == IRS3_AMF0_OBJECT) {
        amf0_read_object(reader, command);
    } else if (type == IRS3_AMF0_NULL) {
    } else {
        amf0_skip_value(reader);
    }

    if (strcmp(command->name, "publish") == 0 || strcmp(command->name, "FCPublish") == 0 || strcmp(command->name, "releaseStream") == 0) {
        if (reader->pos < reader->len) {
            if (amf0_read_byte(reader, &type) == 0 && type == IRS3_AMF0_STRING) {
                (void)amf0_read_string_raw(reader, command->stream_name, sizeof(command->stream_name));
            }
        }
        if (strcmp(command->name, "publish") == 0 && reader->pos < reader->len) {
            if (amf0_read_byte(reader, &type) == 0 && type == IRS3_AMF0_STRING) {
                (void)amf0_read_string_raw(reader, command->publish_type, sizeof(command->publish_type));
            }
        }
    }

    return 0;
}

size_t irs3_amf0_write_string(uint8_t *dst, const char *value) {
    size_t len = strlen(value);
    dst[0] = IRS3_AMF0_STRING;
    dst[1] = (uint8_t)((len >> 8) & 0xff);
    dst[2] = (uint8_t)(len & 0xff);
    memcpy(dst + 3, value, len);
    return 3 + len;
}

size_t irs3_amf0_write_number(uint8_t *dst, double value) {
    union {
        uint64_t u64;
        double d;
    } conv;
    conv.d = value;
    dst[0] = IRS3_AMF0_NUMBER;
    dst[1] = (uint8_t)((conv.u64 >> 56) & 0xff);
    dst[2] = (uint8_t)((conv.u64 >> 48) & 0xff);
    dst[3] = (uint8_t)((conv.u64 >> 40) & 0xff);
    dst[4] = (uint8_t)((conv.u64 >> 32) & 0xff);
    dst[5] = (uint8_t)((conv.u64 >> 24) & 0xff);
    dst[6] = (uint8_t)((conv.u64 >> 16) & 0xff);
    dst[7] = (uint8_t)((conv.u64 >> 8) & 0xff);
    dst[8] = (uint8_t)(conv.u64 & 0xff);
    return 9;
}

size_t irs3_amf0_write_boolean(uint8_t *dst, int value) {
    dst[0] = IRS3_AMF0_BOOLEAN;
    dst[1] = value ? 1 : 0;
    return 2;
}

size_t irs3_amf0_write_null(uint8_t *dst) {
    dst[0] = IRS3_AMF0_NULL;
    return 1;
}

size_t irs3_amf0_write_object_start(uint8_t *dst) {
    dst[0] = IRS3_AMF0_OBJECT;
    return 1;
}

static size_t write_named_prefix(uint8_t *dst, const char *name) {
    size_t len = strlen(name);
    dst[0] = (uint8_t)((len >> 8) & 0xff);
    dst[1] = (uint8_t)(len & 0xff);
    memcpy(dst + 2, name, len);
    return 2 + len;
}

size_t irs3_amf0_write_named_string(uint8_t *dst, const char *name, const char *value) {
    size_t offset = write_named_prefix(dst, name);
    size_t len = strlen(value);
    dst[offset++] = IRS3_AMF0_STRING;
    dst[offset++] = (uint8_t)((len >> 8) & 0xff);
    dst[offset++] = (uint8_t)(len & 0xff);
    memcpy(dst + offset, value, len);
    return offset + len;
}

size_t irs3_amf0_write_named_number(uint8_t *dst, const char *name, double value) {
    size_t offset = write_named_prefix(dst, name);
    union {
        uint64_t u64;
        double d;
    } conv;
    conv.d = value;
    dst[offset++] = IRS3_AMF0_NUMBER;
    dst[offset++] = (uint8_t)((conv.u64 >> 56) & 0xff);
    dst[offset++] = (uint8_t)((conv.u64 >> 48) & 0xff);
    dst[offset++] = (uint8_t)((conv.u64 >> 40) & 0xff);
    dst[offset++] = (uint8_t)((conv.u64 >> 32) & 0xff);
    dst[offset++] = (uint8_t)((conv.u64 >> 24) & 0xff);
    dst[offset++] = (uint8_t)((conv.u64 >> 16) & 0xff);
    dst[offset++] = (uint8_t)((conv.u64 >> 8) & 0xff);
    dst[offset++] = (uint8_t)(conv.u64 & 0xff);
    return offset;
}

size_t irs3_amf0_write_named_boolean(uint8_t *dst, const char *name, int value) {
    size_t offset = write_named_prefix(dst, name);
    dst[offset++] = IRS3_AMF0_BOOLEAN;
    dst[offset++] = value ? 1 : 0;
    return offset;
}

size_t irs3_amf0_write_object_end(uint8_t *dst) {
    dst[0] = 0x00;
    dst[1] = 0x00;
    dst[2] = IRS3_AMF0_OBJECT_END;
    return 3;
}
