/*
	Glidix kernel

	Copyright (c) 2014, Madd Games.
	All rights reserved.
	
	Redistribution and use in source and binary forms, with or without
	modification, are permitted provided that the following conditions are met:
	
	* Redistributions of source code must retain the above copyright notice, this
	  list of conditions and the following disclaimer.
	
	* Redistributions in binary form must reproduce the above copyright notice,
	  this list of conditions and the following disclaimer in the documentation
	  and/or other materials provided with the distribution.
	
	THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
	AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
	IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
	DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE
	FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
	DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
	SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
	CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
	OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
	OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*/

#include <glidix/memory.h>
#include <glidix/spinlock.h>
#include <glidix/console.h>
#include <glidix/pagetab.h>
#include <glidix/string.h>
#include <glidix/physmem.h>
#include <stdint.h>

static Spinlock heapLock;

extern int end;			// the type is not important
static uint64_t placement;
static int readyForDynamic;
static PDPT *pdptHeap;

void initMemoryPhase1()
{
	placement = (uint64_t) &end;
	spinlockRelease(&heapLock);
	readyForDynamic = 0;
};

void initMemoryPhase2()
{
	// TODO: xd (execute disable, not the stupid face), we'll look at that shit in a bit.

	// note that for now, kxmalloc() returns physical addresses,
	// so we can be sure that the virtual address returned is equal
	// to the physical address.
	PML4 *pml4 = getPML4();
	PDPT *pdpt = kxmalloc(sizeof(PDPT), MEM_PAGEALIGN);
	pdptHeap = pdpt;
	memset(pdpt, 0, sizeof(PDPT));
	PD *pd = kxmalloc(sizeof(PD), MEM_PAGEALIGN);
	memset(pd, 0, sizeof(PD));
	pdpt->entries[0].present = 1;
	pdpt->entries[0].rw = 1;
	pdpt->entries[0].pdPhysAddr = ((uint64_t)pd >> 12);

	// page table for the first 2MB.
	PT *pt = kxmalloc(sizeof(PT), MEM_PAGEALIGN);
	memset(pt, 0, sizeof(PT));
	pd->entries[0].present = 1;
	pd->entries[0].rw = 1;
	pd->entries[0].ptPhysAddr = ((uint64_t)pt >> 12);

	// map the first 2MB to physical addresses.
	int i;
	for (i=0; i<512; i++)
	{
		pt->entries[i].present = 1;
		pt->entries[i].rw = 1;
		pt->entries[i].framePhysAddr = phmAllocFrame();
	};

	// set it in the PML4 (so it maps from 0x10000000000 up).
	pml4->entries[2].present = 1;
	pml4->entries[2].rw = 1;
	pml4->entries[2].pdptPhysAddr = ((uint64_t)pdpt >> 12);

	refreshAddrSpace();

	// create one giant (2MB) block for the heap.
	HeapHeader *head = (HeapHeader*) 0x10000000000;
	head->magic = HEAP_HEADER_MAGIC;
	head->size = 0x200000 - sizeof(HeapHeader) - sizeof(HeapFooter);
	head->flags = 0;		// this block is free.

	HeapFooter *foot = (HeapFooter*) (0x10000200000 - sizeof(HeapFooter));
	foot->magic = HEAP_FOOTER_MAGIC;
	foot->size = head->size;
	foot->flags = 0;
	
	readyForDynamic = 1;
};

static HeapHeader *heapWalkRight(HeapHeader *head)
{
	uint64_t addr = (uint64_t) head;
	addr += sizeof(HeapHeader) + head->size + sizeof(HeapFooter);

	HeapFooter *foot = (HeapFooter*) (addr - sizeof(HeapFooter));
	if ((foot->flags & HEAP_BLOCK_HAS_RIGHT) == 0)
	{
		// TODO: expand the heap if possible.
		panic("ran out of heap memory!");
	};

	HeapHeader *nextHead = (HeapHeader*) addr;
	if (nextHead->magic != HEAP_HEADER_MAGIC)
	{
		panic("detected heap corruption (invalid header magic)");
	};

	return nextHead;
};

static void heapSplitBlock(HeapHeader *head, size_t size)
{
	// find the current footer
	uint64_t currentHeaderAddr = (uint64_t) head;
	uint64_t currentFooterAddr = currentHeaderAddr + sizeof(HeapHeader) + head->size;
	HeapFooter *currentFooter = (HeapFooter*) currentFooterAddr;

	// change the size of the current header
	head->size = size;

	// decrease the size on the footer (we'll place the appropriate header there in a sec..)
	currentFooter->size -= (size + sizeof(HeapHeader) + sizeof(HeapFooter));

	// get the address of the new footer and header
	uint64_t newFooterAddr = currentHeaderAddr + sizeof(HeapHeader) + size;
	uint64_t newHeaderAddr = newFooterAddr + sizeof(HeapFooter);
	HeapFooter *newFooter = (HeapFooter*) newFooterAddr;
	HeapHeader *newHeader = (HeapHeader*) newHeaderAddr;

	// make the new header
	newHeader->magic = HEAP_HEADER_MAGIC;
	newHeader->flags = HEAP_BLOCK_HAS_LEFT;
	newHeader->size = currentFooter->size;

	// make the new footer
	newFooter->magic = HEAP_FOOTER_MAGIC;
	newFooter->flags = HEAP_BLOCK_HAS_RIGHT;
	newFooter->size = size;
};

static void *kxmallocDynamic(size_t size, int flags)
{
	// TODO: don't ignore the flags!
	void *retAddr = NULL;
	spinlockAcquire(&heapLock);

	// find the first free block.
	HeapHeader *head = (HeapHeader*) 0x10000000000;
	while ((head->flags & HEAP_BLOCK_TAKEN) || (head->size < size))
	{
		head = heapWalkRight(head);
	};

	if (head->size > (size+sizeof(HeapHeader)+sizeof(HeapFooter)+8))
	{
		heapSplitBlock(head, size);
	};

	retAddr = &head[1];		// the memory right after the header is the free block.
	head->flags |= HEAP_BLOCK_TAKEN;

	spinlockRelease(&heapLock);
	return retAddr;
};

static HeapHeader *heapHeaderFromFooter(HeapFooter *foot)
{
	uint64_t footerAddr = (uint64_t) foot;
	uint64_t headerAddr = footerAddr - foot->size - sizeof(HeapHeader);
	return (HeapHeader*) headerAddr;
};

static HeapFooter *heapFooterFromHeader(HeapHeader *head)
{
	uint64_t headerAddr = (uint64_t) head;
	uint64_t footerAddr = headerAddr + sizeof(HeapHeader) + head->size;
	return (HeapFooter*) footerAddr;
};

void *kxmalloc(size_t size, int flags)
{
	if (readyForDynamic)
	{
		return kxmallocDynamic(size, flags);
	};

	spinlockAcquire(&heapLock);

	// align the placement addr on a page boundary if neccessary
	if (flags & MEM_PAGEALIGN)
	{
		if ((placement & 0xFFF) != 0)
		{
			placement &= ~0xFFF;
			placement += 0x1000;
		};
	};

	void *ret = (void*) placement;
	placement += size;
	if (placement > 0x200000)
	{
		panic("placement allocation went beyond 2MB mark!");
	};
	spinlockRelease(&heapLock);
	return ret;
};

void *kmalloc(size_t size)
{
	return kxmalloc(size, 0);
};

void kfree(void *block)
{
	spinlockAcquire(&heapLock);

	uint64_t addr = (uint64_t) block;
	if (addr < (0x10000000000 + sizeof(HeapHeader)))
	{
		panic("invalid pointer passed to kfree(): %a: below heap start", addr);
	};

	HeapHeader *head = (HeapHeader*) (addr - sizeof(HeapHeader));
	if (head->magic != HEAP_HEADER_MAGIC)
	{
		panic("invalid pointer passed to kfree(): %a: lacking or corrupt block header", addr);
	};

	HeapFooter *foot = heapFooterFromHeader(head);
	if (foot->magic != HEAP_FOOTER_MAGIC)
	{
		panic("heap corruption detected: the header for %a is not linked to a valid footer", addr);
	};

	if (foot->size != head->size)
	{
		panic("heap corruption detected: the header for %a does not agree with the footer on block size", addr);
	};

	if ((head->flags & HEAP_BLOCK_TAKEN) == 0)
	{
		panic("invalid pointer passed to kfree(): %a: already free", addr);
	};

	// mark this block as free
	head->flags &= ~HEAP_BLOCK_TAKEN;

	// try to join with adjacent blocks
	HeapHeader *headLeft = NULL;
	HeapFooter *footRight = NULL;

	if (head->flags & HEAP_BLOCK_HAS_LEFT)
	{
		HeapFooter *footLeft = (HeapFooter*) (addr - sizeof(HeapHeader) - sizeof(HeapFooter));
		if (footLeft->magic != HEAP_FOOTER_MAGIC)
		{
			panic("heap corruption detected: block to the left of %a is marked as existing but has invalid footer magic");
		};

		HeapHeader *tmpHead = heapHeaderFromFooter(footLeft);
		if ((tmpHead->flags & HEAP_BLOCK_TAKEN) == 0)
		{
			headLeft = tmpHead;
		};
	};

	if (foot->flags & HEAP_BLOCK_HAS_RIGHT)
	{
		HeapHeader *headRight = (HeapHeader*) &foot[1];
		if (headRight->magic != HEAP_HEADER_MAGIC)
		{
			panic("heap corruption detected: block to the right of %a is marked as existing but has invalid header magic");
		};

		HeapFooter *tmpFoot = heapFooterFromHeader(headRight);
		if ((headRight->flags & HEAP_BLOCK_TAKEN) == 0)
		{
			footRight = tmpFoot;
		};
	};

	if ((headLeft != NULL) && (footRight == NULL))
	{
		// only join with the left block
		size_t newSize = headLeft->size + sizeof(HeapHeader) + sizeof(HeapFooter) + head->size;
		headLeft->size = newSize;
		foot->size = newSize;
	}
	else if ((headLeft == NULL) && (footRight != NULL))
	{
		// only join with the right block
		size_t newSize = head->size + sizeof(HeapHeader) + sizeof(HeapFooter) + footRight->size;
		head->size = newSize;
		footRight->size = newSize;
	}
	else if ((headLeft != NULL) && (footRight != NULL))
	{
		// join with both blocks (ie. join the left and right together, the current head/foot become
		// part of a block).
		size_t newSize = headLeft->size + footRight->size + head->size + 2*sizeof(HeapHeader) + 2*sizeof(HeapFooter);
		headLeft->size = newSize;
		footRight->size = newSize;
	};
	// otherwise no joining.

	spinlockRelease(&heapLock);
};

void heapDump()
{
	// dump the list of blocks to the console.
	HeapHeader *head = (HeapHeader*) 0x10000000000;
	kprintf("---\n");
	kprintf("ADDR                   STAT     SIZE\n");
	while (1)
	{
		uint64_t addr = (uint64_t) &head[1];

		kprintf("%a     ", addr);
		const char *stat = "%$\x02" "FREE%#";
		if (head->flags & HEAP_BLOCK_TAKEN)
		{
			stat = "%$\x04" "USED%#";
		};
		kprintf(stat);
		kprintf("     %d\n", head->size);

		HeapFooter *foot = heapFooterFromHeader(head);
		if (foot->flags & HEAP_BLOCK_HAS_RIGHT)
		{
			head = (HeapHeader*) &foot[1];
		}
		else
		{
			break;
		};
	};
	kprintf("---\n");
};