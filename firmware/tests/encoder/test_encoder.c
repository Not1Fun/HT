/* @brief 验证正交整格、回摆抵消、非法转移、方向配置和实例隔离。 */
#include "encoder.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(condition) do { \
    if (!(condition)) { \
        fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #condition); \
        exit(EXIT_FAILURE); \
    } \
} while (0)

static struct encoder initialize(uint8_t count, bool reversed, uint8_t ab)
{
    struct encoder encoder;
    const struct encoder_config config = {count, reversed};

    CHECK(encoder_init(&encoder, &config, ab) == ENCODER_OK);
    CHECK(encoder.ready && encoder.synced && encoder.previous == ab && encoder.partial == 0);
    return encoder;
}

static int8_t update(struct encoder *encoder, uint8_t ab)
{
    int8_t detents = 99;

    CHECK(encoder_update(encoder, ab, &detents) == ENCODER_OK);
    return detents;
}

static void test_directions(void)
{
    static const uint8_t forward[] = {1, 3, 2, 0};
    static const uint8_t backward[] = {2, 3, 1, 0};

    for (uint8_t count = 2; count <= 4; count += 2) {
        for (int reversed = 0; reversed <= 1; ++reversed) {
            struct encoder encoder = initialize(count, reversed != 0, 0);

            for (int cycle = 0; cycle < 3; ++cycle) {
                for (size_t i = 0; i < 4; ++i) {
                    int8_t expected = (i + 1) % count == 0 ? (reversed ? -1 : 1) : 0;

                    CHECK(update(&encoder, forward[i]) == expected);
                }
                for (size_t i = 0; i < 4; ++i) {
                    int8_t expected = (i + 1) % count == 0 ? (reversed ? 1 : -1) : 0;

                    CHECK(update(&encoder, backward[i]) == expected);
                }
            }
        }
    }
}

static void test_initial_states(void)
{
    static const uint8_t states[] = {0, 1, 3, 2};

    for (size_t start = 0; start < 4; ++start) {
        struct encoder encoder = initialize(4, false, states[start]);

        CHECK(update(&encoder, states[start]) == 0);
        for (size_t offset = 1; offset <= 4; ++offset) {
            CHECK(update(&encoder, states[(start + offset) % 4]) == (offset == 4 ? 1 : 0));
        }
    }
}

static void test_partials(void)
{
    struct encoder encoder = initialize(4, false, 0);

    CHECK(update(&encoder, 1) == 0 && encoder.partial == 1);
    CHECK(update(&encoder, 3) == 0 && encoder.partial == 2);
    CHECK(update(&encoder, 2) == 0 && encoder.partial == 3);
    for (int i = 0; i < 10; ++i) {
        CHECK(update(&encoder, 2) == 0 && encoder.partial == 3);
    }
    CHECK(update(&encoder, 0) == 1 && encoder.partial == 0);
}

static void test_bounce(void)
{
    for (uint8_t count = 2; count <= 4; count += 2) {
        struct encoder encoder = initialize(count, false, 0);

        for (int i = 0; i < 50; ++i) {
            CHECK(update(&encoder, 1) == 0);
            CHECK(update(&encoder, 1) == 0);
            CHECK(update(&encoder, 0) == 0 && encoder.partial == 0);
            CHECK(update(&encoder, 2) == 0);
            CHECK(update(&encoder, 0) == 0 && encoder.partial == 0);
        }
    }
    struct encoder encoder = initialize(4, false, 0);

    CHECK(update(&encoder, 1) == 0);
    CHECK(update(&encoder, 3) == 0);
    CHECK(update(&encoder, 2) == 0);
    CHECK(update(&encoder, 3) == 0);
    CHECK(update(&encoder, 1) == 0);
    CHECK(update(&encoder, 0) == 0 && encoder.partial == 0);
}

static void test_reversal(void)
{
    struct encoder encoder = initialize(4, false, 0);

    CHECK(update(&encoder, 1) == 0);
    CHECK(update(&encoder, 0) == 0);
    CHECK(update(&encoder, 2) == 0);
    CHECK(update(&encoder, 3) == 0);
    CHECK(update(&encoder, 1) == 0);
    CHECK(update(&encoder, 0) == -1);
    CHECK(encoder.partial == 0);
    CHECK(update(&encoder, 1) == 0);
    CHECK(update(&encoder, 3) == 0);
    CHECK(update(&encoder, 2) == 0);
    CHECK(update(&encoder, 0) == 1);
}

static void test_invalid_transition(void)
{
    static const uint8_t states[] = {0, 1, 3, 2};

    for (size_t start = 0; start < 4; ++start) {
        struct encoder encoder = initialize(4, false, states[start]);
        uint8_t previous = states[(start + 1) % 4];
        uint8_t invalid = previous ^ 3u;

        CHECK(update(&encoder, previous) == 0 && encoder.partial == 1);
        CHECK(update(&encoder, invalid) == 0);
        CHECK(encoder.previous == invalid && encoder.partial == 0 && encoder.synced);
    }
    for (uint8_t count = 2; count <= 4; count += 2) {
        struct encoder encoder = initialize(count, false, 0);

        CHECK(update(&encoder, 1) == 0);
        CHECK(update(&encoder, 2) == 0 && encoder.partial == 0);
        CHECK(update(&encoder, 0) == 0);
        CHECK(update(&encoder, 1) == (count == 2 ? 1 : 0));
        CHECK(update(&encoder, 3) == 0);
        CHECK(update(&encoder, 2) == 1);
    }
}

static void test_resync(void)
{
    struct encoder encoder = initialize(4, false, 0);
    int8_t detents = 99;

    CHECK(update(&encoder, 1) == 0);
    CHECK(encoder_update(&encoder, 4, &detents) == ENCODER_ERR_ARG);
    CHECK(detents == 0 && !encoder.synced && encoder.partial == 0);
    CHECK(encoder_update(&encoder, UINT8_MAX, &detents) == ENCODER_ERR_ARG);
    CHECK(update(&encoder, 3) == 0 && encoder.synced && encoder.partial == 0);
    CHECK(update(&encoder, 2) == 0);
    CHECK(update(&encoder, 0) == 0);
    CHECK(update(&encoder, 1) == 0);
    CHECK(update(&encoder, 3) == 1);
}

static void test_instances(void)
{
    struct encoder first = initialize(4, false, 0);
    struct encoder second = initialize(2, true, 0);

    CHECK(update(&first, 1) == 0);
    CHECK(update(&second, 1) == 0);
    CHECK(update(&first, 3) == 0);
    CHECK(update(&second, 3) == -1);
    CHECK(first.partial == 2 && second.partial == 0);
    CHECK(update(&first, 2) == 0);
    CHECK(update(&second, 1) == 0);
    CHECK(update(&first, 0) == 1);
    CHECK(update(&second, 0) == 1);
}

static void test_invalid_args(void)
{
    static const uint8_t invalid_counts[] = {0, 1, 3, 5, UINT8_MAX};
    struct encoder encoder = {0};
    struct encoder_config config = {4, false};
    int8_t detents = 99;

    CHECK(encoder_update(&encoder, 0, &detents) == ENCODER_ERR_NOT_READY && detents == 0);
    CHECK(encoder_update(NULL, 0, &detents) == ENCODER_ERR_ARG && detents == 0);
    CHECK(encoder_update(&encoder, 0, NULL) == ENCODER_ERR_ARG);
    CHECK(encoder_init(NULL, &config, 0) == ENCODER_ERR_ARG);
    for (size_t i = 0; i < sizeof(invalid_counts); ++i) {
        encoder = initialize(4, false, 0);
        config.transitions_per_detent = invalid_counts[i];
        CHECK(encoder_init(&encoder, &config, 0) == ENCODER_ERR_ARG);
        CHECK(!encoder.ready && !encoder.synced);
        CHECK(encoder_update(&encoder, 0, &detents) == ENCODER_ERR_NOT_READY);
    }
    config.transitions_per_detent = 4;
    CHECK(encoder_init(&encoder, &config, 4) == ENCODER_ERR_ARG && !encoder.ready);
    CHECK(encoder_init(&encoder, NULL, 0) == ENCODER_ERR_ARG && !encoder.ready);
    encoder = initialize(4, false, 0);
    CHECK(encoder_init(&encoder, &encoder.config, 0) == ENCODER_OK);
    CHECK(encoder.config.transitions_per_detent == 4);
}

int main(int argc, char **argv)
{
    static const struct {
        const char *name;
        void (*run)(void);
    } cases[] = {
        {"directions", test_directions}, {"initial_states", test_initial_states},
        {"partials", test_partials}, {"bounce", test_bounce}, {"reversal", test_reversal},
        {"invalid_transition", test_invalid_transition}, {"resync", test_resync},
        {"instances", test_instances}, {"invalid_args", test_invalid_args}
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
