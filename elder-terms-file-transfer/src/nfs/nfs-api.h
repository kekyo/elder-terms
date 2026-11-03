// elder-terms: A terminal application, inspired by personal computing in the 1990s.
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/elder-terms/

#pragma once

#include <nfsc/libnfs.h>

/**
 * Queues a RAW operation using the installed library's public API generation.
 * @param operation RPC function prefix without its async/task suffix.
 * @param ... Arguments from the installed public header.
 * @returns Zero when a callback was queued, negative on immediate failure.
 * @remarks API v2 is not NFS protocol v2. Cancellation retires the whole context
 * on both API generations; no private symbols or vendor patches are required.
 */
#ifdef LIBNFS_API_V2
#define ELDER_NFS_RAW_CALL(operation, ...) (operation##_task(__VA_ARGS__) ? 0 : -1)
#else
#define ELDER_NFS_RAW_CALL(operation, ...) operation##_async(__VA_ARGS__)
#endif
