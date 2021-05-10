/*
 * memory.c
 *
 * Note:
 * There is no separate swap area. When a data page is swapped out, it is
 * stored in the location it was loaded from in the process' image. This means
 * it's impossible to start two processes from the same image without messing
 * up the already executing process. It also means that we cannot use the
 * program disk anymore.
 *
 * Best viewed with tabs set to 4 spaces.
 */

#include "common.h"
#include "kernel.h"
#include "scheduler.h"
#include "memory.h"
#include "thread.h"
#include "util.h"
#include "interrupt.h"
#include "tlb.h"
#include "usb/scsi.h"
#include "usb/error.h"
#include "usb/debug.h"


/* Physical Memory status struct
 * contains information about physical memory blocks
 */
struct PMS {
    uint32_t vaddr; // Current Virtual Address
    uint32_t paddr; // Physical Address
    pcb_t pcb; // The pcb that uses this block
    bool_t pinned; // If the memory block is pinned and unswappable
};
struct PMS memoryblocks[PAGEABLE_PAGES];

/* Contains "kernel" paging */
static pcb_t kernel;
#define PROCESS_ENTRY 0x1000000
static lock_t memory_lock;

/* Use virtual address to get index in page directory.  */
static inline uint32_t get_directory_index(uint32_t vaddr)
{
    return (vaddr & PAGE_DIRECTORY_MASK) >> PAGE_DIRECTORY_BITS;
}

/*
 * Use virtual address to get index in a page table.  The bits are
 * masked, so we essentially get a modulo 1024 index.  The selection
 * of which page table to index into is done with
 * get_directory_index().
 */
static inline uint32_t get_table_index(uint32_t vaddr)
{
    return (vaddr & PAGE_TABLE_MASK) >> PAGE_TABLE_BITS;
}
 /* Updates a entry in a page table or directory
 * params:
 *   uint32_t* table : the table or directory to update
 *   uint32_t index : index in table/directory
 *   uint32_t vaddr : Virtual Address
 *   uint32_t paddr : Physical Address
 *   uint32_t flags : The new bit flags for the entry
 */
void update_entry(uint32_t* table, uint32_t index, uint32_t vaddr,
                  uint32_t paddr, uint32_t flags) {
    table[index] = (paddr & PE_BASE_ADDR_MASK) | (flags & MODE_MASK);
    flush_tlb_entry(vaddr);
}

/* Returns a entry in a page table, the location and sectors for the USB-Drive
 * params:
 *   uint32_t vaddr : Virtual Address
 *   pcb_t* pcb : PCB that owns table
 *   uint32_t* location : uint32_t to write location to
 *   uint32_t* sectors  : uint32_t to write sectors to
 * returns: uint32_t* pointer to table entry
 */
uint32_t* get_entry_and_location(uint32_t vaddr, pcb_t* pcb, uint32_t* location, uint32_t* sectors) {
        uint32_t* entry = (uint32_t*)
            (pcb->page_directory[get_directory_index(vaddr)] & PE_BASE_ADDR_MASK);

        // Writing/Reading from the faulting address sector directly does not work
        // so i am aligning the faulting address to a 8 Sector space (a page can hold 8 sectors)
        uint32_t sector_offset = (vaddr - PROCESS_ENTRY) / SECTOR_SIZE;
        uint32_t aligned_offset = sector_offset / SECTORS_PER_PAGE;
        aligned_offset *= SECTORS_PER_PAGE;
        // If we are on the last bit and theres less than 8 sectors to read/write
        *sectors = SECTORS_PER_PAGE + aligned_offset > pcb->swap_size
            ? pcb->swap_size - aligned_offset : SECTORS_PER_PAGE;
        *location = pcb->swap_loc + aligned_offset;
        return entry;
}

/* counts how many blocks we have allocated */
static uint32_t allocated = 0;

/* Allocates a new memory block if we have free memory / pageable pages
 * If not it will swap out a unpinned pageable page
 * params:
 *   bool_t pinned : If the block should be pinned
 *   uint32_t vaddr : Virtual Address for the block
 *   pcb_t pcb : PCB that takes ownership of the block
 * returns: uint32_t physical address for the memory block
 */
uint32_t get_memory(bool_t pinned, uint32_t vaddr, pcb_t pcb) {
    uint32_t i = allocated;
    if(allocated < PAGEABLE_PAGES) {
        uint32_t addr = MEM_START + (allocated * PAGE_SIZE);
        memoryblocks[i].paddr = addr;
        allocated++;
    } else {
        uint32_t unpinned[PAGEABLE_PAGES];
        int counter = 0;
        for(int i = 0; i < PAGEABLE_PAGES; i++) {
            if(memoryblocks[i].pinned == FALSE) {
                unpinned[counter] = i;
                counter++;
            }
        }
        if(counter == 0) {
            scrprintf(0,40,"PID %i : No unpinned memory free", pcb.pid);
            lock_release(&memory_lock);
            exit();
            return 0;
        }

        // Get a random block from unpinned blocks
        srand(get_timer());
        uint32_t r = rand() % counter;
        i = unpinned[r];

        uint32_t location;
        uint32_t sectors;
        uint32_t* entry = get_entry_and_location(memoryblocks[i].vaddr, &memoryblocks[i].pcb, &location, &sectors);
        uint32_t index = get_table_index(memoryblocks[i].vaddr);
        int dirty = entry[index] & PE_D;
        // Reset flags for the task that was using this page.
        // Since we are doing this completly random we might remove a page current task is using :(
        // We can just reset all flags since we set them all when we add
        update_entry(entry,index, memoryblocks[i].vaddr, memoryblocks[i].paddr, 0);

        if(dirty) {
            // Write page to file
            scsi_write(location, sectors, (void*)memoryblocks[i].paddr);
        }
    }
    memoryblocks[i].pcb = pcb;
    memoryblocks[i].pinned = pinned;
    memoryblocks[i].vaddr = vaddr;
    bzero(memoryblocks[i].paddr, PAGE_SIZE);
    return memoryblocks[i].paddr;
}

/* create_table
 * creates a new table if one does not exist, otherwise updates and returns existing table
 * params:
 *   uint32_t addr : Virtual Address
 *   pcb_t pcb : PCB it belongs to
 *   uint32_t flags : Flags for table
 * returns: uint32_t the table
 */
uint32_t create_table(uint32_t addr, pcb_t pcb, uint32_t flags) {
    uint32_t index = get_directory_index(addr);
    uint32_t table = pcb.page_directory[index];
    if((table & PE_P) == 0) {
        table = get_memory(TRUE, addr, kernel);
    }
    update_entry(pcb.page_directory, index, addr, table, flags);
    return table;
}
/*
 * init_memory()
 *
 * called once by _start() in kernel.c
 * You need to set up the virtual memory map for the kernel here.
 */
void init_memory(void)
{
    lock_init(&memory_lock);
    kernel.page_directory = get_memory(TRUE, 0, kernel);
    uint32_t paddr = 0;
    for(int i = 0; i < N_KERNEL_PTS; i++) {
        uint32_t table = create_table(paddr, kernel, (PE_P | PE_RW));
        for(int x = 0; x < PAGE_N_ENTRIES; x++) {
            uint32_t index = get_table_index(paddr);
            //Set video memory access for processes
            if(paddr == SCREEN_ADDR) {
                update_entry(table, index, paddr, paddr, (PE_P | PE_RW | PE_US));
                uint32_t l = get_directory_index(paddr);
                kernel.page_directory[l] |= PE_US;
            }else {
                update_entry(table, index, paddr, paddr, (PE_P | PE_RW));
            }
            paddr += PAGE_SIZE;
        }
    }
}



/*
 * This function identity maps memory ranges. It is used by
 * the USB subsystem to map device registers in high memory.
 *
 * It is only the EHCI (USB 2.0) host controller driver that
 * needs this function. The UHCI (USB 1.1) host controller
 * driver registers are in low memory, and should be mapped
 * in automatically with the first 4 MB of memory. Since it
 * is only on hardware that the EHCI driver is necessary,
 * this function is NOT required to make the project work in
 * bochs. It must, however, be implemented for the project
 * to run on hardware.
 *
  *
 * @address:  A physical location for device registers. This
 *            address must be mapped one-to-one to virtual
 *            memory.
 * @size:     Size of the physical memory that it is required
 *            to map.
 */
int identity_map(uint32_t address, uint32_t size)
{
    // Shouldnt need locks here since this is called before we start scheduling
    uint32_t nrOfPages = (size + PAGE_SIZE -1) / PAGE_SIZE;
    uint32_t nrOfTables = (nrOfPages / PAGE_N_ENTRIES) + 1;
    uint32_t pagesAdded = 0;
    uint32_t addr = address;

    // It should only be 1 table and 1 page but i added the for loops just in case.
    for(uint32_t i = 0; i < nrOfTables; i++) {
        uint32_t table = create_table(addr, kernel, (PE_P | PE_RW | PE_US));
        for(int i = 0; (i < PAGE_N_ENTRIES) && pagesAdded < nrOfPages; i++) {
            uint32_t index = get_table_index(addr);
            update_entry(table, index, addr, addr, (PE_P | PE_RW | PE_US));
            addr += PAGE_SIZE;
            pagesAdded++;
        }
   }
   return SUCCESS;
}

void setup_page_table(pcb_t *p)
{
    lock_acquire(&memory_lock);
    if(p->is_thread) {
        p->page_directory = kernel.page_directory;
    }else {
        p->page_directory = get_memory(TRUE, 0, *p);
        // Set pointer to kernel pages
        // We copy over all entries, because of identity_map
        // Then we just write over/replace empty entries below
        for(int i = 0; i < PAGE_N_ENTRIES; i++) {
            p->page_directory[i] = kernel.page_directory[i];
        }

        // Adding table for stack
       uint32_t table = create_table(PROCESS_STACK, *p, (PE_P | PE_RW | PE_US));
        // Adding stack pages, presented
        for(int j = 0; j < 2; j++) {
            uint32_t stackaddr = PROCESS_STACK - (j * PAGE_SIZE);
            uint32_t index = get_table_index(stackaddr);
            uint32_t page = get_memory(TRUE, stackaddr, *p);
            update_entry(table, index, stackaddr, page, (PE_P | PE_RW | PE_US));
        }

        //How many pages do we need for data/code
        uint32_t nrOfPages = (p->swap_size + SECTORS_PER_PAGE -1) / SECTORS_PER_PAGE;
        uint32_t nrOfTables = (nrOfPages / PAGE_N_ENTRIES) + 1;

        uint32_t pagesAdded = 0;
        uint32_t vaddr = PROCESS_ENTRY;
        for(uint32_t i = 0; i < nrOfTables; i++) {
           uint32_t table = create_table(vaddr, *p, (PE_P | PE_RW | PE_US));
            // Adding code pages, not presented
            for(int i = 0; (i < PAGE_N_ENTRIES) && pagesAdded < nrOfPages; i++) {
                uint32_t index = get_table_index(vaddr);
                update_entry(table, index, vaddr, 0, (PE_RW | PE_US));
                vaddr += PAGE_SIZE;
                pagesAdded++;
            }
        }
    }
    lock_release(&memory_lock);
}

/*
 * called by exception_14 in interrupt.c (the faulting address is in
 * current_running->fault_addr)
 *
 * Interrupts are on when calling this function.
 */
void page_fault_handler(void)
{
    lock_acquire(&memory_lock);
    current_running->page_fault_count++;

    //scrprintf(7,0,"Faulting addr: %X, %i", current_running->fault_addr, current_running->pid);

    /* Some error messages for page fault */
    if(current_running->fault_addr == 0) {
        scrprintf(0,30,"PID: %i : Null pointer error", current_running->pid);
        lock_release(&memory_lock);
        exit();
        return; // not reached i guess
    }
    if((current_running->error_code & (PE_P)) != 0) {
        scrprintf(0,30,"PID: %i : Access Denied %x", current_running->pid, current_running->fault_addr);
        lock_release(&memory_lock);
        exit();
        return;
    }
    // Some sources says this bit is reserved/not used
    /*
    if((current_running->error_code & (PE_PWT)) != 0) {
        scrprintf(0,30,
                  "PID: %i : Writing to read-only memory: %x", current_running->pid, current_running->fault_addr);
        lock_release(&memory_lock);
        exit();
        return;
    }
    */

    // Get page table entry and disk information
    uint32_t location;
    uint32_t sectors;
    uint32_t* entry = get_entry_and_location(current_running->fault_addr, current_running, &location, &sectors);

    // Get a page to write to
    uint32_t page = get_memory(FALSE, current_running->fault_addr, *current_running);

    // Read inn from disk
    scsi_read(location, sectors, (void*)page);

    // Update page table entry
    uint32_t index = get_table_index(current_running->fault_addr);
    update_entry(entry, index, current_running->fault_addr, page, (PE_P | PE_RW | PE_US));
    lock_release(&memory_lock);
}
