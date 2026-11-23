# RNNoise (vendored)

Unmodified source from the RNNoise v0.2 release by Jean-Marc Valin / Xiph.Org
(https://github.com/xiph/rnnoise/releases/tag/v0.2), BSD-3-Clause, see COPYING.

Left out: the training/dump tools (dump_features.c, write_weights.c) and the
x86 SIMD code. Only x86/x86_arch_macros.h is kept, since vec.h always includes it.
