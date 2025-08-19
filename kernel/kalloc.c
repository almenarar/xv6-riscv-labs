// Physical memory allocator, for user processes,
// kernel stacks, page-table pages,
// and pipe buffers. Allocates whole 4096-byte pages.

#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "spinlock.h"
#include "riscv.h"
#include "defs.h"

void freerange(void *pa_start, void *pa_end);
void distribute_pages(void);

extern char end[]; // first address after kernel.
                   // defined by kernel.ld.

struct run {
  struct run *next;
};

struct {
  struct spinlock lock;
  struct run *freelist;
} kmem_list[NCPU];

void
kinit()
{
  char buffer[10];

  for(int i = 0; i< NCPU; i++) {
    snprintf(buffer, sizeof(buffer), "kmem_%d", i);
    initlock(&kmem_list[i].lock, buffer);
  }
  
  freerange(end, (void*)PHYSTOP);
}

void
freerange(void *pa_start, void *pa_end)
{
  char *p;
  p = (char*)PGROUNDUP((uint64)pa_start);
  
  // First count how many pages we have
  int total_pages = 0;
  char *count_p = p;
  for(; count_p + PGSIZE <= (char*)pa_end; count_p += PGSIZE) {
    total_pages++;
  }
  
  int pages_per_cpu = total_pages / NCPU;
  
  // Distribute pages across CPUs as they are freed
  int current_cpu = 0;
  int pages_on_current_cpu = 0;
  
  for(; p + PGSIZE <= (char*)pa_end; p += PGSIZE) {
    struct run *r = (struct run*)p;
    memset(p, 1, PGSIZE);
    
    acquire(&kmem_list[current_cpu].lock);
    r->next = kmem_list[current_cpu].freelist;
    kmem_list[current_cpu].freelist = r;
    release(&kmem_list[current_cpu].lock);
    
    pages_on_current_cpu++;
    if(pages_on_current_cpu >= pages_per_cpu && current_cpu < NCPU-1) {
      current_cpu++;
      pages_on_current_cpu = 0;
    }
  }
}

// Free the page of physical memory pointed at by pa,
// which normally should have been returned by a
// call to kalloc().  (The exception is when
// initializing the allocator; see kinit above.)
void
kfree(void *pa)
{
  struct run *r;

  if(((uint64)pa % PGSIZE) != 0 || (char*)pa < end || (uint64)pa >= PHYSTOP)
    panic("kfree");

  // Fill with junk to catch dangling refs.
  memset(pa, 1, PGSIZE);

  r = (struct run*)pa;

  push_off();
  int id = cpuid();
  pop_off();
  
  acquire(&kmem_list[id].lock);
  r->next = kmem_list[id].freelist;
  kmem_list[id].freelist = r;
  release(&kmem_list[id].lock);
}

// Allocate one 4096-byte page of physical memory.
// Returns a pointer that the kernel can use.
// Returns 0 if the memory cannot be allocated.
void *
kalloc(void)
{
  struct run *r;
  
  push_off();
  int original_cpu = cpuid();
  pop_off();

  // Try to allocate from current CPU's freelist first
  acquire(&kmem_list[original_cpu].lock);
  r = kmem_list[original_cpu].freelist;
  if(r) {
    kmem_list[original_cpu].freelist = r->next;
    memset((char*)r, 5, PGSIZE); 
    release(&kmem_list[original_cpu].lock);
    return (void*)r;
  }

  release(&kmem_list[original_cpu].lock);

  for(int i = 0; i < NCPU; i++) {
      if(i == original_cpu) continue; // Skip current CPU, already tried
      
      acquire(&kmem_list[i].lock);
      r = kmem_list[i].freelist;
      if(r) {
        kmem_list[i].freelist = r->next;
        release(&kmem_list[i].lock);
        break;
      }
      release(&kmem_list[i].lock);
    }

  return (void*)r;
}