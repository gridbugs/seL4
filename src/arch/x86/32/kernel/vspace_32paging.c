/*
 * Copyright 2014, General Dynamics C4 Systems
 *
 * This software may be distributed and modified according to the terms of
 * the GNU General Public License version 2. Note that NO WARRANTY is provided.
 * See "LICENSE_GPLv2.txt" for details.
 *
 * @TAG(GD_GPL)
 */

#include <api/syscall.h>
#include <config.h>
#include <machine/io.h>
#include <kernel/boot.h>
#include <kernel/cspace.h>
#include <kernel/thread.h>
#include <kernel/cdt.h>
#include <model/statedata.h>
#include <object/cnode.h>
#include <arch/api/invocation.h>
#include <arch/kernel/apic.h>
#include <arch/kernel/vspace.h>
#include <arch/linker.h>
#include <util.h>

#ifndef CONFIG_PAE_PAGING

/* setup initial boot page directory */

/* The boot pd is referenced by code that runs before paging, so
 * place it in PHYS_DATA */
pde_t _boot_pd[BIT(PD_BITS)] ALIGN(BIT(PAGE_BITS)) VISIBLE PHYS_DATA;

BOOT_CODE
pde_t *get_boot_pd()
{
    return _boot_pd;
}

/* This function is duplicated from pde_pde_large_ptr_new, generated by the
 * bitfield tool in structures_gen.h. It is required by functions that need to
 * call it before the MMU is turned on. Any changes made to the bitfield
 * generation need to be replicated here.
 */
PHYS_CODE
static inline void
pde_pde_large_ptr_new_phys(pde_t *pde_ptr, uint32_t page_base_address,
                           uint32_t pat, uint32_t avl, uint32_t global, uint32_t dirty,
                           uint32_t accessed, uint32_t cache_disabled, uint32_t write_through,
                           uint32_t super_user, uint32_t read_write, uint32_t present)
{
    pde_ptr->words[0] = 0;

    pde_ptr->words[0] |= (page_base_address & 0xffc00000) >> 0;
    pde_ptr->words[0] |= (pat & 0x1) << 12;
    pde_ptr->words[0] |= (avl & 0x7) << 9;
    pde_ptr->words[0] |= (global & 0x1) << 8;
    pde_ptr->words[0] |= (pde_pde_large & 0x1) << 7;
    pde_ptr->words[0] |= (dirty & 0x1) << 6;
    pde_ptr->words[0] |= (accessed & 0x1) << 5;
    pde_ptr->words[0] |= (cache_disabled & 0x1) << 4;
    pde_ptr->words[0] |= (write_through & 0x1) << 3;
    pde_ptr->words[0] |= (super_user & 0x1) << 2;
    pde_ptr->words[0] |= (read_write & 0x1) << 1;
    pde_ptr->words[0] |= (present & 0x1) << 0;
}

PHYS_CODE VISIBLE void
init_boot_pd(void)
{
    word_t i;

    /* identity mapping from 0 up to PPTR_BASE (virtual address) */
    for (i = 0; i < (PPTR_BASE >> IA32_4M_bits); i++) {
        pde_pde_large_ptr_new_phys(
            _boot_pd + i,
            i << IA32_4M_bits, /* physical address */
            0, /* pat            */
            0, /* avl            */
            1, /* global         */
            0, /* dirty          */
            0, /* accessed       */
            0, /* cache_disabled */
            0, /* write_through  */
            0, /* super_user     */
            1, /* read_write     */
            1  /* present        */
        );
    }

    /* mapping of PPTR_BASE (virtual address) to PADDR_BASE up to end of virtual address space */
    for (i = 0; i < ((-PPTR_BASE) >> IA32_4M_bits); i++) {
        pde_pde_large_ptr_new_phys(
            _boot_pd + i + (PPTR_BASE >> IA32_4M_bits),
            (i << IA32_4M_bits) + PADDR_BASE, /* physical address */
            0, /* pat            */
            0, /* avl            */
            1, /* global         */
            0, /* dirty          */
            0, /* accessed       */
            0, /* cache_disabled */
            0, /* write_through  */
            0, /* super_user     */
            1, /* read_write     */
            1  /* present        */
        );
    }
}

BOOT_CODE void
map_it_pd_cap(cap_t pd_cap)
{
    /* this shouldn't be called, and it does nothing */
    fail("Should not be called");
}

/* ==================== BOOT CODE FINISHES HERE ==================== */

lookupPDSlot_ret_t lookupPDSlot(void *vspace, vptr_t vptr)
{
    lookupPDSlot_ret_t pdSlot;
    pde_t *pd = PDE_PTR(vspace);
    unsigned int pdIndex;

    pdIndex = vptr >> (PAGE_BITS + PT_BITS);
    pdSlot.status = EXCEPTION_NONE;
    pdSlot.pdSlot = pd + pdIndex;
    pdSlot.pd = pd;
    pdSlot.pdIndex = pdIndex;
    return pdSlot;
}

bool_t CONST isVTableRoot(cap_t cap)
{
    return cap_get_capType(cap) == cap_page_directory_cap;
}

bool_t CONST isValidNativeRoot(cap_t cap)
{
    return isVTableRoot(cap);
}

bool_t CONST isValidVTableRoot(cap_t cap)
{
#ifdef CONFIG_VTX
    if (cap_get_capType(cap) == cap_ept_page_directory_pointer_table_cap) {
        return true;
    }
#endif
    return isValidNativeRoot(cap);
}

void *getValidNativeRoot(cap_t vspace_cap)
{
    if (isValidNativeRoot(vspace_cap)) {
        return PDE_PTR(cap_page_directory_cap_get_capPDBasePtr(vspace_cap));
    }
    return NULL;
}

void copyGlobalMappings(void* new_vspace)
{
    word_t i;
    pde_t *newPD = (pde_t*)new_vspace;

    for (i = PPTR_BASE >> IA32_4M_bits; i < BIT(PD_BITS); i++) {
        newPD[i] = ia32KSkernelPD[i];
    }
}

void unmapPageDirectory(pdpte_t *pdpt, uint32_t pdptIndex, pde_t *pd)
{
}

void unmapAllPageDirectories(pdpte_t *pdpt)
{
}

void flushAllPageDirectories(pdpte_t *pdpt)
{
}

void flushPageSmall(pte_t *pt, uint32_t ptIndex)
{
    cap_t threadRoot;
    cte_t *ptCte;
    pde_t *pd;
    uint32_t pdIndex;

    /* We know this pt can only be mapped into one single pd. So
     * lets find a cap with that mapping information */
    ptCte = cdtFindWithExtra(cap_page_table_cap_new(0, 0, PT_REF(pt)));
    if (ptCte) {
        pd = PD_PTR(cap_page_table_cap_get_capPTMappedObject(ptCte->cap));
        pdIndex = cap_page_table_cap_get_capPTMappedIndex(ptCte->cap);

        /* check if page belongs to current address space */
        threadRoot = TCB_PTR_CTE_PTR(ksCurThread, tcbVTable)->cap;
        if (isValidNativeRoot(threadRoot) && (void*)pptr_of_cap(threadRoot) == pd) {
            invalidateTLBentry( (pdIndex << (PT_BITS + PAGE_BITS)) | (ptIndex << PAGE_BITS));
            invalidatePageStructureCache();
        }
    }
}

void flushPageLarge(pde_t *pd, uint32_t pdIndex)
{
    cap_t               threadRoot;

    /* check if page belongs to current address space */
    threadRoot = TCB_PTR_CTE_PTR(ksCurThread, tcbVTable)->cap;
    if (cap_get_capType(threadRoot) == cap_page_directory_cap &&
            PDE_PTR(cap_page_directory_cap_get_capPDBasePtr(threadRoot)) == pd) {
        invalidateTLBentry(pdIndex << (PT_BITS + PAGE_BITS));
        invalidatePageStructureCache();
    }
}

void flushAllPageTables(pde_t *pd)
{
    cap_t threadRoot;
    /* check if this is the current address space */
    threadRoot = TCB_PTR_CTE_PTR(ksCurThread, tcbVTable)->cap;
    if (cap_get_capType(threadRoot) == cap_page_directory_cap &&
            PDE_PTR(cap_page_directory_cap_get_capPDBasePtr(threadRoot)) == pd) {
        invalidateTLB();
    }
    invalidatePageStructureCache();
}

void flushPageDirectory(pdpte_t *pdpt, uint32_t pdptIndex, pde_t *pd)
{
    flushAllPageTables(pd);
}

exception_t
decodeIA32PageDirectoryInvocation(
    word_t label,
    word_t length,
    cte_t* cte,
    cap_t cap,
    extra_caps_t extraCaps,
    word_t* buffer
)
{
    current_syscall_error.type = seL4_IllegalOperation;
    return EXCEPTION_SYSCALL_ERROR;
}

#endif
