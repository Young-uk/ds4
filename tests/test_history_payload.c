#include "ds4.h"
#include "ds4_history_payload.h"
#include <assert.h>
#include <stdio.h>
int main(void) {
    assert(ds4_history_image_discarded(100, 200, 64, 768, 1536));
    assert(ds4_history_image_discarded(1600, 100, 64, 768, 1536));
    assert(!ds4_history_image_discarded(60, 20, 64, 768, 1536));
    assert(!ds4_history_image_discarded(760, 20, 64, 768, 1536));
    assert(!ds4_history_image_discarded(800, 20, 64, 768, 1536));
    assert(!ds4_history_image_discarded(UINT64_MAX, 20, 64, 768, 1536));
    for (int qwen = 0; qwen < 2; qwen++) for (int tp = 0; tp < 2; tp++) {
        assert(ds4_history_payload_version_valid(2, qwen, tp));
        assert(ds4_history_payload_version_valid(3, qwen, tp) == (qwen && !tp));
        assert(!ds4_history_payload_version_valid(0, qwen, tp));
        assert(ds4_history_payload_version_valid(4, qwen, tp) == (qwen && !tp));
        assert(!ds4_history_payload_version_valid(5, qwen, tp));
    }
    uint32_t rows[] = {0,0,0,0, 1,1,1,0, 9,9,9,0, 10,10,10,0};
    assert(ds4_history_positions_valid(rows, 4, 7));
    assert(!ds4_history_positions_valid(rows, 4, 6));
    assert(!ds4_history_positions_valid(rows, 4, -1));
    assert(!ds4_history_positions_valid(rows, 4, INT32_MAX));
    assert(!ds4_history_positions_valid(NULL, 4, 7));
    rows[8] = 1; rows[9] = 1; rows[10] = 1;
    assert(!ds4_history_positions_valid(rows, 4, 7));
    rows[8] = 9; rows[9] = 8;
    assert(!ds4_history_positions_valid(rows, 4, 7));
    rows[9] = 9; rows[11] = 1;
    assert(!ds4_history_positions_valid(rows, 4, 7));
    int ids[] = {10, 11, 42, 42, 42, 42, 12, 13};
    uint32_t visual[] = {0,0,0,0, 1,1,1,0, 20,20,20,0, 20,20,21,0,
        20,21,20,0, 20,21,21,0, 22,22,22,0, 23,23,23,0};
    ds4_history_image image = {.token_start=2, .token_count=4};
    assert(ds4_history_visual_positions_valid(visual, ids, 8, 16, &image, 1, 42));
    assert(!ds4_history_visual_positions_valid(visual, ids, 8, 15, &image, 1, 42));
    assert(!ds4_history_visual_positions_valid(visual, ids, 8, 16, NULL, 0, 42));
    image.token_count=3;
    assert(!ds4_history_visual_positions_valid(visual, ids, 8, 16, &image, 1, 42));
    image.token_count=4; visual[18]++;
    assert(!ds4_history_visual_positions_valid(visual, ids, 8, 16, &image, 1, 42));
    visual[18]--; ids[2]=43;
    assert(!ds4_history_visual_positions_valid(visual, ids, 8, 16, &image, 1, 42));
    uint32_t compact_visual[] = {0,0,0,0, 1,1,1,0, 2,2,2,0, 2,2,3,0,
        2,3,2,0, 2,3,3,0, 4,4,4,0, 5,5,5,0};
    ids[2]=42;
    assert(ds4_history_visual_positions_valid(compact_visual, ids, 8, -2, &image, 1, 42));
    assert(!ds4_history_visual_positions_valid(compact_visual, ids, 8, -8, &image, 1, 42));
    puts("history payload: unchanged v2, Metal-only v3/v4, visual ownership and position bounds passed");
    return 0;
}
