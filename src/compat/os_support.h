/* Shim for RNNoise's plain-C vector path (vec.h), which includes
   "os_support.h" from Opus. The RNNoise v0.2 release doesn't ship that
   header because x86 builds always take the SSE path instead. */
#ifndef VOICEKERNEL_OS_SUPPORT_H
#define VOICEKERNEL_OS_SUPPORT_H

#include <string.h>

#define OPUS_CLEAR(dst, n) (memset((dst), 0, (n)*sizeof(*(dst))))
#define OPUS_COPY(dst, src, n) (memcpy((dst), (src), (n)*sizeof(*(dst))))
#define OPUS_MOVE(dst, src, n) (memmove((dst), (src), (n)*sizeof(*(dst))))

#endif
