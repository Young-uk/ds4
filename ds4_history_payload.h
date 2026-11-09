#ifndef DS4_HISTORY_PAYLOAD_H
#define DS4_HISTORY_PAYLOAD_H
#include <stdbool.h>
#include <stdint.h>
#include <limits.h>

/* Ordinary v2 bytes remain valid for every existing backend. Approximate v3
 * state is explicitly limited to the local Qwen Metal history-cache path. */
static inline bool ds4_history_payload_version_valid(uint32_t version, bool qwen_metal, bool tensor_parallel) {
    return version == DS4_SESSION_PAYLOAD_VERSION ||
        (version == DS4_SESSION_HISTORY_PAYLOAD_VERSION && qwen_metal && !tensor_parallel);
}

/* Compacted text rows retain their original monotonic absolute positions.
 * Images use other coordinate geometry and cannot enter this v3 contract. */
static inline bool ds4_history_positions_valid(const uint32_t *positions, uint32_t rows, int32_t delta) {
    if (!positions || !rows || delta < 0 || (uint64_t)rows + (uint32_t)delta > INT32_MAX) return false;
    uint32_t previous = 0, frontier = rows + (uint32_t)delta;
    for (uint32_t row = 0; row < rows; row++) {
        const uint32_t *p = positions + (uint64_t)row * 4u;
        if (p[0] != p[1] || p[0] != p[2] || p[3] || p[0] >= frontier || (row && p[0] <= previous)) return false;
        previous = p[0];
    }
    return true;
}
/* A text handoff may discard an image span, but cannot retain a fragment of
 * an image or silently relabel retained image rows as text. */
static inline bool ds4_history_image_discarded(uint64_t start, uint64_t count,
        uint64_t prefix, uint64_t source, uint64_t keep_end) {
    return count && start <= UINT64_MAX - count && start >= prefix &&
        (start + count <= source || start >= keep_end);
}

#endif
