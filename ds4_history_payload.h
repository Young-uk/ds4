#ifndef DS4_HISTORY_PAYLOAD_H
#define DS4_HISTORY_PAYLOAD_H
#include <stdbool.h>
#include <stdint.h>
#include <limits.h>

/* Ordinary v2 bytes remain valid for every existing backend. Approximate v3
 * state is explicitly limited to the local Qwen Metal history-cache path. */
static inline bool ds4_history_payload_version_valid(uint32_t version, bool qwen_metal, bool tensor_parallel) {
    return version == DS4_SESSION_PAYLOAD_VERSION ||
        ((version == DS4_SESSION_HISTORY_PAYLOAD_VERSION || version == DS4_SESSION_VISUAL_HISTORY_PAYLOAD_VERSION) && qwen_metal && !tensor_parallel);
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

/* v4 adds image ownership; ordinary v2 and text-only v3 bytes do not change. */
typedef struct {
    uint32_t token_start, token_count;
    uint8_t fingerprint[32];
} ds4_history_image;

static inline bool ds4_history_visual_positions_valid(const uint32_t *p, const int *ids,
        uint32_t rows, int32_t delta, const ds4_history_image *images, uint32_t count, int image_token) {
    const int64_t frontier = (int64_t)rows + delta;
    if (!p || !ids || !rows || !images || !count || count > rows || frontier <= 0 || frontier > INT32_MAX) return false;
    uint32_t image = 0, row = 0;
    int64_t last = -1;
    while (row < rows) {
        const uint32_t start = image < count ? images[image].token_start : rows;
        if (start < row || (image < count && start >= rows)) return false;
        const uint32_t *v = p + (uint64_t)row * 4;
        const uint32_t base = v[0];
        if ((int64_t)base <= last || base >= frontier) return false;
        if (row != start) {
            if (v[1] != base || v[2] != base || v[3] || ids[row] == image_token) return false;
            last = base; row++; continue;
        }
        const uint32_t n = images[image].token_count;
        if (!n || n > rows-row || (image+1 < count && images[image+1].token_start < row+n)) return false;
        uint32_t ph = base, pw = base, width = 0;
        for (uint32_t j = 0; j < n; j++) {
            v = p + (uint64_t)(row+j)*4;
            if (ids[row+j] != image_token || v[0] != base || v[3] || v[1] < base || v[2] < base || v[1] >= frontier || v[2] >= frontier) return false;
            if (!j) { if (v[1] != base || v[2] != base) return false; }
            else if (v[1] == ph) { if (v[2] != pw+1) return false; }
            else {
                if (v[1] != ph+1 || v[2] != base) return false;
                const uint32_t observed = pw-base+1;
                if (width && width != observed) return false;
                width = observed;
            }
            ph = v[1]; pw = v[2];
            if (ph > last) last = ph;
            if (pw > last) last = pw;
        }
        if (width && (n%width || pw-base+1 != width)) return false;
        row += n; image++;
    }
    return image == count && last < frontier;
}
#endif
