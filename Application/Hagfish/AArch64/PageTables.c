#include <Uefi.h>
#include <Library/UefiLib.h>

#include <Allocation.h>
#include <PageTables.h>
#include <Util.h>

#include <stdint.h>
#include <vm.h>

struct page_tables {
    size_t nL1;

    union aarch64_descriptor *L0_table;
    union aarch64_descriptor **L1_tables;
};

#define BLOCK_16G (ARMv8_HUGE_PAGE_SIZE * 16ULL)

struct page_tables *
build_page_tables(EFI_SYSTEM_TABLE *SystemTable,
                  struct region_list *list) {
    if(list->nregions == 0) {
        AsciiPrint("No memory regions defined.\n");
        goto build_page_tables_fail;
    }

    struct page_tables *tables=
        allocate_zero_pool(sizeof(struct page_tables), EfiLoaderData);
    if(!tables) goto build_page_tables_fail;

    uint64_t first_address, last_address;
    first_address= list->regions[0].base;
    last_address= list->regions[list->nregions-1].base
                + list->regions[list->nregions-1].npages * PAGE_4k - 1;

    AsciiPrint("Window from %llx to %llx\n", first_address, last_address);

    /* We will map in aligned 16G blocks, as each requires only one TLB
     * entry. */
    uint64_t window_start, window_length;
    window_start= first_address & ~BLOCK_16G;
    window_length= ROUNDUP(last_address - window_start, BLOCK_16G);

    AsciiPrint("Mapping from %llx to %llx\n", window_start,
            window_start + window_length - 1);

    tables->L0_table= allocate_pages(1, EfiBarrelfishBootPageTable);
    if(!tables->L0_table) {
        AsciiPrint("Failed to allocate L0 page table.\n");
        goto build_page_tables_fail;
    }
    ZeroMem(tables->L0_table, PAGE_4k);

    /* Count the number of L1 tables (512GB) blocks required to cover the
     * physical mapping window. */
    tables->nL1= 0;
    uint64_t L1base= window_start & ~ARMv8_TOP_TABLE_SIZE;
    uint64_t L1addr;
    for(L1addr= window_start & ~ARMv8_TOP_TABLE_SIZE;
        L1addr < window_start + window_length;
        L1addr+= ARMv8_TOP_TABLE_SIZE) {
        tables->nL1++;
    }

    AsciiPrint("Allocating %d L1 tables\n", tables->nL1);

    /* ALlocate the L1 table pointers. */
    tables->L1_tables=
        allocate_zero_pool(tables->nL1 * sizeof(union aarch64_descriptor *),
                           EfiLoaderData);
    if(!tables->L1_tables) {
        AsciiPrint("Failed to allocate L1 page table pointers.\n");
        goto build_page_tables_fail;
    }

    /* Allocate the L1 tables. */
    size_t i;
    for(i= 0; i < tables->nL1; i++) {
        tables->L1_tables[i]= allocate_pages(1, EfiBarrelfishBootPageTable);
        if(!tables->L1_tables[i]) {
            AsciiPrint("Failed to allocate L1 page tables.\n");
            goto build_page_tables_fail;
        }
        AsciiPrint("  %p\n", tables->L1_tables[i]);
        ZeroMem(tables->L1_tables[i], PAGE_4k);

        /* Map the L1 into the L0. */
        size_t L0_index= (L1base >> ARMv8_TOP_TABLE_BITS) + i;
        tables->L0_table[L0_index].d.base=
            (uint64_t)tables->L1_tables[i] >> ARMv8_BASE_PAGE_BITS;
        tables->L0_table[L0_index].d.mb1=   1; /* Page table */
        tables->L0_table[L0_index].d.valid= 1;
    }

    /* Install the 1GB block mappings. */
    uint64_t firstblock= window_start / ARMv8_HUGE_PAGE_SIZE;
    uint64_t nblocks= window_length / ARMv8_HUGE_PAGE_SIZE;
    uint64_t block;
    for(block= firstblock; block < firstblock + nblocks; block++) {
        size_t table_number= block >> ARMv8_BLOCK_BITS;
        size_t table_index= block & ARMv8_BLOCK_MASK;

        AsciiPrint("%d %d %d %016llx\n", block, table_number, table_index,
                                         block * ARMv8_HUGE_PAGE_SIZE);

        /* We're mapping 16GB contiguous blocks, to save TLB entries. */
        tables->L1_tables[table_number][table_index].block_l1.contiguous= 1;
        tables->L1_tables[table_number][table_index].block_l1.base= block;
        /* Mark the accessed flag, so we don't get a fault. */
        tables->L1_tables[table_number][table_index].block_l1.af= 1;
        /* Outer shareable - coherent. */
        tables->L1_tables[table_number][table_index].block_l1.sh= 2;
        /* EL1+ only. */
        tables->L1_tables[table_number][table_index].block_l1.ap= 0;
        /* Normal memory XXX set up MAIR_EL{1,2}[0]. */
        tables->L1_tables[table_number][table_index].block_l1.attrindex= 0;
        /* A block. */
        tables->L1_tables[table_number][table_index].block_l1.mb0= 0;
        tables->L1_tables[table_number][table_index].block_l1.valid= 1;
    }

    return tables;

build_page_tables_fail:
    if(tables) {
        if(tables->L1_tables) {
            size_t i;
            for(i= 0; i < tables->nL1; i++) {
                if(tables->L1_tables[i]) FreePages(tables->L1_tables[i], 1);
            }
            FreePool(tables->L1_tables);
        }
        if(tables->L0_table) FreePages(tables->L0_table, 1);
        FreePool(tables);
    }

    return NULL;
}

void
free_page_table_bookkeeping(struct page_tables *tables) {
    FreePool(tables->L1_tables);
    FreePool(tables);
}

