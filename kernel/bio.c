// Buffer cache.
//
// The buffer cache is a hash table of buf structures holding
// cached copies of disk block contents.  Caching disk blocks
// in memory reduces the number of disk reads and also provides
// a synchronization point for disk blocks used by multiple processes.
//
// Interface:
// * To get a buffer for a particular disk block, call bread.
// * After changing buffer data, call bwrite to write it to disk.
// * When done with the buffer, call brelse.
// * Do not use the buffer after calling brelse.
// * Only one process at a time can use a buffer,
//     so do not keep them longer than necessary.


#include "types.h"
#include "param.h"
#include "spinlock.h"
#include "sleeplock.h"
#include "riscv.h"
#include "defs.h"
#include "fs.h"
#include "buf.h"

#define NBUCKET NBUF

// Hash function to map (dev, blockno) to a bucket
static uint
hash(uint dev, uint blockno)
{
  return (dev + blockno) % NBUCKET;
}

struct {
  struct spinlock lock[NBUCKET]; 
  struct buf buf[NBUF];
  
  struct buf head[NBUCKET];
} bcache;

void
binit(void)
{
  struct buf *b;
  int i;
  char lockname[16];

  // Initialize per-bucket locks
  for(i = 0; i < NBUCKET; i++) {
    snprintf(lockname, sizeof(lockname), "bcache_%d", i);
    initlock(&bcache.lock[i], lockname);
    
    // Initialize each bucket head as an empty circular list
    bcache.head[i].prev = &bcache.head[i];
    bcache.head[i].next = &bcache.head[i];
  }

  // Initialize all buffers and assign one buffer to each bucket
  for(b = bcache.buf; b < bcache.buf+NBUF; b++) {
    int bucket_idx = b - bcache.buf;  // Each buffer gets its own bucket
    
    // Initialize buffer fields
    initsleeplock(&b->lock, "buffer");
    snprintf(lockname, sizeof(lockname), "buf_%d", bucket_idx);
    initlock(&b->spinlock, lockname);
    
    // Assign this buffer to its corresponding bucket
    b->next = bcache.head[bucket_idx].next;
    b->prev = &bcache.head[bucket_idx];
    bcache.head[bucket_idx].next->prev = b;
    bcache.head[bucket_idx].next = b;
    
    // Initialize buffer state
    b->dev = 0;
    b->blockno = 0;
    b->refcnt = 0;
    b->valid = 0;
  }
}

// Look through buffer cache for block on device dev.
// If not found, allocate a buffer.
// In either case, return locked buffer.
static struct buf*
bget(uint dev, uint blockno)
{
  struct buf *b;
  uint bucket = hash(dev, blockno);

  acquire(&bcache.lock[bucket]);

  // Is the block already cached in this bucket?
  for(b = bcache.head[bucket].next; b != &bcache.head[bucket]; b = b->next){
    if(b->dev == dev && b->blockno == blockno){
      b->refcnt++;
      release(&bcache.lock[bucket]);
      acquiresleep(&b->lock);
      return b;
    }
  }

  // Not cached in this bucket.
  // Look for an unused buffer in this bucket first
  for(b = bcache.head[bucket].next; b != &bcache.head[bucket]; b = b->next){
    if(b->refcnt == 0) {
      b->dev = dev;
      b->blockno = blockno;
      b->valid = 0;
      b->refcnt = 1;
      release(&bcache.lock[bucket]);
      acquiresleep(&b->lock);
      return b;
    }
  }

  // No unused buffer in this bucket, look in other buckets
  release(&bcache.lock[bucket]);
  
  // Search all buckets for an unused buffer
  for(int i = 0; i < NBUCKET; i++) {
    acquire(&bcache.lock[i]);
    for(b = bcache.head[i].next; b != &bcache.head[i]; b = b->next){
      if(b->refcnt == 0) {
        // Move buffer from current bucket to target bucket
        // Remove from current bucket
        b->next->prev = b->prev;
        b->prev->next = b->next;
        release(&bcache.lock[i]);
        
        // Add to target bucket
        acquire(&bcache.lock[bucket]);
        b->next = bcache.head[bucket].next;
        b->prev = &bcache.head[bucket];
        bcache.head[bucket].next->prev = b;
        bcache.head[bucket].next = b;
        
        // Set buffer data
        b->dev = dev;
        b->blockno = blockno;
        b->valid = 0;
        b->refcnt = 1;
        release(&bcache.lock[bucket]);
        acquiresleep(&b->lock);
        return b;
      }
    }
    release(&bcache.lock[i]);
  }
  
  panic("bget: no buffers");
}

// Return a locked buf with the contents of the indicated block.
struct buf*
bread(uint dev, uint blockno)
{
  struct buf *b;

  b = bget(dev, blockno);
  if(!b->valid) {
    virtio_disk_rw(b, 0);
    b->valid = 1;
  }
  return b;
}

// Write b's contents to disk.  Must be locked.
void
bwrite(struct buf *b)
{
  if(!holdingsleep(&b->lock))
    panic("bwrite");
  virtio_disk_rw(b, 1);
}

void
brelse(struct buf *b)
{
  if(!holdingsleep(&b->lock))
    panic("brelse");

  releasesleep(&b->lock);

  uint bucket = hash(b->dev, b->blockno);
  acquire(&bcache.lock[bucket]);
  b->refcnt--;
  release(&bcache.lock[bucket]);
}

void
bpin(struct buf *b) {
  uint bucket = hash(b->dev, b->blockno);
  acquire(&bcache.lock[bucket]);
  b->refcnt++;
  release(&bcache.lock[bucket]);
}

void
bunpin(struct buf *b) {
  uint bucket = hash(b->dev, b->blockno);
  acquire(&bcache.lock[bucket]);
  b->refcnt--;
  release(&bcache.lock[bucket]);
}


