/* @brief 官方向量、边界及坏字节流测试，验证 DGUS 拆包和重同步。 */
#include "dgus.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(condition) do { \
    if (!(condition)) { \
        fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #condition); \
        exit(EXIT_FAILURE); \
    } \
} while (0)

/* 独立参考：DWIN 官网 V2.921 §4.1 表，不由待测编码器生成。 */
static const uint8_t write_plain[] = {0x5a, 0xa5, 5, 0x82, 0x10, 0, 0x31, 0x32};
static const uint8_t write_crc[] = {0x5a, 0xa5, 7, 0x82, 0x10, 0, 0x31, 0x32, 0xcc, 0x9b};
static const uint8_t read_plain[] = {0x5a, 0xa5, 4, 0x83, 0, 0x0f, 1};
static const uint8_t read_crc[] = {0x5a, 0xa5, 6, 0x83, 0, 0x0f, 1, 0xed, 0x90};
static const uint8_t reply_plain[] = {0x5a, 0xa5, 6, 0x83, 0, 0x0f, 1, 0x14, 0x10};
static const uint8_t reply_crc[] = {0x5a, 0xa5, 8, 0x83, 0, 0x0f, 1, 0x14, 0x10, 0x43, 0xf0};
static const uint8_t ack_plain[] = {0x5a, 0xa5, 3, 0x82, 0x4f, 0x4b};
static const uint8_t ack_crc[] = {0x5a, 0xa5, 5, 0x82, 0x4f, 0x4b, 0xa5, 0xef};
static const uint8_t upload_plain[] = {0x5a, 0xa5, 6, 0x83, 0x10, 1, 1, 0, 0x5a};
static const uint8_t upload_crc[] = {0x5a, 0xa5, 8, 0x83, 0x10, 1, 1, 0, 0x5a, 0x0e, 0x2c};

struct received {
    size_t count;
    struct dgus_frame last;
};

static void receive(void *ctx, const struct dgus_frame *frame)
{
    struct received *received = ctx;

    CHECK(frame->count <= DGUS_MAX_WORDS);
    received->last = *frame;
    ++received->count;
}

static void expect_frame(enum dgus_crc mode, const uint8_t *bytes, size_t length,
                         enum dgus_kind kind, uint16_t vp, uint8_t count,
                         uint16_t first_word)
{
    struct dgus_frame frame;

    CHECK(dgus_decode(mode, bytes, length, &frame) == DGUS_OK);
    CHECK(frame.kind == kind && frame.vp == vp && frame.count == count);
    if (count != 0 && kind != DGUS_READ_REQUEST) {
        CHECK(frame.words[0] == first_word);
    }
}

static void test_golden(void)
{
    uint8_t out[DGUS_MAX_FRAME];
    uint16_t word = 0x3132;

    CHECK(dgus_encode_write(DGUS_CRC_NONE, 0x1000, &word, 1, out,
                             sizeof(out)) == sizeof(write_plain));
    CHECK(memcmp(out, write_plain, sizeof(write_plain)) == 0);
    CHECK(dgus_encode_read(DGUS_CRC_NONE, 0x000f, 1, out,
                            sizeof(out)) == sizeof(read_plain));
    CHECK(memcmp(out, read_plain, sizeof(read_plain)) == 0);
    expect_frame(DGUS_CRC_NONE, write_plain, sizeof(write_plain), DGUS_WRITE, 0x1000, 1, 0x3132);
    expect_frame(DGUS_CRC_NONE, read_plain, sizeof(read_plain), DGUS_READ_REQUEST, 0xf, 1, 0);
    expect_frame(DGUS_CRC_NONE, reply_plain, sizeof(reply_plain), DGUS_READ_DATA, 0xf, 1, 0x1410);
    expect_frame(DGUS_CRC_NONE, ack_plain, sizeof(ack_plain), DGUS_WRITE_ACK, 0, 0, 0);
    expect_frame(DGUS_CRC_NONE, upload_plain, sizeof(upload_plain), DGUS_READ_DATA, 0x1001, 1, 0x5a);
}

static void test_crc_vectors(void)
{
    uint8_t out[DGUS_MAX_FRAME];
    uint16_t word = 0x3132;

    CHECK(dgus_encode_write(DGUS_CRC_MODBUS, 0x1000, &word, 1, out,
                             sizeof(out)) == sizeof(write_crc));
    CHECK(memcmp(out, write_crc, sizeof(write_crc)) == 0);
    CHECK(dgus_encode_read(DGUS_CRC_MODBUS, 0x000f, 1, out,
                            sizeof(out)) == sizeof(read_crc));
    CHECK(memcmp(out, read_crc, sizeof(read_crc)) == 0);
    expect_frame(DGUS_CRC_MODBUS, write_crc, sizeof(write_crc), DGUS_WRITE, 0x1000, 1, 0x3132);
    expect_frame(DGUS_CRC_MODBUS, read_crc, sizeof(read_crc), DGUS_READ_REQUEST, 0xf, 1, 0);
    expect_frame(DGUS_CRC_MODBUS, reply_crc, sizeof(reply_crc), DGUS_READ_DATA, 0xf, 1, 0x1410);
    expect_frame(DGUS_CRC_MODBUS, ack_crc, sizeof(ack_crc), DGUS_WRITE_ACK, 0, 0, 0);
    expect_frame(DGUS_CRC_MODBUS, upload_crc, sizeof(upload_crc), DGUS_READ_DATA, 0x1001, 1, 0x5a);

    memcpy(out, reply_crc, sizeof(reply_crc));
    out[9] = 0xf0;
    out[10] = 0x43;
    {
        struct dgus_frame frame;
        CHECK(dgus_decode(DGUS_CRC_MODBUS, out, sizeof(reply_crc), &frame) == DGUS_ERR_CRC);
    }
}

static void test_boundaries(void)
{
    uint8_t out[DGUS_MAX_FRAME];
    uint8_t original[DGUS_MAX_FRAME];
    uint16_t words[DGUS_MAX_WORDS];
    struct dgus_frame frame;
    struct dgus_parser parser = {0};
    struct received received = {0};

    for (size_t i = 0; i < DGUS_MAX_WORDS; ++i) {
        words[i] = (uint16_t)(0x1234u + i);
    }
    memset(out, 0x77, sizeof(out));
    memcpy(original, out, sizeof(out));
    CHECK(dgus_encode_write(DGUS_CRC_UNCONFIGURED, 0x1000, words, 1, out, sizeof(out)) == DGUS_ERR_MODE);
    CHECK(dgus_encode_read((enum dgus_crc)99, 0x1000, 1, out, sizeof(out)) == DGUS_ERR_MODE);
    CHECK(dgus_encode_write(DGUS_CRC_NONE, 0x1000, NULL, 1, out, sizeof(out)) == DGUS_ERR_ARG);
    CHECK(dgus_encode_write(DGUS_CRC_NONE, 0x1000, words, 1, NULL, sizeof(out)) == DGUS_ERR_ARG);
    CHECK(dgus_encode_read(DGUS_CRC_NONE, 0x1000, 1, NULL, sizeof(out)) == DGUS_ERR_ARG);
    CHECK(dgus_encode_write(DGUS_CRC_NONE, 0, words, 0, out, sizeof(out)) == DGUS_ERR_SIZE);
    CHECK(dgus_encode_read(DGUS_CRC_NONE, 0, 0, out, sizeof(out)) == DGUS_ERR_SIZE);
    CHECK(dgus_encode_write(DGUS_CRC_NONE, 0, words, SIZE_MAX, out, sizeof(out)) == DGUS_ERR_SIZE);
    CHECK(dgus_encode_read(DGUS_CRC_NONE, 0, DGUS_MAX_WORDS + 1, out, sizeof(out)) == DGUS_ERR_SIZE);
    CHECK(dgus_encode_write(DGUS_CRC_NONE, 0xffff, words, 2, out, sizeof(out)) == DGUS_ERR_SIZE);
    CHECK(dgus_encode_read(DGUS_CRC_NONE, 0xffff, 2, out, sizeof(out)) == DGUS_ERR_SIZE);
    CHECK(dgus_encode_write(DGUS_CRC_NONE, 0, words, 1, out, 7) == DGUS_ERR_SIZE);
    CHECK(dgus_encode_read(DGUS_CRC_MODBUS, 0, 1, out, 8) == DGUS_ERR_SIZE);
    CHECK(memcmp(out, original, sizeof(out)) == 0);

    for (int mode = DGUS_CRC_NONE; mode <= DGUS_CRC_MODBUS; ++mode) {
        int length = dgus_encode_write((enum dgus_crc)mode, 0xff88, words,
                                       DGUS_MAX_WORDS, out, sizeof(out));
        CHECK(length > 0 && (size_t)length <= DGUS_MAX_FRAME);
        CHECK(dgus_decode((enum dgus_crc)mode, out, (size_t)length, &frame) == DGUS_OK);
        CHECK(frame.vp == 0xff88 && frame.count == DGUS_MAX_WORDS);
        CHECK(memcmp(frame.words, words, sizeof(words)) == 0);
        CHECK(dgus_encode_read((enum dgus_crc)mode, 0xffff, 1, out, sizeof(out)) > 0);
    }

    CHECK(dgus_feed(&parser, ack_plain, sizeof(ack_plain)) == DGUS_ERR_MODE);
    CHECK(dgus_init(&parser, DGUS_CRC_UNCONFIGURED, receive, &received) == DGUS_ERR_MODE);
    CHECK(dgus_init(&parser, DGUS_CRC_NONE, NULL, &received) == DGUS_ERR_ARG);
    CHECK(dgus_init(NULL, DGUS_CRC_NONE, receive, &received) == DGUS_ERR_ARG);
    CHECK(dgus_feed(NULL, ack_plain, sizeof(ack_plain)) == DGUS_ERR_ARG);
    CHECK(dgus_init(&parser, DGUS_CRC_NONE, receive, &received) == DGUS_OK);
    CHECK(dgus_feed(&parser, NULL, 0) == DGUS_OK);
    CHECK(dgus_feed(&parser, NULL, 1) == DGUS_ERR_ARG);
    CHECK(dgus_decode(DGUS_CRC_UNCONFIGURED, ack_plain, sizeof(ack_plain), &frame) == DGUS_ERR_MODE);
    CHECK(dgus_decode(DGUS_CRC_NONE, NULL, 0, &frame) == DGUS_ERR_ARG);
    CHECK(dgus_decode(DGUS_CRC_NONE, ack_plain, sizeof(ack_plain), NULL) == DGUS_ERR_ARG);
    dgus_reset(NULL);
}

static void test_malformed(void)
{
    static const uint8_t bad[][12] = {
        {0x5a, 0xa5, 3, 0x82, 0x4f, 0x00},
        {0x5a, 0xa5, 3, 0x99, 0x4f, 0x4b},
        {0x5a, 0xa5, 4, 0x83, 0x10, 0, 0},
        {0x5a, 0xa5, 6, 0x83, 0x10, 0, 2, 0, 2},
        {0x5a, 0xa5, 6, 0x82, 0x10, 0, 1, 2, 3},
        {0x5a, 0xa5, 4, 0x83, 0xff, 0xff, 2}
    };
    static const size_t lengths[] = {6, 6, 7, 9, 9, 7};
    struct dgus_frame output;
    unsigned char original[sizeof(output)];

    memset(&output, 0x5c, sizeof(output));
    memcpy(original, &output, sizeof(output));
    for (size_t i = 0; i < sizeof(lengths) / sizeof(lengths[0]); ++i) {
        CHECK(dgus_decode(DGUS_CRC_NONE, bad[i], lengths[i], &output) < 0);
        CHECK(memcmp(&output, original, sizeof(output)) == 0);
    }
    for (size_t i = 0; i < sizeof(reply_crc); ++i) {
        CHECK(dgus_decode(DGUS_CRC_MODBUS, reply_crc, i, &output) < 0);
    }
    {
        uint8_t oversized[DGUS_MAX_FRAME + 1] = {0};
        CHECK(dgus_decode(DGUS_CRC_NONE, oversized, sizeof(oversized), &output) == DGUS_ERR_SIZE);
    }
}

static void check_splits(enum dgus_crc mode, const uint8_t *first, size_t first_size,
                         const uint8_t *second, size_t second_size)
{
    uint8_t joined[DGUS_MAX_FRAME * 2];

    memcpy(joined, first, first_size);
    memcpy(joined + first_size, second, second_size);
    for (size_t split = 0; split <= first_size + second_size; ++split) {
        struct received received = {0};
        struct dgus_parser parser;

        CHECK(dgus_init(&parser, mode, receive, &received) == DGUS_OK);
        CHECK(dgus_feed(&parser, joined, split) == DGUS_OK);
        CHECK(dgus_feed(&parser, joined + split, first_size + second_size - split) == DGUS_OK);
        CHECK(received.count == 2 && parser.used == 0 && parser.rejected == 0);
        CHECK(received.last.kind == DGUS_READ_DATA && received.last.words[0] == 0x1410);
    }
}

static void test_split_and_join(void)
{
    struct dgus_parser parser;
    struct received received = {0};
    uint8_t out[DGUS_MAX_FRAME];
    const uint16_t words[] = {0x5aa5, 0x0382, 0x4f4b};
    int length;

    check_splits(DGUS_CRC_NONE, ack_plain, sizeof(ack_plain), reply_plain, sizeof(reply_plain));
    check_splits(DGUS_CRC_MODBUS, ack_crc, sizeof(ack_crc), reply_crc, sizeof(reply_crc));
    CHECK(dgus_init(&parser, DGUS_CRC_MODBUS, receive, &received) == DGUS_OK);
    length = dgus_encode_write(DGUS_CRC_MODBUS, 0x1000, words, 3, out, sizeof(out));
    CHECK(length > 0);
    for (int i = 0; i < length; ++i) {
        CHECK(dgus_feed(&parser, out + i, 1) == DGUS_OK);
    }
    CHECK(received.count == 1 && received.last.kind == DGUS_WRITE);
    CHECK(memcmp(received.last.words, words, sizeof(words)) == 0);
}

static void test_resync(void)
{
    struct dgus_parser parser;
    struct received received = {0};
    static const uint8_t noise[] = {0, 0xa5, 0x5a, 0x5a, 0xa5, 0, 0x5a, 0xa5, 0xff,
                                     0x5a, 0xa5, 3, 0x99};
    static const uint8_t overlap[] = {0x5a, 0xa5, 3, 0x82, 0x5a, 0xa5, 3, 0x82, 0x4f, 0x4b};
    static const uint8_t bad_count[] = {0x5a, 0xa5, 244, 0x83, 0x10, 0, 1};

    CHECK(dgus_init(&parser, DGUS_CRC_NONE, receive, &received) == DGUS_OK);
    CHECK(dgus_feed(&parser, noise, sizeof(noise)) == DGUS_OK);
    CHECK(dgus_feed(&parser, ack_plain, sizeof(ack_plain)) == DGUS_OK);
    CHECK(received.count == 1 && parser.rejected >= 3);
    CHECK(dgus_feed(&parser, overlap, sizeof(overlap)) == DGUS_OK);
    CHECK(received.count == 2);
    CHECK(dgus_feed(&parser, bad_count, sizeof(bad_count)) == DGUS_OK);
    CHECK(dgus_feed(&parser, reply_plain, sizeof(reply_plain)) == DGUS_OK);
    CHECK(received.count == 3 && received.last.words[0] == 0x1410);
}

static void test_timeout(void)
{
    struct dgus_parser parser;
    struct received received = {0};
    static const uint8_t partial[] = {0x5a, 0xa5, 243, 0x82, 0x10, 0, 0x12};

    CHECK(dgus_init(&parser, DGUS_CRC_NONE, receive, &received) == DGUS_OK);
    CHECK(dgus_feed(&parser, partial, sizeof(partial)) == DGUS_OK);
    CHECK(parser.used == sizeof(partial) && received.count == 0);
    dgus_reset(&parser);
    CHECK(parser.used == 0 && parser.mode == DGUS_CRC_NONE);
    CHECK(dgus_feed(&parser, ack_plain, sizeof(ack_plain)) == DGUS_OK);
    CHECK(received.count == 1);
    CHECK(dgus_init(&parser, DGUS_CRC_UNCONFIGURED, receive, &received) == DGUS_ERR_MODE);
    CHECK(dgus_feed(&parser, ack_plain, sizeof(ack_plain)) == DGUS_ERR_MODE);
}

static void test_corruption(void)
{
    /* 每个 command/data/CRC 位翻转一次，CRC 模式都必须拒绝。 */
    for (size_t byte = 3; byte < sizeof(reply_crc); ++byte) {
        for (unsigned int bit = 0; bit < 8; ++bit) {
            struct dgus_parser parser;
            struct received received = {0};
            struct dgus_frame frame;
            uint8_t broken[sizeof(reply_crc)];

            memcpy(broken, reply_crc, sizeof(broken));
            broken[byte] ^= (uint8_t)(1u << bit);
            CHECK(dgus_decode(DGUS_CRC_MODBUS, broken, sizeof(broken), &frame) < 0);
            CHECK(dgus_init(&parser, DGUS_CRC_MODBUS, receive, &received) == DGUS_OK);
            CHECK(dgus_feed(&parser, broken, sizeof(broken)) == DGUS_OK);
            CHECK(received.count == 0);
            CHECK(dgus_feed(&parser, ack_crc, sizeof(ack_crc)) == DGUS_OK);
            CHECK(received.count == 1 && received.last.kind == DGUS_WRITE_ACK);
        }
    }
}

static void test_bounded_noise(void)
{
    struct guarded {
        uint32_t before;
        struct dgus_parser parser;
        uint32_t after;
    } guarded;
    struct received received = {0};
    uint32_t random = 0x13579bdf;

    guarded.before = 0x12345678;
    guarded.after = 0x89abcdef;
    CHECK(dgus_init(&guarded.parser, DGUS_CRC_MODBUS, receive, &received) == DGUS_OK);
    for (size_t i = 0; i < 100000; ++i) {
        uint8_t byte;

        random = random * 1664525u + 1013904223u;
        byte = (uint8_t)(random >> 24);
        CHECK(dgus_feed(&guarded.parser, &byte, 1) == DGUS_OK);
        CHECK(guarded.parser.used < DGUS_MAX_FRAME);
        CHECK(guarded.before == 0x12345678 && guarded.after == 0x89abcdef);
        if (i % 101 == 0) {
            dgus_reset(&guarded.parser);
        }
    }
    dgus_reset(&guarded.parser);
    {
        size_t count = received.count;
        CHECK(dgus_feed(&guarded.parser, ack_crc, sizeof(ack_crc)) == DGUS_OK);
        CHECK(received.count == count + 1);
    }
}

int main(int argc, char **argv)
{
    static const struct {
        const char *name;
        void (*run)(void);
    } cases[] = {
        {"golden", test_golden},
        {"crc_vectors", test_crc_vectors},
        {"boundaries", test_boundaries},
        {"malformed", test_malformed},
        {"split_and_join", test_split_and_join},
        {"resync", test_resync},
        {"timeout", test_timeout},
        {"corruption", test_corruption},
        {"bounded_noise", test_bounded_noise}
    };
    size_t ran = 0;

    CHECK(argc <= 2);
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        if (argc == 1 || strcmp(argv[1], cases[i].name) == 0) {
            cases[i].run();
            printf("PASS %s\n", cases[i].name);
            ++ran;
        }
    }
    CHECK(ran != 0);
    return EXIT_SUCCESS;
}
