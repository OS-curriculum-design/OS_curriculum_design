#include "pager.h"

#include "../console/console.h"
#include "../drivers/ata.h"
#include "pmm.h"
#include "vmm.h"

#define PAGER_SWAP_LBA_BASE 2048U
#define PAGER_SECTORS_PER_PAGE (PAGE_SIZE / ATA_SECTOR_SIZE)

typedef struct {
    int used;
    int present;
    int swapped;
    uint32_t virt_addr;
    uint32_t phys_addr;
    uint32_t flags;
    uint32_t swap_slot;
} PagerPage;

static PagerPage pager_pages[PAGER_MAX_PAGES];
static uint32_t frame_pages[PAGER_FRAME_LIMIT];
static uint8_t zero_page[PAGE_SIZE];

static uint32_t registered_pages = 0;
static uint32_t resident_frames = 0;
static uint32_t clock_hand = 0;
static uint32_t page_faults = 0;
static uint32_t evictions = 0;
static uint32_t swap_ins = 0;
static uint32_t swap_outs = 0;
static uint32_t last_fault_addr = 0;
static int pager_ready = 0;

static uint32_t align_down_page(uint32_t value) {
    return value & ~(PAGE_SIZE - 1U);
}

static uint32_t swap_lba_for_slot(uint32_t slot) {
    return PAGER_SWAP_LBA_BASE + slot * PAGER_SECTORS_PER_PAGE;
}

static int swap_write_slot(uint32_t slot, const void* buffer) {
    return ata_write_sectors(swap_lba_for_slot(slot), (uint8_t)PAGER_SECTORS_PER_PAGE, buffer);
}

static int swap_read_slot(uint32_t slot, void* buffer) {
    return ata_read_sectors(swap_lba_for_slot(slot), (uint8_t)PAGER_SECTORS_PER_PAGE, buffer);
}

static int find_page_by_virt(uint32_t virt_addr, uint32_t* page_index_out) {
    uint32_t page_addr = align_down_page(virt_addr);

    for (uint32_t i = 0; i < PAGER_MAX_PAGES; i++) {
        if (pager_pages[i].used && pager_pages[i].virt_addr == page_addr) {
            if (page_index_out != (uint32_t*)0) {
                *page_index_out = i;
            }
            return 1;
        }
    }

    return 0;
}

static int find_free_page_entry(uint32_t* page_index_out) {
    for (uint32_t i = 0; i < PAGER_MAX_PAGES; i++) {
        if (!pager_pages[i].used) {
            *page_index_out = i;
            return 1;
        }
    }

    return 0;
}

static int page_was_accessed(uint32_t page_index) {
    uint32_t entry = 0;

    if (!vmm_get_page_entry(pager_pages[page_index].virt_addr, &entry)) {
        return 0;
    }

    return (entry & VMM_PAGE_ACCESSED) != 0;
}

static uint32_t choose_victim_frame(void) {
    while (1) {
        uint32_t page_index = frame_pages[clock_hand];

        if (page_was_accessed(page_index)) {
            vmm_clear_page_accessed(pager_pages[page_index].virt_addr);
            clock_hand = (clock_hand + 1U) % PAGER_FRAME_LIMIT;
            continue;
        }

        return clock_hand;
    }
}

static int evict_frame(uint32_t frame_slot) {
    uint32_t victim_index = frame_pages[frame_slot];
    PagerPage* victim = &pager_pages[victim_index];

    if (!swap_write_slot(victim->swap_slot, vmm_phys_to_virt(victim->phys_addr))) {
        console_write_line("pager: disk swap-out failed");
        return 0;
    }

    vmm_unmap_page(victim->virt_addr);
    pmm_free_page(victim->phys_addr);

    victim->phys_addr = 0;
    victim->present = 0;
    victim->swapped = 1;

    evictions++;
    swap_outs++;
    return 1;
}

static int page_in(uint32_t page_index) {
    uint32_t frame_slot;
    uint32_t phys_addr;
    PagerPage* page = &pager_pages[page_index];

    page_faults++;
    last_fault_addr = page->virt_addr;

    if (resident_frames < PAGER_FRAME_LIMIT) {
        frame_slot = resident_frames++;
    } else {
        frame_slot = choose_victim_frame();
        if (!evict_frame(frame_slot)) {
            return 0;
        }
    }

    phys_addr = pmm_alloc_page();
    if (phys_addr == 0) {
        console_write_line("pager: out of physical pages");
        return 0;
    }

    if (!swap_read_slot(page->swap_slot, vmm_phys_to_virt(phys_addr))) {
        pmm_free_page(phys_addr);
        console_write_line("pager: disk swap-in failed");
        return 0;
    }

    if (!vmm_map_page(page->virt_addr, phys_addr, page->flags)) {
        pmm_free_page(phys_addr);
        console_write_line("pager: vmm_map_page failed");
        return 0;
    }

    frame_pages[frame_slot] = page_index;
    clock_hand = (frame_slot + 1U) % PAGER_FRAME_LIMIT;

    page->phys_addr = phys_addr;
    page->present = 1;
    page->swapped = 0;
    swap_ins++;
    return 1;
}

int pager_init(void) {
    if (!ata_is_ready()) {
        pager_ready = 0;
        return 0;
    }

    for (uint32_t i = 0; i < PAGER_MAX_PAGES; i++) {
        pager_pages[i].used = 0;
        pager_pages[i].present = 0;
        pager_pages[i].swapped = 0;
        pager_pages[i].virt_addr = 0;
        pager_pages[i].phys_addr = 0;
        pager_pages[i].flags = 0;
        pager_pages[i].swap_slot = i;
    }

    for (uint32_t i = 0; i < PAGE_SIZE; i++) {
        zero_page[i] = 0;
    }

    registered_pages = 0;
    resident_frames = 0;
    clock_hand = 0;
    page_faults = 0;
    evictions = 0;
    swap_ins = 0;
    swap_outs = 0;
    last_fault_addr = 0;
    pager_ready = 1;
    return 1;
}

int pager_is_ready(void) {
    return pager_ready;
}

int pager_register_page(uint32_t virt_addr, uint32_t flags) {
    uint32_t page_index;
    uint32_t page_addr;

    if (!pager_ready || (virt_addr % PAGE_SIZE) != 0) {
        return 0;
    }

    if (find_page_by_virt(virt_addr, &page_index)) {
        pager_pages[page_index].flags = flags;
        return 1;
    }

    if (!find_free_page_entry(&page_index)) {
        return 0;
    }

    page_addr = align_down_page(virt_addr);
    if (!swap_write_slot(pager_pages[page_index].swap_slot, zero_page)) {
        return 0;
    }

    pager_pages[page_index].used = 1;
    pager_pages[page_index].present = 0;
    pager_pages[page_index].swapped = 1;
    pager_pages[page_index].virt_addr = page_addr;
    pager_pages[page_index].phys_addr = 0;
    pager_pages[page_index].flags = flags;

    registered_pages++;
    return 1;
}

int pager_handle_page_fault(uint32_t fault_addr, uint32_t err_code) {
    uint32_t page_index;

    if (!pager_ready || (err_code & 0x01)) {
        return 0;
    }

    if (!find_page_by_virt(fault_addr, &page_index)) {
        return 0;
    }

    if (pager_pages[page_index].present) {
        return 1;
    }

    return page_in(page_index);
}

void pager_print_stats(void) {
    console_write("pager: ");
    console_write_line(pager_ready ? "ready" : "not ready");

    console_write("registered pages: ");
    console_write_dec((int)registered_pages);
    console_put_char('\n');

    console_write("resident frames: ");
    console_write_dec((int)resident_frames);
    console_write("/");
    console_write_dec((int)PAGER_FRAME_LIMIT);
    console_put_char('\n');

    console_write("page faults: ");
    console_write_dec((int)page_faults);
    console_put_char('\n');

    console_write("last fault addr: ");
    console_write_hex(last_fault_addr);
    console_put_char('\n');

    console_write("evictions: ");
    console_write_dec((int)evictions);
    console_put_char('\n');

    console_write("swap in/out: ");
    console_write_dec((int)swap_ins);
    console_write("/");
    console_write_dec((int)swap_outs);
    console_put_char('\n');
}
