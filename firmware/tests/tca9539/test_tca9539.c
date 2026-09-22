/* @brief 注入总线错误和寄存器错值，检查安全顺序与故障锁定。 */
#include "tca9539_safe.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(condition) do { \
    if (!(condition)) { \
        fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #condition); \
        exit(EXIT_FAILURE); \
    } \
} while (0)

struct transfer {
    char kind;
    uint8_t addr;
    uint8_t reg;
    uint8_t value;
};

struct bus {
    uint8_t regs[8];
    struct transfer transfers[32];
    size_t count;
    size_t fail_at;
    size_t mismatch_at;
    int failure;
    bool initializing;
    struct tca9539 *dev;
};

static const struct transfer init_transfers[] = {
    {'W', 0x74, 0x02, 0x00}, {'R', 0x74, 0x02, 0x00},
    {'W', 0x74, 0x07, 0xff}, {'R', 0x74, 0x07, 0xff},
    {'W', 0x74, 0x05, 0x00}, {'R', 0x74, 0x05, 0x00},
    {'W', 0x74, 0x06, 0x00}, {'R', 0x74, 0x06, 0x00}
};

static void record(struct bus *bus, char kind, uint8_t addr,
                   uint8_t reg, uint8_t value)
{
    CHECK(reg < sizeof(bus->regs));
    CHECK(bus->count < sizeof(bus->transfers) / sizeof(bus->transfers[0]));
    if (bus->initializing) {
        CHECK(!bus->dev->ready);
    }
    bus->transfers[bus->count++] = (struct transfer){kind, addr, reg, value};
}

static int read_reg(void *ctx, uint8_t addr, uint8_t reg, uint8_t *value)
{
    struct bus *bus = ctx;

    CHECK(reg < sizeof(bus->regs));
    record(bus, 'R', addr, reg, bus->regs[reg]);
    *value = bus->regs[reg];
    if (bus->count == bus->fail_at) {
        *value = 0xde;
        return bus->failure;
    }
    if (bus->count == bus->mismatch_at) {
        *value ^= 1;
    }
    return 0;
}

static int write_reg(void *ctx, uint8_t addr, uint8_t reg, uint8_t value)
{
    struct bus *bus = ctx;

    record(bus, 'W', addr, reg, value);
    if (bus->count == bus->fail_at) {
        return bus->failure;
    }
    bus->regs[reg] = value;
    return 0;
}

static void setup(struct bus *bus, struct tca9539 *dev)
{
    memset(bus, 0, sizeof(*bus));
    memset(dev, 0, sizeof(*dev));
    bus->regs[0x01] = 0xa5;
    bus->regs[0x02] = 0xff;
    bus->regs[0x03] = 0xff;
    bus->regs[0x05] = 0xff;
    bus->regs[0x06] = 0xff;
    bus->regs[0x07] = 0xff;
    bus->failure = -99;
    bus->dev = dev;
    dev->ctx = bus;
    dev->addr = 0x74;
    dev->read_reg = read_reg;
    dev->write_reg = write_reg;
}

static void clear_transfers(struct bus *bus)
{
    bus->count = 0;
    bus->fail_at = 0;
    bus->mismatch_at = 0;
}

static int initialize(struct bus *bus, struct tca9539 *dev)
{
    int result;

    bus->initializing = true;
    result = tca9539_init(dev);
    bus->initializing = false;
    return result;
}

static void check_locked(struct bus *bus, struct tca9539 *dev)
{
    size_t count = bus->count;
    uint8_t panel = 0x5a;

    CHECK(!dev->ready);
    CHECK(tca9539_read_panel(dev, &panel) == TCA9539_ERR_NOT_READY);
    CHECK(panel == 0x5a);
    CHECK(tca9539_verify_off(dev) == TCA9539_ERR_NOT_READY);
    CHECK(bus->count == count);
}

static void check_recovery(struct bus *bus, struct tca9539 *dev)
{
    clear_transfers(bus);
    CHECK(initialize(bus, dev) == TCA9539_OK);
    CHECK(dev->ready);
    CHECK(tca9539_verify_off(dev) == TCA9539_OK);
}

static void test_init_order(void)
{
    struct bus bus;
    struct tca9539 dev;

    setup(&bus, &dev);
    check_locked(&bus, &dev);
    CHECK(initialize(&bus, &dev) == TCA9539_OK);
    CHECK(dev.ready);
    CHECK(bus.count == sizeof(init_transfers) / sizeof(init_transfers[0]));
    for (size_t i = 0; i < bus.count; ++i) {
        CHECK(bus.transfers[i].kind == init_transfers[i].kind);
        CHECK(bus.transfers[i].addr == init_transfers[i].addr);
        CHECK(bus.transfers[i].reg == init_transfers[i].reg);
        CHECK(bus.transfers[i].value == init_transfers[i].value);
    }
    CHECK(bus.regs[0x02] == 0);
    CHECK(bus.regs[0x06] == 0);
    CHECK(bus.regs[0x07] == 0xff);
    CHECK(bus.regs[0x05] == 0);
    CHECK(bus.regs[0x03] == 0xff);

    clear_transfers(&bus);
    dev.addr = 0x77;
    CHECK(initialize(&bus, &dev) == TCA9539_OK);
    for (size_t i = 0; i < bus.count; ++i) {
        CHECK(bus.transfers[i].addr == 0x77);
    }
}

static void test_init_io_failure(void)
{
    for (size_t i = 1; i <= 8; ++i) {
        struct bus bus;
        struct tca9539 dev;

        setup(&bus, &dev);
        dev.ready = true;
        bus.fail_at = i;
        bus.failure = (i % 2 == 0) ? 7 : -99;
        CHECK(initialize(&bus, &dev) == TCA9539_ERR_IO);
        CHECK(bus.count == i);
        if (i <= 7) {
            CHECK(bus.regs[0x06] == 0xff);
        }
        check_locked(&bus, &dev);
        check_recovery(&bus, &dev);
    }
}

static void test_init_verify_failure(void)
{
    for (size_t i = 2; i <= 8; i += 2) {
        struct bus bus;
        struct tca9539 dev;

        setup(&bus, &dev);
        bus.mismatch_at = i;
        CHECK(initialize(&bus, &dev) == TCA9539_ERR_VERIFY);
        CHECK(bus.count == i);
        if (i < 8) {
            CHECK(bus.regs[0x06] == 0xff);
        }
        check_locked(&bus, &dev);
        check_recovery(&bus, &dev);
    }
}

static void test_verify_off(void)
{
    struct bus bus;
    struct tca9539 dev;

    setup(&bus, &dev);
    CHECK(initialize(&bus, &dev) == TCA9539_OK);
    clear_transfers(&bus);
    CHECK(tca9539_verify_off(&dev) == TCA9539_OK);
    CHECK(dev.ready);
    CHECK(bus.count == 2);
    CHECK(bus.transfers[0].kind == 'R' && bus.transfers[0].reg == 0x02);
    CHECK(bus.transfers[1].kind == 'R' && bus.transfers[1].reg == 0x06);
}

static void test_verify_io_failure(void)
{
    for (size_t i = 1; i <= 2; ++i) {
        struct bus bus;
        struct tca9539 dev;

        setup(&bus, &dev);
        CHECK(initialize(&bus, &dev) == TCA9539_OK);
        clear_transfers(&bus);
        bus.fail_at = i;
        CHECK(tca9539_verify_off(&dev) == TCA9539_ERR_IO);
        CHECK(bus.count == i);
        check_locked(&bus, &dev);
        check_recovery(&bus, &dev);
    }
}

static void test_verify_mismatch(void)
{
    for (unsigned int bit = 0; bit < 8; ++bit) {
        for (unsigned int reg = 0x02; reg <= 0x06; reg += 4) {
            struct bus bus;
            struct tca9539 dev;

            setup(&bus, &dev);
            CHECK(initialize(&bus, &dev) == TCA9539_OK);
            clear_transfers(&bus);
            bus.regs[reg] = (uint8_t)(1u << bit);
            CHECK(tca9539_verify_off(&dev) == TCA9539_ERR_VERIFY);
            CHECK(bus.count == ((reg == 0x02) ? 1u : 2u));
            check_locked(&bus, &dev);
            check_recovery(&bus, &dev);
        }
    }
}

static void test_panel(void)
{
    struct bus bus;
    struct tca9539 dev;
    uint8_t panel = 0;

    setup(&bus, &dev);
    CHECK(initialize(&bus, &dev) == TCA9539_OK);
    for (unsigned int value = 0; value <= 0xff; ++value) {
        clear_transfers(&bus);
        bus.regs[0x01] = (uint8_t)value;
        CHECK(tca9539_read_panel(&dev, &panel) == TCA9539_OK);
        CHECK(panel == value);
        CHECK(dev.ready && bus.count == 1);
        CHECK(bus.transfers[0].kind == 'R' && bus.transfers[0].reg == 0x01);
    }

    clear_transfers(&bus);
    bus.fail_at = 1;
    panel = 0x5a;
    CHECK(tca9539_read_panel(&dev, &panel) == TCA9539_ERR_IO);
    CHECK(panel == 0x5a);
    check_locked(&bus, &dev);
    check_recovery(&bus, &dev);
}

static void test_invalid_arguments(void)
{
    struct bus bus;
    struct tca9539 dev;
    uint8_t panel = 0x5a;

    CHECK(tca9539_init(NULL) == TCA9539_ERR_ARG);
    CHECK(tca9539_verify_off(NULL) == TCA9539_ERR_ARG);
    CHECK(tca9539_read_panel(NULL, &panel) == TCA9539_ERR_ARG);
    CHECK(panel == 0x5a);

    for (unsigned int invalid = 0; invalid < 4; ++invalid) {
        setup(&bus, &dev);
        dev.ready = true;
        switch (invalid) {
        case 0: dev.read_reg = NULL; break;
        case 1: dev.write_reg = NULL; break;
        case 2: dev.addr = 0x73; break;
        default: dev.addr = 0x78; break;
        }
        CHECK(tca9539_init(&dev) == TCA9539_ERR_ARG);
        CHECK(!dev.ready && bus.count == 0);
        dev.ready = true;
        CHECK(tca9539_verify_off(&dev) == TCA9539_ERR_ARG);
        CHECK(!dev.ready && bus.count == 0);
        dev.ready = true;
        CHECK(tca9539_read_panel(&dev, &panel) == TCA9539_ERR_ARG);
        CHECK(!dev.ready && bus.count == 0 && panel == 0x5a);
    }

    setup(&bus, &dev);
    CHECK(initialize(&bus, &dev) == TCA9539_OK);
    clear_transfers(&bus);
    CHECK(tca9539_read_panel(&dev, NULL) == TCA9539_ERR_ARG);
    CHECK(bus.count == 0);
    check_locked(&bus, &dev);
    check_recovery(&bus, &dev);
}

int main(int argc, char **argv)
{
    static const struct {
        const char *name;
        void (*run)(void);
    } cases[] = {
        {"init_order", test_init_order},
        {"init_io_failure", test_init_io_failure},
        {"init_verify_failure", test_init_verify_failure},
        {"verify_off", test_verify_off},
        {"verify_io_failure", test_verify_io_failure},
        {"verify_mismatch", test_verify_mismatch},
        {"panel", test_panel},
        {"invalid_arguments", test_invalid_arguments}
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
