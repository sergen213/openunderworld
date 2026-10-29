// Stand-in for miniz (api/internal/ffx_blob_decompress.h includes it; the SDK does not ship it). Our blobs from
// tools/ffx_shaders.py are never deflate-compressed, so the decompressor's only call never runs; if it did, the
// error leaves the bytes as they are and pipeline creation fails loudly.
#pragma once

typedef unsigned char mz_uint8;
typedef unsigned long mz_ulong;
enum { MZ_OK = 0, MZ_DATA_ERROR = -3 };
inline int mz_uncompress(unsigned char*, mz_ulong*, const unsigned char*, mz_ulong) { return MZ_DATA_ERROR; }
