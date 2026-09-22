/* @brief 有界 DGUS II 帧编解码与流式接收，不含串口配置或 HT 变量映射。 */
#ifndef DGUS_H
#define DGUS_H

#include <stddef.h>
#include <stdint.h>

/* 本模块的保守上限，不代表屏的协议极限；word 为 16 位，线上高字节在前。 */
#define DGUS_MAX_WORDS 120u
#define DGUS_MAX_FRAME (3u + 4u + 2u * DGUS_MAX_WORDS + 2u)

enum dgus_crc {
    DGUS_CRC_UNCONFIGURED = 0,
    DGUS_CRC_NONE,
    DGUS_CRC_MODBUS
};

enum dgus_result {
    DGUS_OK = 0,
    DGUS_ERR_ARG = -1,
    DGUS_ERR_MODE = -2,
    DGUS_ERR_SIZE = -3,
    DGUS_ERR_FRAME = -4,
    DGUS_ERR_CRC = -5
};

enum dgus_kind {
    DGUS_WRITE,
    DGUS_READ_REQUEST,
    DGUS_READ_DATA,
    DGUS_WRITE_ACK
};

struct dgus_frame {
    enum dgus_kind kind;
    uint16_t vp;
    uint8_t count;
    uint16_t words[DGUS_MAX_WORDS];
};

/* 编码成功返回帧字节数，失败返回负错误码且不修改 out；输入输出不得重叠。 */
int dgus_encode_write(enum dgus_crc mode, uint16_t vp, const uint16_t *words,
                      size_t count, uint8_t *out, size_t capacity);
int dgus_encode_read(enum dgus_crc mode, uint16_t vp, size_t count,
                     uint8_t *out, size_t capacity);
/* 只接受恰好一帧；失败不修改 frame。0x83 回传与主动上传在线上不能区分。 */
int dgus_decode(enum dgus_crc mode, const uint8_t *data, size_t length,
                struct dgus_frame *frame);

/* frame 只在回调期间有效；回调不得重入/修改同一 parser。 */
typedef void (*dgus_receive_fn)(void *ctx, const struct dgus_frame *frame);

struct dgus_parser {
    enum dgus_crc mode;
    dgus_receive_fn receive;
    void *ctx;
    size_t used;
    uint32_t rejected;
    uint8_t buffer[DGUS_MAX_FRAME];
};

int dgus_init(struct dgus_parser *parser, enum dgus_crc mode,
               dgus_receive_fn receive, void *ctx);
/* 串行调用；0 表示数据已处理，完整帧同步交给回调，坏帧计入 rejected。 */
int dgus_feed(struct dgus_parser *parser, const uint8_t *data, size_t length);
/* 调用者检测到帧间超时/串口丢字节时调用；清除残帧，保留显式模式及回调。 */
void dgus_reset(struct dgus_parser *parser);

#endif
