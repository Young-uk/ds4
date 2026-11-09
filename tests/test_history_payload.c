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
        assert(!ds4_history_payload_version_valid(4, qwen, tp));
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
    puts("history payload: unchanged v2 acceptance, Metal-only v3 and absolute-position bounds passed");
    return 0;
}
