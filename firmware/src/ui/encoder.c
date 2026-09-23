/* @brief 累计合法 Gray 转换并抵消回摆，非法跳变丢弃未完成格数。 */
#include "encoder.h"

#include <stddef.h>

int encoder_init(struct encoder *encoder, const struct encoder_config *config, uint8_t ab)
{
    struct encoder_config copy;

    if (encoder == NULL) {
        return ENCODER_ERR_ARG;
    }
    if (config == NULL) {
        *encoder = (struct encoder){0};
        return ENCODER_ERR_ARG;
    }
    copy = *config;
    *encoder = (struct encoder){0};
    if (ab > 3 || (copy.transitions_per_detent != 2 && copy.transitions_per_detent != 4)) {
        return ENCODER_ERR_ARG;
    }
    encoder->config = copy;
    encoder->previous = ab;
    encoder->synced = true;
    encoder->ready = true;
    return ENCODER_OK;
}

int encoder_update(struct encoder *encoder, uint8_t ab, int8_t *detents)
{
    static const int8_t transitions[16] = {
         0,  1, -1,  0,
        -1,  0,  0,  1,
         1,  0,  0, -1,
         0, -1,  1,  0
    };
    int8_t step;

    if (detents != NULL) {
        *detents = 0;
    }
    if (encoder == NULL || detents == NULL) {
        return ENCODER_ERR_ARG;
    }
    if (!encoder->ready) {
        return ENCODER_ERR_NOT_READY;
    }
    if (ab > 3) {
        encoder->partial = 0;
        encoder->synced = false;
        return ENCODER_ERR_ARG;
    }
    if (!encoder->synced || (encoder->previous ^ ab) == 3) {
        encoder->previous = ab;
        encoder->partial = 0;
        encoder->synced = true;
        return ENCODER_OK;
    }

    step = transitions[encoder->previous * 4u + ab];
    encoder->previous = ab;
    encoder->partial = (int8_t)(encoder->partial + step);
    if (encoder->partial >= encoder->config.transitions_per_detent) {
        *detents = 1;
    } else if (encoder->partial <= -(int)encoder->config.transitions_per_detent) {
        *detents = -1;
    }
    if (*detents != 0) {
        encoder->partial = 0;
        if (encoder->config.reversed) {
            *detents = (int8_t)-*detents;
        }
    }
    return ENCODER_OK;
}
