#include "memory.h"
#include "../console/console.h"
#include "../include/string.h"

#define MM_PAGE_SIZE 4096U
#define MM_TOTAL_PAGES 32U
#define MM_TOTAL_BYTES (MM_PAGE_SIZE * MM_TOTAL_PAGES)

#define MM_LOG_LINES 64
#define MM_TEXT_SIZE 96
#define MM_COMPARE_SEQUENCE 128

#define MM_MAX_PARTITIONS 32
#define MM_MAX_BESTFIT_BLOCKS 16

#define MM_MAX_SEGMENTS 8
#define MM_MAX_SEGMENT_PAGES 8

typedef enum {
    MM_MODE_BEST_FIT = 0,
    MM_MODE_SEG_PAGED = 1,
    MM_MODE_VIRTUAL = 2
} MemoryMode;

typedef struct {
    char text[MM_TEXT_SIZE];
} MemoryMessage;

typedef struct {
    uint32_t start;
    uint32_t size;
    uint8_t used;
    uint8_t owner_id;
} Partition;

typedef struct {
    uint8_t active;
    uint8_t owner_id;
    uint32_t request_size;
} BestFitBlock;

typedef struct {
    uint8_t active;
    uint8_t segment_id;
    uint8_t page_count;
    uint8_t frames[MM_MAX_SEGMENT_PAGES];
} Segment;

typedef struct {
    uint8_t present;
    uint8_t frame;
    uint32_t last_used;
} VmPage;

static MemoryMode current_mode = MM_MODE_BEST_FIT;
static int manager_running = 0;

static uint32_t configured_user_pages = 16;
static uint32_t configured_physical_frames = 8;

static volatile int allocator_can_run = 1;
static volatile int tracker_can_run = 0;
static uint32_t allocator_turns = 0;
static uint32_t tracker_turns = 0;
static uint32_t completed_cycles = 0;
static MemoryMessage shared_message;

static char log_buffer[MM_LOG_LINES][MM_TEXT_SIZE];
static uint32_t log_head = 0;
static uint32_t log_count = 0;

static uint32_t rng_state = 0x13572468U;

static Partition partitions[MM_MAX_PARTITIONS];
static BestFitBlock bestfit_blocks[MM_MAX_BESTFIT_BLOCKS];
static uint32_t partition_count = 0;
static uint8_t next_owner_id = 1;

static Segment segments[MM_MAX_SEGMENTS];
static int8_t segment_frame_owner[MM_TOTAL_PAGES];
static uint8_t next_segment_id = 1;

static VmPage vm_pages[MM_TOTAL_PAGES];
static int16_t vm_frame_to_page[MM_TOTAL_PAGES];
static uint32_t vm_accesses = 0;
static uint32_t vm_faults = 0;
static uint32_t vm_evictions = 0;
static uint32_t vm_timestamp = 0;
static uint32_t vm_last_page = 0;

static void line_reset(char* out) {
    out[0] = '\0';
}

static void line_append_char(char* out, size_t* pos, char c) {
    if (*pos + 1 >= MM_TEXT_SIZE) {
        return;
    }

    out[*pos] = c;
    (*pos)++;
    out[*pos] = '\0';
}

static void line_append_str(char* out, size_t* pos, const char* text) {
    while (*text) {
        line_append_char(out, pos, *text++);
    }
}

static void line_append_uint(char* out, size_t* pos, uint32_t value) {
    char digits[16];
    int index = 0;

    if (value == 0) {
        line_append_char(out, pos, '0');
        return;
    }

    while (value > 0 && index < (int)sizeof(digits)) {
        digits[index++] = (char)('0' + (value % 10U));
        value /= 10U;
    }

    while (index > 0) {
        line_append_char(out, pos, digits[--index]);
    }
}

static void push_log_line(const char* text) {
    strcpy(log_buffer[log_head], text);
    log_head = (log_head + 1U) % MM_LOG_LINES;
    if (log_count < MM_LOG_LINES) {
        log_count++;
    }
}

static void clear_logs(void) {
    for (uint32_t i = 0; i < MM_LOG_LINES; i++) {
        memset(log_buffer[i], 0, MM_TEXT_SIZE);
    }
    log_head = 0;
    log_count = 0;
}

static void note_message(const char* text) {
    push_log_line(text);
}

static uint32_t random_next(void) {
    rng_state = rng_state * 1664525U + 1013904223U;
    return rng_state;
}

static uint32_t random_range(uint32_t minimum, uint32_t maximum) {
    uint32_t span;

    if (maximum <= minimum) {
        return minimum;
    }

    span = maximum - minimum + 1U;
    return minimum + (random_next() % span);
}

static int choose_random_active_index(uint32_t total, int (*is_active)(uint32_t)) {
    uint32_t active_count = 0;
    uint32_t desired;

    for (uint32_t i = 0; i < total; i++) {
        if (is_active(i)) {
            active_count++;
        }
    }

    if (active_count == 0) {
        return -1;
    }

    desired = random_range(0, active_count - 1U);
    for (uint32_t i = 0; i < total; i++) {
        if (!is_active(i)) {
            continue;
        }
        if (desired == 0U) {
            return (int)i;
        }
        desired--;
    }

    return -1;
}

static const char* mode_name(MemoryMode mode) {
    switch (mode) {
        case MM_MODE_BEST_FIT:
            return "bestfit";
        case MM_MODE_SEG_PAGED:
            return "segpage";
        default:
            return "vm";
    }
}

static void reset_sync_state(void) {
    allocator_can_run = 1;
    tracker_can_run = 0;
    allocator_turns = 0;
    tracker_turns = 0;
    completed_cycles = 0;
    memset(shared_message.text, 0, MM_TEXT_SIZE);
}

static int bestfit_block_is_active(uint32_t index) {
    return bestfit_blocks[index].active;
}

static uint32_t bestfit_used_bytes(void) {
    uint32_t used = 0;

    for (uint32_t i = 0; i < partition_count; i++) {
        if (partitions[i].used) {
            used += partitions[i].size;
        }
    }

    return used;
}

static uint32_t bestfit_free_partition_count(void) {
    uint32_t free_count = 0;

    for (uint32_t i = 0; i < partition_count; i++) {
        if (!partitions[i].used) {
            free_count++;
        }
    }

    return free_count;
}

static void bestfit_merge_free_partitions(void) {
    uint32_t index = 0;

    while (index + 1U < partition_count) {
        if (partitions[index].used || partitions[index + 1U].used) {
            index++;
            continue;
        }

        partitions[index].size += partitions[index + 1U].size;
        for (uint32_t move = index + 1U; move + 1U < partition_count; move++) {
            partitions[move] = partitions[move + 1U];
        }
        partition_count--;
    }
}

static void bestfit_reset(void) {
    partition_count = 1;
    partitions[0].start = 0;
    partitions[0].size = MM_TOTAL_BYTES;
    partitions[0].used = 0;
    partitions[0].owner_id = 0;
    memset(bestfit_blocks, 0, sizeof(bestfit_blocks));
    next_owner_id = 1;
}

static void bestfit_allocate(MemoryMessage* message) {
    uint32_t request_size = random_range(256, 4096);
    uint32_t best_index = MM_MAX_PARTITIONS;
    uint32_t best_size = 0xFFFFFFFFU;
    int block_slot = choose_random_active_index(MM_MAX_BESTFIT_BLOCKS, bestfit_block_is_active);
    size_t pos = 0;

    if (block_slot >= 0 && random_range(0, 99) < 35U) {
        BestFitBlock* block = &bestfit_blocks[(uint32_t)block_slot];

        for (uint32_t i = 0; i < partition_count; i++) {
            if (partitions[i].used && partitions[i].owner_id == block->owner_id) {
                partitions[i].used = 0;
                partitions[i].owner_id = 0;
                bestfit_merge_free_partitions();
                line_reset(message->text);
                line_append_str(message->text, &pos, "bf free id=");
                line_append_uint(message->text, &pos, block->owner_id);
                line_append_str(message->text, &pos, " size=");
                line_append_uint(message->text, &pos, block->request_size);
                block->active = 0;
                return;
            }
        }
    }

    for (uint32_t i = 0; i < partition_count; i++) {
        if (partitions[i].used || partitions[i].size < request_size) {
            continue;
        }
        if (partitions[i].size < best_size) {
            best_size = partitions[i].size;
            best_index = i;
        }
    }

    line_reset(message->text);
    if (best_index == MM_MAX_PARTITIONS) {
        line_append_str(message->text, &pos, "bf alloc size=");
        line_append_uint(message->text, &pos, request_size);
        line_append_str(message->text, &pos, " fail");
        return;
    }

    block_slot = -1;
    for (uint32_t i = 0; i < MM_MAX_BESTFIT_BLOCKS; i++) {
        if (!bestfit_blocks[i].active) {
            block_slot = (int)i;
            break;
        }
    }

    if (block_slot < 0) {
        line_append_str(message->text, &pos, "bf alloc size=");
        line_append_uint(message->text, &pos, request_size);
        line_append_str(message->text, &pos, " fail");
        return;
    }

    partitions[best_index].used = 1;
    partitions[best_index].owner_id = next_owner_id++;

    if (partitions[best_index].size > request_size && partition_count < MM_MAX_PARTITIONS) {
        for (uint32_t move = partition_count; move > best_index + 1U; move--) {
            partitions[move] = partitions[move - 1U];
        }
        partitions[best_index + 1U].start = partitions[best_index].start + request_size;
        partitions[best_index + 1U].size = partitions[best_index].size - request_size;
        partitions[best_index + 1U].used = 0;
        partitions[best_index + 1U].owner_id = 0;
        partitions[best_index].size = request_size;
        partition_count++;
    }

    bestfit_blocks[(uint32_t)block_slot].active = 1;
    bestfit_blocks[(uint32_t)block_slot].owner_id = partitions[best_index].owner_id;
    bestfit_blocks[(uint32_t)block_slot].request_size = request_size;

    line_append_str(message->text, &pos, "bf alloc id=");
    line_append_uint(message->text, &pos, partitions[best_index].owner_id);
    line_append_str(message->text, &pos, " size=");
    line_append_uint(message->text, &pos, request_size);
}

static void bestfit_snapshot(char* out) {
    uint32_t used = bestfit_used_bytes();
    uint32_t free_bytes = MM_TOTAL_BYTES - used;
    uint32_t free_parts = bestfit_free_partition_count();
    size_t pos = 0;

    line_reset(out);
    line_append_str(out, &pos, " | used=");
    line_append_uint(out, &pos, used);
    line_append_str(out, &pos, " free=");
    line_append_uint(out, &pos, free_bytes);
    line_append_str(out, &pos, " holes=");
    line_append_uint(out, &pos, free_parts);
}

static int segment_is_active(uint32_t index) {
    return segments[index].active;
}

static uint32_t segment_free_frames(void) {
    uint32_t free_frames = 0;

    for (uint32_t i = 0; i < MM_TOTAL_PAGES; i++) {
        if (segment_frame_owner[i] < 0) {
            free_frames++;
        }
    }

    return free_frames;
}

static void segpage_reset(void) {
    memset(segments, 0, sizeof(segments));
    for (uint32_t i = 0; i < MM_TOTAL_PAGES; i++) {
        segment_frame_owner[i] = -1;
    }
    next_segment_id = 1;
}

static void segpage_allocate_segment(MemoryMessage* message) {
    uint32_t needed_pages = random_range(1, 6);
    int slot = -1;
    size_t pos = 0;

    for (uint32_t i = 0; i < MM_MAX_SEGMENTS; i++) {
        if (!segments[i].active) {
            slot = (int)i;
            break;
        }
    }

    line_reset(message->text);
    if (slot < 0 || segment_free_frames() < needed_pages) {
        line_append_str(message->text, &pos, "sp create pages=");
        line_append_uint(message->text, &pos, needed_pages);
        line_append_str(message->text, &pos, " fail");
        return;
    }

    segments[(uint32_t)slot].active = 1;
    segments[(uint32_t)slot].segment_id = next_segment_id++;
    segments[(uint32_t)slot].page_count = (uint8_t)needed_pages;

    for (uint32_t page = 0; page < needed_pages; page++) {
        for (uint32_t frame = 0; frame < MM_TOTAL_PAGES; frame++) {
            if (segment_frame_owner[frame] >= 0) {
                continue;
            }
            segment_frame_owner[frame] = segments[(uint32_t)slot].segment_id;
            segments[(uint32_t)slot].frames[page] = (uint8_t)frame;
            break;
        }
    }

    line_append_str(message->text, &pos, "sp create seg=");
    line_append_uint(message->text, &pos, segments[(uint32_t)slot].segment_id);
    line_append_str(message->text, &pos, " pages=");
    line_append_uint(message->text, &pos, needed_pages);
}

static void segpage_access_or_release(MemoryMessage* message) {
    int index = choose_random_active_index(MM_MAX_SEGMENTS, segment_is_active);
    size_t pos = 0;

    line_reset(message->text);
    if (index < 0) {
        segpage_allocate_segment(message);
        return;
    }

    if (random_range(0, 99) < 35U) {
        Segment* segment = &segments[(uint32_t)index];

        for (uint32_t page = 0; page < segment->page_count; page++) {
            segment_frame_owner[segment->frames[page]] = -1;
        }

        line_append_str(message->text, &pos, "sp free seg=");
        line_append_uint(message->text, &pos, segment->segment_id);
        segment->active = 0;
        segment->segment_id = 0;
        segment->page_count = 0;
        return;
    }

    {
        Segment* segment = &segments[(uint32_t)index];
        uint32_t page = random_range(0, segment->page_count - 1U);
        uint32_t offset = random_range(0, MM_PAGE_SIZE - 1U);

        line_append_str(message->text, &pos, "sp access seg=");
        line_append_uint(message->text, &pos, segment->segment_id);
        line_append_str(message->text, &pos, " page=");
        line_append_uint(message->text, &pos, page);
        line_append_str(message->text, &pos, " off=");
        line_append_uint(message->text, &pos, offset);
        line_append_str(message->text, &pos, " frame=");
        line_append_uint(message->text, &pos, segment->frames[page]);
    }
}

static void segpage_step(MemoryMessage* message) {
    uint32_t active_segments = 0;

    for (uint32_t i = 0; i < MM_MAX_SEGMENTS; i++) {
        if (segments[i].active) {
            active_segments++;
        }
    }

    if (active_segments == 0 || random_range(0, 99) < 45U) {
        segpage_allocate_segment(message);
        return;
    }

    segpage_access_or_release(message);
}

static void segpage_snapshot(char* out) {
    uint32_t active_segments = 0;
    uint32_t used_frames = MM_TOTAL_PAGES - segment_free_frames();
    size_t pos = 0;

    for (uint32_t i = 0; i < MM_MAX_SEGMENTS; i++) {
        if (segments[i].active) {
            active_segments++;
        }
    }

    line_reset(out);
    line_append_str(out, &pos, " | segs=");
    line_append_uint(out, &pos, active_segments);
    line_append_str(out, &pos, " frames=");
    line_append_uint(out, &pos, used_frames);
    line_append_char(out, &pos, '/');
    line_append_uint(out, &pos, MM_TOTAL_PAGES);
}

static void vm_reset(void) {
    memset(vm_pages, 0, sizeof(vm_pages));
    for (uint32_t i = 0; i < MM_TOTAL_PAGES; i++) {
        vm_frame_to_page[i] = -1;
    }
    vm_accesses = 0;
    vm_faults = 0;
    vm_evictions = 0;
    vm_timestamp = 0;
    vm_last_page = 0;
}

static uint32_t vm_pick_next_page(void) {
    if (configured_user_pages <= 1U) {
        return 0;
    }

    if (vm_accesses > 0 && random_range(0, 99) < 70U) {
        int32_t delta = (int32_t)random_range(0, 2) - 1;
        int32_t candidate = (int32_t)vm_last_page + delta;

        if (candidate < 0) {
            candidate = 0;
        }
        if (candidate >= (int32_t)configured_user_pages) {
            candidate = (int32_t)configured_user_pages - 1;
        }
        return (uint32_t)candidate;
    }

    return random_range(0, configured_user_pages - 1U);
}

static int vm_find_free_frame(void) {
    for (uint32_t frame = 0; frame < configured_physical_frames; frame++) {
        if (vm_frame_to_page[frame] < 0) {
            return (int)frame;
        }
    }
    return -1;
}

static uint32_t vm_pick_lru_victim(void) {
    uint32_t victim = 0;
    uint32_t oldest = 0xFFFFFFFFU;

    for (uint32_t page = 0; page < configured_user_pages; page++) {
        if (!vm_pages[page].present) {
            continue;
        }
        if (vm_pages[page].last_used < oldest) {
            oldest = vm_pages[page].last_used;
            victim = page;
        }
    }

    return victim;
}

static void vm_step(MemoryMessage* message) {
    uint32_t page = vm_pick_next_page();
    size_t pos = 0;

    vm_accesses++;
    vm_timestamp++;
    vm_last_page = page;

    line_reset(message->text);
    line_append_str(message->text, &pos, "vm access page=");
    line_append_uint(message->text, &pos, page);

    if (vm_pages[page].present) {
        vm_pages[page].last_used = vm_timestamp;
        line_append_str(message->text, &pos, " hit frame=");
        line_append_uint(message->text, &pos, vm_pages[page].frame);
        return;
    }

    vm_faults++;
    line_append_str(message->text, &pos, " fault");

    {
        int frame = vm_find_free_frame();

        if (frame < 0) {
            uint32_t victim = vm_pick_lru_victim();
            frame = vm_pages[victim].frame;
            vm_pages[victim].present = 0;
            vm_frame_to_page[(uint32_t)frame] = -1;
            vm_evictions++;
            line_append_str(message->text, &pos, " evict=");
            line_append_uint(message->text, &pos, victim);
        }

        vm_pages[page].present = 1;
        vm_pages[page].frame = (uint8_t)frame;
        vm_pages[page].last_used = vm_timestamp;
        vm_frame_to_page[(uint32_t)frame] = (int16_t)page;
        line_append_str(message->text, &pos, " frame=");
        line_append_uint(message->text, &pos, (uint32_t)frame);
    }
}

static void vm_snapshot(char* out) {
    uint32_t resident = 0;
    size_t pos = 0;

    for (uint32_t page = 0; page < configured_user_pages; page++) {
        if (vm_pages[page].present) {
            resident++;
        }
    }

    line_reset(out);
    line_append_str(out, &pos, " | faults=");
    line_append_uint(out, &pos, vm_faults);
    line_append_str(out, &pos, " evict=");
    line_append_uint(out, &pos, vm_evictions);
    line_append_str(out, &pos, " resident=");
    line_append_uint(out, &pos, resident);
    line_append_char(out, &pos, '/');
    line_append_uint(out, &pos, configured_physical_frames);
}

static void reset_mode_state(void) {
    switch (current_mode) {
        case MM_MODE_BEST_FIT:
            bestfit_reset();
            break;
        case MM_MODE_SEG_PAGED:
            segpage_reset();
            break;
        case MM_MODE_VIRTUAL:
            vm_reset();
            break;
    }

    reset_sync_state();
}

static void allocator_thread_step(void) {
    allocator_turns++;

    switch (current_mode) {
        case MM_MODE_BEST_FIT:
            bestfit_allocate(&shared_message);
            break;
        case MM_MODE_SEG_PAGED:
            segpage_step(&shared_message);
            break;
        case MM_MODE_VIRTUAL:
            vm_step(&shared_message);
            break;
    }

    allocator_can_run = 0;
    tracker_can_run = 1;
}

static void tracker_thread_step(void) {
    char line[MM_TEXT_SIZE];
    char snapshot[MM_TEXT_SIZE];
    size_t pos = 0;

    tracker_turns++;
    line_reset(snapshot);

    switch (current_mode) {
        case MM_MODE_BEST_FIT:
            bestfit_snapshot(snapshot);
            break;
        case MM_MODE_SEG_PAGED:
            segpage_snapshot(snapshot);
            break;
        case MM_MODE_VIRTUAL:
            vm_snapshot(snapshot);
            break;
    }

    line_reset(line);
    line_append_char(line, &pos, '[');
    line_append_uint(line, &pos, completed_cycles);
    line_append_char(line, &pos, ']');
    line_append_char(line, &pos, ' ');
    line_append_str(line, &pos, shared_message.text);
    line_append_str(line, &pos, snapshot);
    push_log_line(line);

    tracker_can_run = 0;
    allocator_can_run = 1;
    completed_cycles++;
}

static void run_one_cycle(void) {
    if (allocator_can_run) {
        allocator_thread_step();
    }

    if (tracker_can_run) {
        tracker_thread_step();
    }
}

static void print_compare_line(uint32_t frames, uint32_t fifo_faults, uint32_t lru_faults, uint32_t clock_faults) {
    console_write("frames=");
    console_write_dec((int)frames);
    console_write(" fifo=");
    console_write_dec((int)fifo_faults);
    console_write(" lru=");
    console_write_dec((int)lru_faults);
    console_write(" clock=");
    console_write_dec((int)clock_faults);
    console_put_char('\n');
}

static void generate_access_sequence(uint8_t* sequence, uint32_t length, uint32_t page_count) {
    uint32_t current = random_range(0, page_count - 1U);

    for (uint32_t i = 0; i < length; i++) {
        if (i > 0 && random_range(0, 99) < 70U) {
            int32_t delta = (int32_t)random_range(0, 2) - 1;
            int32_t next = (int32_t)current + delta;

            if (next < 0) {
                next = 0;
            }
            if (next >= (int32_t)page_count) {
                next = (int32_t)page_count - 1;
            }
            current = (uint32_t)next;
        } else {
            current = random_range(0, page_count - 1U);
        }

        sequence[i] = (uint8_t)current;
    }
}

static uint32_t simulate_fifo(const uint8_t* sequence, uint32_t length, uint32_t frames) {
    int16_t frame_pages[MM_TOTAL_PAGES];
    uint32_t faults = 0;
    uint32_t loaded = 0;
    uint32_t hand = 0;

    for (uint32_t i = 0; i < MM_TOTAL_PAGES; i++) {
        frame_pages[i] = -1;
    }

    for (uint32_t i = 0; i < length; i++) {
        int hit = 0;

        for (uint32_t frame = 0; frame < frames; frame++) {
            if (frame_pages[frame] == (int16_t)sequence[i]) {
                hit = 1;
                break;
            }
        }

        if (hit) {
            continue;
        }

        faults++;
        if (loaded < frames) {
            frame_pages[loaded++] = (int16_t)sequence[i];
        } else {
            frame_pages[hand] = (int16_t)sequence[i];
            hand = (hand + 1U) % frames;
        }
    }

    return faults;
}

static uint32_t simulate_lru(const uint8_t* sequence, uint32_t length, uint32_t frames) {
    int16_t frame_pages[MM_TOTAL_PAGES];
    uint32_t frame_used[MM_TOTAL_PAGES];
    uint32_t faults = 0;
    uint32_t loaded = 0;

    for (uint32_t i = 0; i < MM_TOTAL_PAGES; i++) {
        frame_pages[i] = -1;
        frame_used[i] = 0;
    }

    for (uint32_t i = 0; i < length; i++) {
        int hit_frame = -1;

        for (uint32_t frame = 0; frame < frames; frame++) {
            if (frame_pages[frame] == (int16_t)sequence[i]) {
                hit_frame = (int)frame;
                break;
            }
        }

        if (hit_frame >= 0) {
            frame_used[(uint32_t)hit_frame] = i + 1U;
            continue;
        }

        faults++;
        if (loaded < frames) {
            frame_pages[loaded] = (int16_t)sequence[i];
            frame_used[loaded] = i + 1U;
            loaded++;
            continue;
        }

        {
            uint32_t victim = 0;
            uint32_t oldest = frame_used[0];

            for (uint32_t frame = 1; frame < frames; frame++) {
                if (frame_used[frame] < oldest) {
                    oldest = frame_used[frame];
                    victim = frame;
                }
            }

            frame_pages[victim] = (int16_t)sequence[i];
            frame_used[victim] = i + 1U;
        }
    }

    return faults;
}

static uint32_t simulate_clock(const uint8_t* sequence, uint32_t length, uint32_t frames) {
    int16_t frame_pages[MM_TOTAL_PAGES];
    uint8_t ref_bits[MM_TOTAL_PAGES];
    uint32_t faults = 0;
    uint32_t loaded = 0;
    uint32_t hand = 0;

    for (uint32_t i = 0; i < MM_TOTAL_PAGES; i++) {
        frame_pages[i] = -1;
        ref_bits[i] = 0;
    }

    for (uint32_t i = 0; i < length; i++) {
        int hit_frame = -1;

        for (uint32_t frame = 0; frame < frames; frame++) {
            if (frame_pages[frame] == (int16_t)sequence[i]) {
                hit_frame = (int)frame;
                break;
            }
        }

        if (hit_frame >= 0) {
            ref_bits[(uint32_t)hit_frame] = 1;
            continue;
        }

        faults++;
        if (loaded < frames) {
            frame_pages[loaded] = (int16_t)sequence[i];
            ref_bits[loaded] = 1;
            loaded++;
            continue;
        }

        while (ref_bits[hand]) {
            ref_bits[hand] = 0;
            hand = (hand + 1U) % frames;
        }

        frame_pages[hand] = (int16_t)sequence[i];
        ref_bits[hand] = 1;
        hand = (hand + 1U) % frames;
    }

    return faults;
}

void memory_manager_init(void) {
    clear_logs();
    bestfit_reset();
    segpage_reset();
    vm_reset();
    reset_sync_state();
    note_message("Memory manager ready. Use 'mem help' for commands.");
}

void memory_manager_tick(void) {
    if (!manager_running) {
        return;
    }

    run_one_cycle();
}

void memory_manager_start(void) {
    manager_running = 1;
    note_message("Memory simulation running.");
}

void memory_manager_stop(void) {
    manager_running = 0;
    note_message("Memory simulation stopped.");
}

int memory_manager_is_running(void) {
    return manager_running;
}

int memory_manager_set_mode(const char* name) {
    if (strcmp(name, "bestfit") == 0) {
        current_mode = MM_MODE_BEST_FIT;
    } else if (strcmp(name, "segpage") == 0) {
        current_mode = MM_MODE_SEG_PAGED;
    } else if (strcmp(name, "vm") == 0) {
        current_mode = MM_MODE_VIRTUAL;
    } else {
        return 0;
    }

    clear_logs();
    reset_mode_state();
    note_message("Memory mode switched.");
    return 1;
}

const char* memory_manager_mode_name(void) {
    return mode_name(current_mode);
}

int memory_manager_set_user_pages(uint32_t pages) {
    if (pages < 4U || pages > MM_TOTAL_PAGES) {
        return 0;
    }

    configured_user_pages = pages;
    if (current_mode == MM_MODE_VIRTUAL) {
        vm_reset();
        reset_sync_state();
    }
    note_message("User page count updated.");
    return 1;
}

int memory_manager_set_physical_frames(uint32_t frames) {
    if (frames < 4U || frames > MM_TOTAL_PAGES) {
        return 0;
    }

    configured_physical_frames = frames;
    if (current_mode == MM_MODE_VIRTUAL) {
        vm_reset();
        reset_sync_state();
    }
    note_message("Physical frame count updated.");
    return 1;
}

void memory_manager_reset(void) {
    clear_logs();
    reset_mode_state();
    note_message("Memory mode state reset.");
}

void memory_manager_step(uint32_t cycles) {
    if (cycles == 0U) {
        cycles = 1U;
    }

    for (uint32_t i = 0; i < cycles; i++) {
        run_one_cycle();
    }
}

void memory_manager_print_help(void) {
    console_write_line("Memory commands:");
    console_write_line("  mem");
    console_write_line("  mem help");
    console_write_line("  mem mode bestfit|segpage|vm");
    console_write_line("  mem start");
    console_write_line("  mem stop");
    console_write_line("  mem reset");
    console_write_line("  mem step <cycles>");
    console_write_line("  mem pages <4-32>");
    console_write_line("  mem frames <4-32>");
    console_write_line("  mem log");
    console_write_line("  mem compare");
}

void memory_manager_print_status(void) {
    console_write("Mode: ");
    console_write_line(mode_name(current_mode));
    console_write("State: ");
    console_write_line(manager_running ? "running" : "stopped");
    console_write("Page size: ");
    console_write_dec((int)MM_PAGE_SIZE);
    console_write_line(" bytes");
    console_write("User pages: ");
    console_write_dec((int)configured_user_pages);
    console_put_char('\n');
    console_write("Physical frames: ");
    console_write_dec((int)configured_physical_frames);
    console_put_char('\n');
    console_write("Allocator turns: ");
    console_write_dec((int)allocator_turns);
    console_write(" tracker turns: ");
    console_write_dec((int)tracker_turns);
    console_put_char('\n');
    console_write("Completed cycles: ");
    console_write_dec((int)completed_cycles);
    console_put_char('\n');
}

void memory_manager_print_log(void) {
    uint32_t count = log_count < 12U ? log_count : 12U;
    uint32_t start;

    if (count == 0U) {
        console_write_line("Memory log is empty.");
        return;
    }

    console_write_line("Recent memory log:");
    start = (log_head + MM_LOG_LINES - count) % MM_LOG_LINES;
    for (uint32_t i = 0; i < count; i++) {
        uint32_t index = (start + i) % MM_LOG_LINES;
        console_write_line(log_buffer[index]);
    }
}

void memory_manager_run_compare(void) {
    uint8_t sequence[MM_COMPARE_SEQUENCE];

    generate_access_sequence(sequence, MM_COMPARE_SEQUENCE, configured_user_pages);

    console_write("Paging compare: pages=");
    console_write_dec((int)configured_user_pages);
    console_write(" sequence=");
    console_write_dec((int)MM_COMPARE_SEQUENCE);
    console_write(" page_size=");
    console_write_dec((int)MM_PAGE_SIZE);
    console_put_char('\n');

    for (uint32_t frames = 4U; frames <= MM_TOTAL_PAGES; frames++) {
        uint32_t fifo_faults = simulate_fifo(sequence, MM_COMPARE_SEQUENCE, frames);
        uint32_t lru_faults = simulate_lru(sequence, MM_COMPARE_SEQUENCE, frames);
        uint32_t clock_faults = simulate_clock(sequence, MM_COMPARE_SEQUENCE, frames);

        print_compare_line(frames, fifo_faults, lru_faults, clock_faults);
    }
}
