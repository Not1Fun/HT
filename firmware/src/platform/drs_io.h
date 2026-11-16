/* @brief 独立低优先级线程只读DRS状态；通信失败不阻塞输出保护。 */
#ifndef HT_DRS_IO_H
#define HT_DRS_IO_H
#include "core/drs.h"
void drs_io_snapshot(struct drs_snapshot *state);
#endif
