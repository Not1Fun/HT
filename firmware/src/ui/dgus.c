/* @brief DGUS II 82/83 帧与 Modbus CRC，固定内存解析并在坏帧后重同步。 */
#include "dgus.h"

#include <string.h>

/* DWIN T5L DGUSII Guide V2.921 §4.1/4.2，PDF 45/47 页。
 * CRC 仅覆盖指令与数据，初值 FFFF，反射多项式 A001，低字节先传。
 */
static int crc_bytes(enum dgus_crc mode)
{
    if (mode == DGUS_CRC_NONE) {
        return 0;
    }
    if (mode == DGUS_CRC_MODBUS) {
        return 2;
    }
    return DGUS_ERR_MODE;
}

static uint16_t crc16(const uint8_t *data, size_t length)
{
    uint16_t crc = 0xffff;

    for (size_t i = 0; i < length; ++i) {
        crc ^= data[i];
        for (unsigned int bit = 0; bit < 8; ++bit) {
            crc = (uint16_t)((crc >> 1) ^ ((crc & 1u) ? 0xa001u : 0u));
        }
    }
    return crc;
}

static uint16_t get_word(const uint8_t *data)
{
    return (uint16_t)(((uint16_t)data[0] << 8) | data[1]);
}

static void put_word(uint8_t *data, uint16_t word)
{
    data[0] = (uint8_t)(word >> 8);
    data[1] = (uint8_t)word;
}

static int valid_range(uint16_t vp, size_t count)
{
    return count > 0 && count <= DGUS_MAX_WORDS &&
           count <= 0x10000u - (uint32_t)vp;
}

static int finish_frame(enum dgus_crc mode, uint8_t *out, size_t body)
{
    size_t length = body + (mode == DGUS_CRC_MODBUS ? 2u : 0u);

    out[0] = 0x5a;
    out[1] = 0xa5;
    out[2] = (uint8_t)length;
    if (mode == DGUS_CRC_MODBUS) {
        uint16_t crc = crc16(out + 3, body);

        out[3 + body] = (uint8_t)crc;
        out[4 + body] = (uint8_t)(crc >> 8);
    }
    return (int)(length + 3);
}

int dgus_encode_write(enum dgus_crc mode, uint16_t vp, const uint16_t *words,
                      size_t count, uint8_t *out, size_t capacity)
{
    int extra = crc_bytes(mode);
    size_t body;

    if (extra < 0) {
        return extra;
    }
    if (out == NULL || words == NULL) {
        return DGUS_ERR_ARG;
    }
    if (!valid_range(vp, count)) {
        return DGUS_ERR_SIZE;
    }
    body = 3 + count * 2;
    if (capacity < body + 3 + (size_t)extra) {
        return DGUS_ERR_SIZE;
    }
    out[3] = 0x82;
    put_word(out + 4, vp);
    for (size_t i = 0; i < count; ++i) {
        put_word(out + 6 + i * 2, words[i]);
    }
    return finish_frame(mode, out, body);
}

int dgus_encode_read(enum dgus_crc mode, uint16_t vp, size_t count,
                     uint8_t *out, size_t capacity)
{
    int extra = crc_bytes(mode);

    if (extra < 0) {
        return extra;
    }
    if (out == NULL) {
        return DGUS_ERR_ARG;
    }
    if (!valid_range(vp, count) || capacity < 7 + (size_t)extra) {
        return DGUS_ERR_SIZE;
    }
    out[3] = 0x83;
    put_word(out + 4, vp);
    out[6] = (uint8_t)count;
    return finish_frame(mode, out, 4);
}

static int valid_shape(uint8_t command, size_t body)
{
    if (command == 0x82) {
        return body == 3 || (body >= 5 && body <= 3 + 2 * DGUS_MAX_WORDS &&
                             (body & 1u) != 0);
    }
    if (command == 0x83) {
        return body == 4 || (body >= 6 && body <= 4 + 2 * DGUS_MAX_WORDS &&
                             (body & 1u) == 0);
    }
    return 0;
}

int dgus_decode(enum dgus_crc mode, const uint8_t *data, size_t length,
                struct dgus_frame *frame)
{
    struct dgus_frame decoded = {0};
    int extra = crc_bytes(mode);
    size_t body;
    size_t word_offset;

    if (extra < 0) {
        return extra;
    }
    if (data == NULL || frame == NULL) {
        return DGUS_ERR_ARG;
    }
    if (length < 6 + (size_t)extra || length > DGUS_MAX_FRAME) {
        return DGUS_ERR_SIZE;
    }
    if (data[0] != 0x5a || data[1] != 0xa5 || data[2] != length - 3) {
        return DGUS_ERR_FRAME;
    }
    body = length - 3 - (size_t)extra;
    if (!valid_shape(data[3], body)) {
        return DGUS_ERR_FRAME;
    }
    if (extra != 0) {
        uint16_t received = (uint16_t)(data[length - 2] |
                                       ((uint16_t)data[length - 1] << 8));

        if (crc16(data + 3, body) != received) {
            return DGUS_ERR_CRC;
        }
    }

    if (data[3] == 0x82 && body == 3) {
        if (data[4] != 0x4f || data[5] != 0x4b) {
            return DGUS_ERR_FRAME;
        }
        decoded.kind = DGUS_WRITE_ACK;
    } else {
        decoded.vp = get_word(data + 4);
        if (data[3] == 0x82) {
            decoded.kind = DGUS_WRITE;
            decoded.count = (uint8_t)((body - 3) / 2);
            word_offset = 6;
        } else {
            decoded.count = data[6];
            decoded.kind = body == 4 ? DGUS_READ_REQUEST : DGUS_READ_DATA;
            word_offset = 7;
            if (body != 4 && body != 4 + 2u * decoded.count) {
                return DGUS_ERR_FRAME;
            }
        }
        if (!valid_range(decoded.vp, decoded.count)) {
            return DGUS_ERR_SIZE;
        }
        if (decoded.kind != DGUS_READ_REQUEST) {
            for (size_t i = 0; i < decoded.count; ++i) {
                decoded.words[i] = get_word(data + word_offset + i * 2);
            }
        }
    }
    *frame = decoded;
    return DGUS_OK;
}

int dgus_init(struct dgus_parser *parser, enum dgus_crc mode,
               dgus_receive_fn receive, void *ctx)
{
    if (parser == NULL) {
        return DGUS_ERR_ARG;
    }
    memset(parser, 0, sizeof(*parser));
    if (crc_bytes(mode) < 0) {
        return DGUS_ERR_MODE;
    }
    if (receive == NULL) {
        return DGUS_ERR_ARG;
    }
    parser->mode = mode;
    parser->receive = receive;
    parser->ctx = ctx;
    return DGUS_OK;
}

static void consume(struct dgus_parser *parser, size_t count)
{
    parser->used -= count;
    memmove(parser->buffer, parser->buffer + count, parser->used);
}

static void reject(struct dgus_parser *parser)
{
    if (parser->rejected != UINT32_MAX) {
        ++parser->rejected;
    }
    consume(parser, 1);
}

static void parse_buffer(struct dgus_parser *parser)
{
    size_t extra = (size_t)crc_bytes(parser->mode);

    while (parser->used != 0) {
        size_t length;
        size_t body;
        struct dgus_frame frame;

        if (parser->buffer[0] != 0x5a) {
            consume(parser, 1);
            continue;
        }
        if (parser->used < 2) {
            return;
        }
        if (parser->buffer[1] != 0xa5) {
            consume(parser, 1);
            continue;
        }
        if (parser->used < 3) {
            return;
        }
        length = (size_t)parser->buffer[2] + 3;
        if (length < 6 + extra || length > DGUS_MAX_FRAME) {
            reject(parser);
            continue;
        }
        body = length - 3 - extra;
        if (parser->used >= 4 && !valid_shape(parser->buffer[3], body)) {
            reject(parser);
            continue;
        }
        /* 83 的字数已到齐时提前检查，避免错误长度长期吞住后续帧。 */
        if (parser->used >= 7 && parser->buffer[3] == 0x83 &&
            (!valid_range(get_word(parser->buffer + 4), parser->buffer[6]) ||
             (body != 4 && body != 4 + 2u * parser->buffer[6]))) {
            reject(parser);
            continue;
        }
        if (parser->used < length) {
            return;
        }
        if (dgus_decode(parser->mode, parser->buffer, length, &frame) != DGUS_OK) {
            reject(parser);
            continue;
        }
        consume(parser, length);
        parser->receive(parser->ctx, &frame);
    }
}

int dgus_feed(struct dgus_parser *parser, const uint8_t *data, size_t length)
{
    if (parser == NULL || (data == NULL && length != 0)) {
        return DGUS_ERR_ARG;
    }
    if (crc_bytes(parser->mode) < 0) {
        return DGUS_ERR_MODE;
    }
    if (parser->receive == NULL || parser->used >= DGUS_MAX_FRAME) {
        return DGUS_ERR_ARG;
    }
    for (size_t i = 0; i < length; ++i) {
        parser->buffer[parser->used++] = data[i];
        parse_buffer(parser);
    }
    return DGUS_OK;
}

void dgus_reset(struct dgus_parser *parser)
{
    if (parser != NULL) {
        parser->used = 0;
    }
}
