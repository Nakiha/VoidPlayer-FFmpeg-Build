/* LGPL-2.1-or-later. Internal specialization of the 12-byte analysis records. */
#ifndef VOIDPLAYER_BLOCK_SORT_H
#define VOIDPLAYER_BLOCK_SORT_H
#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#define VPA_BLOCK_LIMIT 131072u
typedef struct { uint16_t x,y,w,h; int16_t qp; uint8_t mode,depth; } Block;
_Static_assert(sizeof(Block)==12, "analysis block ABI must remain 12 bytes");

#ifndef VPA_BLOCK_QSORT
#define VPA_BLOCK_QSORT qsort
#endif
static int vpa_block_compare(const void *a,const void *b) {
    const Block *x=a,*y=b;
    return x->y!=y->y?(int)x->y-y->y:(int)x->x-y->x;
}
static uint32_t vpa_block_key(const Block *b) { return ((uint32_t)b->y<<16)|b->x; }
static void vpa_block_sift(Block *blocks,uint32_t root,uint32_t n) {
    Block value=blocks[root];
    while(root<n/2) {
        uint32_t child=root*2+1;
        if(child+1<n&&vpa_block_key(&blocks[child])<vpa_block_key(&blocks[child+1]))child++;
        if(vpa_block_key(&value)>=vpa_block_key(&blocks[child]))break;
        blocks[root]=blocks[child];root=child;
    }
    blocks[root]=value;
}

/* No heap allocation, ABI change or second record buffer. A strict sorted
 * sequence is already the unique qsort result. Otherwise prove uniqueness
 * before moving any record: use a bitset in the unused allocation tail only
 * when it fits. Unsupported coordinates, duplicate keys or insufficient tail
 * space call the original libc qsort on the ORIGINAL record sequence. qsort
 * does not promise stable ties, so sorting first and then falling back would
 * change observable bytes. Tail bytes are outside count and never published.
 * Return -1 only for impossible collector count/capacity/pointer invariants. */
static int vpa_block_sort(Block *blocks,uint32_t n,uint32_t capacity,
                          uint32_t width,uint32_t height) {
    if(n>VPA_BLOCK_LIMIT||capacity>VPA_BLOCK_LIMIT||n>capacity||(!blocks&&n))return -1;
    if(n<2)return 0;
    uint32_t i=1;
    for(;i<n&&vpa_block_key(&blocks[i-1])<vpa_block_key(&blocks[i]);i++){}
    if(i==n)return 0;
    if(!width||width>4096||!height||height>2304)goto fallback;
    const uint32_t columns=(width+3)/4,rows=(height+3)/4;
    const size_t bytes=((size_t)columns*rows+7)/8;
    if(bytes>(size_t)(capacity-n)*sizeof(Block))goto fallback;
    for(i=0;i<n;i++)
        if(((blocks[i].x|blocks[i].y)&3)||blocks[i].x>=width||blocks[i].y>=height)goto fallback;
    unsigned char *seen=(unsigned char *)(blocks+n);
    memset(seen,0,bytes);
    for(i=0;i<n;i++) {
        const uint32_t point=(uint32_t)(blocks[i].y/4)*columns+blocks[i].x/4;
        const unsigned char bit=(unsigned char)(1u<<(point&7));
        if(seen[point>>3]&bit)goto fallback;
        seen[point>>3]|=bit;
    }
    for(i=n/2;i>0;i--)vpa_block_sift(blocks,i-1,n);
#ifdef VPA_BLOCK_SORT_TEST_HOOK
    VPA_BLOCK_SORT_TEST_HOOK(); /* Unit build only: cancel after heap construction. */
#endif
    for(uint32_t end=n;end>1;) {
        Block value=blocks[0];blocks[0]=blocks[--end];blocks[end]=value;
        vpa_block_sift(blocks,0,end);
    }
    return 0;
fallback:
    VPA_BLOCK_QSORT(blocks,n,sizeof(Block),vpa_block_compare);
    return 0;
}
#endif
