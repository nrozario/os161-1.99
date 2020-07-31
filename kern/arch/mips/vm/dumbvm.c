/*
 * Copyright (c) 2000, 2001, 2002, 2003, 2004, 2005, 2008, 2009
 *	The President and Fellows of Harvard College.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. Neither the name of the University nor the names of its contributors
 *    may be used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE UNIVERSITY AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE UNIVERSITY OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

#include <types.h>
#include <kern/errno.h>
#include <lib.h>
#include <spl.h>
#include <spinlock.h>
#include <proc.h>
#include <current.h>
#include <mips/tlb.h>
#include <addrspace.h>
#include <vm.h>
#include <copyinout.h>
#include "opt-A2.h"
#include "opt-A3.h"
/*
 * Dumb MIPS-only "VM system" that is intended to only be just barely
 * enough to struggle off the ground.
 */

/* under dumbvm, always have 48k of user stack */
#define DUMBVM_STACKPAGES    12

/*
 * Wrap rma_stealmem in a spinlock.
 */
static struct spinlock stealmem_lock = SPINLOCK_INITIALIZER;

#if OPT_A3
static int *coremap;
static int numberOfPages;
static bool isCoremapReady = false;
static vaddr_t start;
#endif

	void
vm_bootstrap(void)
{
#if OPT_A3
	paddr_t lo, hi;
	ram_getsize(&lo, &hi);
	lo = ROUNDUP(lo, PAGE_SIZE);
	KASSERT((lo % PAGE_SIZE) == 0);
	int n = (hi - lo) / PAGE_SIZE;
	hi = lo + n * PAGE_SIZE;
	KASSERT((hi % PAGE_SIZE) == 0);
	int coremapSize = n * sizeof(int);
	coremapSize = ROUNDUP(coremapSize, PAGE_SIZE);
	start = lo;
	coremap = (int *)(PADDR_TO_KVADDR(start));
	lo += coremapSize;
	KASSERT((lo % PAGE_SIZE) == 0);
	numberOfPages = (hi - lo) / PAGE_SIZE;
	for (int i = 0; i < numberOfPages; i++){
		coremap[i] = 0;
	}
	isCoremapReady = true;
#endif
	/* Do nothing. */
}

static
	paddr_t
getppages(unsigned long npages)
{
	paddr_t addr;
#if OPT_A3
	if (isCoremapReady){
		addr = 0;
		int i = 0;
		while ((int)(i + npages) < numberOfPages){
			if (coremap[i] == 0){
				bool complete = true;
				for (unsigned j = 0; j < npages; j++){
					if (coremap[i + j] != 0){
						i += j + 1;
						complete = false;
					}
				}
				if (complete){
					for (unsigned j = 0; j < npages; j++){
						coremap[i + j] = npages - j;
					}
					addr = start + i * PAGE_SIZE;
					break;
				}
			}else{
				i++;
			}
		}
	}else{
#endif
		spinlock_acquire(&stealmem_lock);

		addr = ram_stealmem(npages);

		spinlock_release(&stealmem_lock);
#if OPT_A3
	}
#endif
	return addr;
}

/* Allocate/free some kernel-space virtual pages */
	vaddr_t 
alloc_kpages(int npages)
{
	paddr_t pa;
	pa = getppages(npages);
	if (pa==0) {
		return 0;
	}
	vaddr_t toReturn = PADDR_TO_KVADDR(pa);
	(void)toReturn;
	return PADDR_TO_KVADDR(pa);
}

	void 
free_kpages(vaddr_t addr)
{
#if OPT_A3
	if (isCoremapReady){
		vaddr_t vStart = PADDR_TO_KVADDR(start);
		(void)vStart;
		int i = (addr - PADDR_TO_KVADDR(start)) / PAGE_SIZE;
		KASSERT(i < numberOfPages);
		int n = coremap[i];
		KASSERT((i + n) < numberOfPages);
		for (int j = 0; j < n; j++){
			coremap[i + j] = 0;
		}
	}else{
#endif
		/* nothing - leak the memory. */

		(void)addr;
#if OPT_A3
	}
#endif
}


	void
vm_tlbshootdown_all(void)
{
	panic("dumbvm tried to do tlb shootdown?!\n");
}

	void
vm_tlbshootdown(const struct tlbshootdown *ts)
{
	(void)ts;
	panic("dumbvm tried to do tlb shootdown?!\n");
}

	int
vm_fault(int faulttype, vaddr_t faultaddress)
{
	vaddr_t vbase1, vtop1, vbase2, vtop2, stackbase, stacktop;
	paddr_t paddr;
	int i;
	uint32_t ehi, elo;
	struct addrspace *as;
	int spl;

	faultaddress &= PAGE_FRAME;

	DEBUG(DB_VM, "dumbvm: fault: 0x%x\n", faultaddress);

	switch (faulttype) {
		case VM_FAULT_READONLY:
#if OPT_A3
			return EFAULT;
#else
			/* We always create pages read-write, so we can't get this */
			panic("dumbvm: got VM_FAULT_READONLY\n");
#endif
		case VM_FAULT_READ:
		case VM_FAULT_WRITE:
			break;
		default:
			return EINVAL;
	}

	if (curproc == NULL) {
		/*
		 * No process. This is probably a kernel fault early
		 * in boot. Return EFAULT so as to panic instead of
		 * getting into an infinite faulting loop.
		 */
		return EFAULT;
	}

	as = curproc_getas();
	if (as == NULL) {
		/*
		 * No address space set up. This is probably also a
		 * kernel fault early in boot.
		 */
		return EFAULT;
	}

	/* Assert that the address space has been set up properly. */
#if OPT_A3
	KASSERT(as->as_pt1 != NULL);
	KASSERT(as->as_pt2 != NULL);
	KASSERT(as->as_stackpt != NULL);
	for (unsigned i = 0; i < as->as_npages1; i++){
		KASSERT((as->as_pt1[i].frame & PAGE_FRAME) == as->as_pt1[i].frame);
	}
	for (unsigned i = 0; i < as->as_npages2; i++){
		KASSERT((as->as_pt2[i].frame & PAGE_FRAME) == as->as_pt2[i].frame);
	}
	for (int i = 0; i < DUMBVM_STACKPAGES; i++){
		KASSERT((as->as_stackpt[i].frame & PAGE_FRAME) == as->as_stackpt[i].frame);
	}
#else
	KASSERT(as->as_pbase1 != 0);
	KASSERT(as->as_pbase2 != 0);
	KASSERT(as->as_stackpbase != 0);
	KASSERT((as->as_pbase1 & PAGE_FRAME) == as->as_pbase1);
	KASSERT((as->as_pbase2 & PAGE_FRAME) == as->as_pbase2);
	KASSERT((as->as_stackpbase & PAGE_FRAME) == as->as_stackpbase);
#endif

	KASSERT(as->as_vbase1 != 0);
	KASSERT(as->as_npages1 != 0);
	KASSERT(as->as_vbase2 != 0);
	KASSERT(as->as_npages2 != 0);
	KASSERT((as->as_vbase1 & PAGE_FRAME) == as->as_vbase1);
	KASSERT((as->as_vbase2 & PAGE_FRAME) == as->as_vbase2);

	vbase1 = as->as_vbase1;
	vtop1 = vbase1 + as->as_npages1 * PAGE_SIZE;
	vbase2 = as->as_vbase2;
	vtop2 = vbase2 + as->as_npages2 * PAGE_SIZE;
	stackbase = USERSTACK - DUMBVM_STACKPAGES * PAGE_SIZE;
	stacktop = USERSTACK;

#if OPT_A3        
	bool isCodeSegment = false;
#endif
	if (faultaddress >= vbase1 && faultaddress < vtop1) {
#if OPT_A3
		isCodeSegment = true;
		int i = (faultaddress - vbase1) / PAGE_SIZE;
		paddr = as->as_pt1[i].frame;
#else
		paddr = (faultaddress - vbase1) + as->as_pbase1;
#endif
	}
	else if (faultaddress >= vbase2 && faultaddress < vtop2) {
#if OPT_A3
		int i = (faultaddress - vbase2) / PAGE_SIZE;
		paddr = as->as_pt2[i].frame;
#else
		paddr = (faultaddress - vbase2) + as->as_pbase2;
#endif
	}
	else if (faultaddress >= stackbase && faultaddress < stacktop) {
#if OPT_A3
		int i = (faultaddress - stackbase) / PAGE_SIZE;
		paddr = as->as_stackpt[i].frame;
#else
		paddr = (faultaddress - stackbase) + as->as_as_stackbase;
#endif
	}
	else {
		return EFAULT;
	}

	/* make sure it's page-aligned */
	KASSERT((paddr & PAGE_FRAME) == paddr);
	/* Disable interrupts on this CPU while frobbing the TLB. */
	spl = splhigh();
	for (i=0; i<NUM_TLB; i++) {
		tlb_read(&ehi, &elo, i);
		if (elo & TLBLO_VALID) {
			continue;
		}
		ehi = faultaddress;
		elo = paddr | TLBLO_DIRTY | TLBLO_VALID;
#if OPT_A3
		if (isCodeSegment && as->isLoadComplete){
			elo &= ~TLBLO_DIRTY;
		}
#endif
		DEBUG(DB_VM, "dumbvm: 0x%x -> 0x%x\n", faultaddress, paddr);
		tlb_write(ehi, elo, i);
		splx(spl);
		return 0;
	}
#if OPT_A3
	ehi = faultaddress;
	elo = paddr | TLBLO_DIRTY | TLBLO_VALID;
	if (isCodeSegment && as->isLoadComplete){
		elo &= ~TLBLO_DIRTY;
	}
	tlb_random(ehi, elo);
	DEBUG(DB_VM, "dumbvm: 0x%x -> 0x%x\n", faultaddress, paddr);
	splx(spl);
	return 0;
#else
	kprintf("dumbvm: Ran out of TLB entries - cannot handle page fault\n");
	splx(spl);
	return EFAULT;
#endif
}

	struct addrspace *
as_create(void)
{
	struct addrspace *as = kmalloc(sizeof(struct addrspace));
	if (as==NULL) {
		return NULL;
	}
#if OPT_A3
	as->isLoadComplete = false;
	as->as_stackpt = NULL;
	as->as_pt1 = NULL;
	as->as_pt2 = NULL;
#else
	as->as_pbase1 = 0;
	as->as_pbase2 = 0;
	as->as_stackpbase = 0;
#endif
	as->as_vbase1 = 0;
	as->as_npages1 = 0;
	as->as_vbase2 = 0;
	as->as_npages2 = 0;
	return as;
}

	void
as_destroy(struct addrspace *as)
{
#if OPT_A3
	for (unsigned i = 0; i < as->as_npages1; i++){
		if (as->as_pt1[i].isValid){
			free_kpages(PADDR_TO_KVADDR(as->as_pt1[i].frame));
		}
	}
	for (unsigned i = 0; i < as->as_npages2; i++){
		if (as->as_pt2[i].isValid){
			free_kpages(PADDR_TO_KVADDR(as->as_pt2[i].frame));
		}
	}
	for (int i = 0; i < DUMBVM_STACKPAGES; i++){
		if (as->as_stackpt[i].isValid){
			free_kpages(PADDR_TO_KVADDR(as->as_stackpt[i].frame));
		}
	}
	kfree(as->as_pt1);
	kfree(as->as_pt2);
	kfree(as->as_stackpt);
	kfree(as);
#else
	kfree(as);
#endif
}

	void
as_activate(void)
{
	int i, spl;
	struct addrspace *as;

	as = curproc_getas();
#ifdef UW
	/* Kernel threads don't have an address spaces to activate */
#endif
	if (as == NULL) {
		return;
	}

	/* Disable interrupts on this CPU while frobbing the TLB. */
	spl = splhigh();

	for (i=0; i<NUM_TLB; i++) {
		tlb_write(TLBHI_INVALID(i), TLBLO_INVALID(), i);
	}

	splx(spl);
}

	void
as_deactivate(void)
{
	/* nothing */
}

	int
as_define_region(struct addrspace *as, vaddr_t vaddr, size_t sz,
		int readable, int writeable, int executable)
{
	size_t npages; 

	/* Align the region. First, the base... */
	sz += vaddr & ~(vaddr_t)PAGE_FRAME;
	vaddr &= PAGE_FRAME;

	/* ...and now the length. */
	sz = (sz + PAGE_SIZE - 1) & PAGE_FRAME;

	npages = sz / PAGE_SIZE;

	/* We don't use these - all pages are read-write */
	(void)readable;
	(void)writeable;
	(void)executable;

	if (as->as_vbase1 == 0) {
		as->as_vbase1 = vaddr;
#if OPT_A3
		as->as_pt1 = (struct pt_entry *)(kmalloc(npages * sizeof(struct pt_entry)));
		if (as->as_pt1 == NULL){
			return ENOMEM;
		}
#endif
		as->as_npages1 = npages;
		return 0;
	}

	if (as->as_vbase2 == 0) {
		as->as_vbase2 = vaddr;		
#if OPT_A3
		as->as_pt2 = (struct pt_entry *)(kmalloc(npages * sizeof(struct pt_entry)));
		if (as->as_pt2 == NULL){
			return ENOMEM;
		}
#endif
		as->as_npages2 = npages;
		return 0;
	}
	/*
	 * Support for more than two regions is not available.
	 */
	kprintf("dumbvm: Warning: too many regions\n");
	return EUNIMP;
}

static
	void
as_zero_region(paddr_t paddr, unsigned npages)
{
	bzero((void *)PADDR_TO_KVADDR(paddr), npages * PAGE_SIZE);
}

	int
as_prepare_load(struct addrspace *as)
{
#if OPT_A3
	KASSERT(as->as_stackpt == NULL);
#else
	KASSERT(as->as_pbase1 == 0);
	KASSERT(as->as_pbase2 == 0);
	KASSERT(as->as_stackpbase == 0);
#endif 

	// TEXT SEGMENT
#if OPT_A3
	for (unsigned i = 0; i < as->as_npages1; i++){
		as->as_pt1[i].frame = getppages(1);
		as->as_pt1[i].isValid = !(as->as_pt1[i].frame == 0);
	}
#else
	as->as_pbase1 = getppages(as->as_npages1);	
	if (as->as_pbase1 == 0) {
		return ENOMEM;
	}
#endif


	// DATA SEGMENT
#if OPT_A3
	for (unsigned i = 0; i < as->as_npages2; i++){
		as->as_pt2[i].frame = getppages(1);
		as->as_pt2[i].isValid = !(as->as_pt2[i].frame == 0);
	}
#else
	as->as_pbase2 = getppages(as->as_npages2);
	if (as->as_pbase2 == 0) {
		return ENOMEM;
	}
#endif

	// STACK SEGMENT	 
	as->as_stackpt = (struct pt_entry *)(kmalloc(DUMBVM_STACKPAGES * sizeof(struct pt_entry)));
	if (as->as_stackpt == NULL){
		return ENOMEM;
	}
#if OPT_A3
	for (int i = 0; i < DUMBVM_STACKPAGES; i++){
		as->as_stackpt[i].frame = getppages(1);
		as->as_stackpt[i].isValid = !(as->as_stackpt[i].frame == 0);
	}
#else
	as->as_stackpbase = getppages(DUMBVM_STACKPAGES);
	if (as->as_stackpbase == 0) {
		return ENOMEM;
	}
#endif
#if OPT_A3
	for (unsigned i = 0; i < as->as_npages1; i++){
		as_zero_region(as->as_pt1[i].frame, 1);
	}
	for (unsigned i = 0; i < as->as_npages2; i++){
		as_zero_region(as->as_pt2[i].frame, 1);
	}
	for (int i = 0; i < DUMBVM_STACKPAGES; i++){
		as_zero_region(as->as_stackpt[i].frame, 1);
	}
#else	
	as_zero_region(as->as_pbase1, as->as_npages1);
	as_zero_region(as->as_pbase2, as->as_npages2);
	as_zero_region(as->as_stackpbase, DUMBVM_STACKPAGES);
#endif
	return 0;
}

	int
as_complete_load(struct addrspace *as)
{
	(void)as;
	return 0;
}

	int
as_define_stack(struct addrspace *as, vaddr_t *stackptr)
{
#if OPT_A3
	KASSERT(as->as_stackpt != NULL);
#else
	KASSERT(as->as_stackpbase != 0);
#endif
	*stackptr = USERSTACK;
	return 0;
}
#if OPT_A2
	int
as_define_args(struct addrspace *as, char **args, int argc, vaddr_t *stackptr)
{
#if OPT_A3
	KASSERT(as->as_stackpt != NULL);
#else
	KASSERT(as->as_stackpbase != 0);
#endif

	vaddr_t temp = USERSTACK;
	int arg_size = 0;
	for(int i = argc - 1; i >= 0; i--){
		size_t len = 0;                	
		arg_size += strlen(args[i]) + 1;
		copyoutstr(args[i], (userptr_t)(temp - arg_size), 128, &len);
	}
	arg_size = ROUNDUP(arg_size, 4);
	temp = temp - arg_size;
	char ** toInsert = (char **)(kmalloc(sizeof(char *)));
	*toInsert = NULL;
	arg_size = 0;
	copyout(toInsert, (userptr_t)(temp - 4), 4);
	temp = temp - 4;
	for (int i = argc - 1; i >= 0; i--){
		arg_size += strlen(args[i]) + 1;
		*toInsert = (char *)(USERSTACK - arg_size);
		copyout(toInsert, (userptr_t)(temp - 4), 4);
		temp = temp - 4;
	}
	as->argv = (char **)(temp);
	arg_size = ROUNDUP(arg_size, 4);
	arg_size += (argc + 1) * 4;
	arg_size = ROUNDUP(arg_size, 8);
	*stackptr = USERSTACK - arg_size;
	kfree(toInsert);
	return 0;


}
#endif

	int
as_copy(struct addrspace *old, struct addrspace **ret)
{
	struct addrspace *new;

	new = as_create();
	if (new==NULL) {
		return ENOMEM;
	}

	new->as_vbase1 = old->as_vbase1;
	new->as_npages1 = old->as_npages1;
	new->as_vbase2 = old->as_vbase2;
	new->as_npages2 = old->as_npages2;

#if OPT_A3
	new->as_pt1 = (struct pt_entry *)(kmalloc(new->as_npages1 * sizeof(struct pt_entry)));
	new->as_pt2 = (struct pt_entry *)(kmalloc(new->as_npages2 * sizeof(struct pt_entry)));
	new->as_stackpt = (struct pt_entry *)(kmalloc(DUMBVM_STACKPAGES * sizeof(struct pt_entry)));
	if (new->as_pt1 == NULL || new->as_pt2 == NULL || new->as_stackpt == NULL){
		return ENOMEM;
	}

	for (unsigned i = 0; i < new->as_npages1; i++){
		new->as_pt1[i].frame = getppages(1);
		new->as_pt1[i].isValid = !(new->as_pt1[i].frame == 0);
		memcpy((void *)PADDR_TO_KVADDR(new->as_pt1[i].frame),
				(const void *)PADDR_TO_KVADDR(old->as_pt1[i].frame),
				PAGE_SIZE);
	}	 
	for (unsigned i = 0; i < new->as_npages2; i++){
		new->as_pt2[i].frame = getppages(1);
		new->as_pt2[i].isValid = !(new->as_pt2[i].frame == 0);
		memcpy((void *)PADDR_TO_KVADDR(new->as_pt2[i].frame),
				(const void *)PADDR_TO_KVADDR(old->as_pt2[i].frame),
				PAGE_SIZE);
	}
	for (int i = 0; i < DUMBVM_STACKPAGES; i++){
		new->as_stackpt[i].frame = getppages(1);
		new->as_stackpt[i].isValid = !(new->as_stackpt[i].frame == 0);
		memcpy((void *)PADDR_TO_KVADDR(new->as_stackpt[i].frame),
				(const void *)PADDR_TO_KVADDR(old->as_stackpt[i].frame),
				PAGE_SIZE);
	}
#else
	/* (Mis)use as_prepare_load to allocate some physical memory. */
	if (as_prepare_load(new)) {
		as_destroy(new);
		return ENOMEM;
	}

	KASSERT(new->as_pbase1 != 0);
	KASSERT(new->as_pbase2 != 0);
	KASSERT(new->as_stackpbase != 0);

	memmove((void *)PADDR_TO_KVADDR(new->as_pbase1),
			(const void *)PADDR_TO_KVADDR(old->as_pbase1),
			old->as_npages1*PAGE_SIZE);

	memmove((void *)PADDR_TO_KVADDR(new->as_pbase2),
			(const void *)PADDR_TO_KVADDR(old->as_pbase2),
			old->as_npages2*PAGE_SIZE);

	memmove((void *)PADDR_TO_KVADDR(new->as_stackpbase),
			(const void *)PADDR_TO_KVADDR(old->as_stackpbase),
			DUMBVM_STACKPAGES*PAGE_SIZE);
#endif	
	*ret = new;
	return 0;
}
