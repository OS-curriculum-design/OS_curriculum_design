#include "memdemo.h"

#include "../console/console.h"
#include "../fs/simplefs.h"
#include "../include/string.h"
#include "../mm/pmm.h"
#include "../mm/vmm.h"

#define MEMDEMO_PAGE_COUNT 8U
#define MEMDEMO_LOG_NAME "memdemo.log"
#define MEMDEMO_USER_BASE 0x02000000U

typedef struct {
    int allocated;
    uint32_t page_directory_phys;
    uint32_t virt_addr;
    uint32_t phys_addr;
} MemDemoSlot;

typedef struct {
    uint32_t sequence;
    uint32_t op;
    uint32_t page;
    uint32_t virt_addr;
    uint32_t phys_addr;
    uint32_t active_pages;
    uint32_t pmm_used_pages;
    uint32_t pmm_free_pages;
    int valid;
    int reported;
} MemDemoEvent;

static MemDemoSlot slots[MEMDEMO_PAGE_COUNT];
static MemDemoEvent events[MEMDEMO_EVENT_COUNT];
static uint32_t active_pages = 0;
static uint32_t sequence = 0;

static void append_char(char* buffer, uint32_t* pos, uint32_t cap, char value) {
    if (*pos + 1U >= cap) {
        return;
    }

    buffer[*pos] = value;
    (*pos)++;
    buffer[*pos] = '\0';
}

static void append_str(char* buffer, uint32_t* pos, uint32_t cap, const char* text) {
    while (*text) {
        append_char(buffer, pos, cap, *text);
        text++;
    }
}

static void append_dec(char* buffer, uint32_t* pos, uint32_t cap, uint32_t value) {
    char tmp[12];
    uint32_t i = 0;

    if (value == 0) {
        append_char(buffer, pos, cap, '0');
        return;
    }

    while (value > 0 && i < sizeof(tmp)) {
        tmp[i++] = (char)('0' + (value % 10U));
        value /= 10U;
    }

    while (i > 0) {
        append_char(buffer, pos, cap, tmp[--i]);
    }
}

static void append_hex32(char* buffer, uint32_t* pos, uint32_t cap, uint32_t value) {
    static const char hex[] = "0123456789ABCDEF";

    append_str(buffer, pos, cap, "0x");
    for (int i = 7; i >= 0; i--) {
        append_char(buffer, pos, cap, hex[(value >> ((uint32_t)i * 4U)) & 0xFU]);
    }
}

static const char* op_name(uint32_t op) {
    if (op == MEMDEMO_OP_ALLOC) {
        return "ALLOC";
    }
    if (op == MEMDEMO_OP_FREE) {
        return "FREE";
    }
    if (op == MEMDEMO_OP_TOUCH) {
        return "TOUCH";
    }
    return "UNKNOWN";
}

static uint32_t slot_virt(uint32_t page) {
    return MEMDEMO_USER_BASE + page * PAGE_SIZE;
}

static void clear_slot(uint32_t page) {
    slots[page].allocated = 0;
    slots[page].page_directory_phys = 0;
    slots[page].virt_addr = 0;
    slots[page].phys_addr = 0;
}

static void free_slot(uint32_t page) {
    if (!slots[page].allocated) {
        return;
    }

    vmm_unmap_page_in_directory(slots[page].page_directory_phys, slots[page].virt_addr);
    pmm_free_page(slots[page].phys_addr);
    clear_slot(page);
    if (active_pages > 0) {
        active_pages--;
    }
}

static int record_event(uint32_t op, uint32_t page, uint32_t virt_addr, uint32_t phys_addr) {
    MemDemoEvent* event;

    if (sequence >= MEMDEMO_EVENT_COUNT) {
        return 0;
    }

    event = &events[sequence];
    event->sequence = sequence + 1U;
    event->op = op;
    event->page = page;
    event->virt_addr = virt_addr;
    event->phys_addr = phys_addr;
    event->active_pages = active_pages;
    event->pmm_used_pages = pmm_get_used_pages();
    event->pmm_free_pages = pmm_get_free_pages();
    event->valid = 1;
    event->reported = 0;

    sequence++;
    return 1;
}

static void build_event_line(const MemDemoEvent* event, char* line, uint32_t cap) {
    uint32_t pos = 0;

    line[0] = '\0';
    append_str(line, &pos, cap, "seq=");
    append_dec(line, &pos, cap, event->sequence);
    append_str(line, &pos, cap, " op=");
    append_str(line, &pos, cap, op_name(event->op));
    append_str(line, &pos, cap, " slot=");
    append_dec(line, &pos, cap, event->page);
    append_str(line, &pos, cap, " virt=");
    append_hex32(line, &pos, cap, event->virt_addr);
    append_str(line, &pos, cap, " phys=");
    append_hex32(line, &pos, cap, event->phys_addr);
    append_str(line, &pos, cap, " active=");
    append_dec(line, &pos, cap, event->active_pages);
    append_str(line, &pos, cap, "/");
    append_dec(line, &pos, cap, MEMDEMO_PAGE_COUNT);
    append_str(line, &pos, cap, " pmm_used=");
    append_dec(line, &pos, cap, event->pmm_used_pages);
    append_str(line, &pos, cap, " pmm_free=");
    append_dec(line, &pos, cap, event->pmm_free_pages);
}

const char* memdemo_log_name(void) {
    return MEMDEMO_LOG_NAME;
}

void memdemo_reset(void) {
    static const char header[] =
        "MyOS real memory demo log\n"
        "page_size=4096 bytes, each ALLOC maps a real PMM page into user space\n";

    for (uint32_t i = 0; i < MEMDEMO_PAGE_COUNT; i++) {
        free_slot(i);
        clear_slot(i);
    }

    for (uint32_t i = 0; i < MEMDEMO_EVENT_COUNT; i++) {
        events[i].sequence = 0;
        events[i].op = 0;
        events[i].page = 0;
        events[i].virt_addr = 0;
        events[i].phys_addr = 0;
        events[i].active_pages = 0;
        events[i].pmm_used_pages = 0;
        events[i].pmm_free_pages = 0;
        events[i].valid = 0;
        events[i].reported = 0;
    }

    active_pages = 0;
    sequence = 0;

    if (simplefs_is_mounted()) {
        simplefs_write_file(MEMDEMO_LOG_NAME, (const uint8_t*)header, (uint32_t)strlen(header));
    }
}

int memdemo_apply_op(uint32_t op, uint32_t page) {
    uint32_t page_directory_phys = vmm_get_page_directory();
    uint32_t virt_addr;
    uint32_t phys_addr;

    if (page >= MEMDEMO_PAGE_COUNT || sequence >= MEMDEMO_EVENT_COUNT) {
        return 0;
    }

    virt_addr = slot_virt(page);

    if (op == MEMDEMO_OP_ALLOC) {
        if (!slots[page].allocated) {
            phys_addr = pmm_alloc_page();
            if (phys_addr == 0) {
                return 0;
            }

            memset(vmm_phys_to_virt(phys_addr), 0, PAGE_SIZE);
            if (!vmm_map_page_in_directory(page_directory_phys,
                                           virt_addr,
                                           phys_addr,
                                           VMM_PAGE_WRITABLE | VMM_PAGE_USER)) {
                pmm_free_page(phys_addr);
                return 0;
            }

            slots[page].allocated = 1;
            slots[page].page_directory_phys = page_directory_phys;
            slots[page].virt_addr = virt_addr;
            slots[page].phys_addr = phys_addr;
            active_pages++;
        }

        if (!record_event(op, page, slots[page].virt_addr, slots[page].phys_addr)) {
            return 0;
        }
        return slots[page].virt_addr;
    }

    if (op == MEMDEMO_OP_TOUCH) {
        if (!slots[page].allocated) {
            return 0;
        }

        if (!record_event(op, page, slots[page].virt_addr, slots[page].phys_addr)) {
            return 0;
        }
        return slots[page].virt_addr;
    }

    if (op == MEMDEMO_OP_FREE) {
        if (!slots[page].allocated) {
            return 0;
        }

        phys_addr = slots[page].phys_addr;
        free_slot(page);

        if (!record_event(op, page, virt_addr, phys_addr)) {
            return 0;
        }
        return 1;
    }

    return 0;
}

int memdemo_report_event(uint32_t requested_sequence) {
    MemDemoEvent* event;
    char line[160];
    char file_line[164];
    uint32_t pos = 0;

    if (requested_sequence == 0 || requested_sequence > MEMDEMO_EVENT_COUNT) {
        return 0;
    }

    event = &events[requested_sequence - 1U];
    if (!event->valid) {
        return 0;
    }

    if (!event->reported) {
        build_event_line(event, line, sizeof(line));

        console_write("memdemo: ");
        console_write_line(line);

        if (simplefs_is_mounted()) {
            file_line[0] = '\0';
            append_str(file_line, &pos, sizeof(file_line), line);
            append_char(file_line, &pos, sizeof(file_line), '\n');
            simplefs_append_file(MEMDEMO_LOG_NAME, (const uint8_t*)file_line, (uint32_t)strlen(file_line));
        }

        event->reported = 1;
    }

    return 1;
}

void memdemo_release_for_directory(uint32_t page_directory_phys) {
    for (uint32_t i = 0; i < MEMDEMO_PAGE_COUNT; i++) {
        if (slots[i].allocated && slots[i].page_directory_phys == page_directory_phys) {
            free_slot(i);
        }
    }
}
