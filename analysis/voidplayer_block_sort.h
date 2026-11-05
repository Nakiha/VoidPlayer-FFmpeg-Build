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
static void vpa_block_radix(Block *blocks,uint32_t n,unsigned shift) {
    if(n<24) {
        for(uint32_t i=1;i<n;i++) {
            Block value=blocks[i];uint32_t j=i,key=vpa_block_key(&value);
            while(j&&vpa_block_key(&blocks[j-1])>key){blocks[j]=blocks[j-1];j--;}
            blocks[j]=value;
        }
        return;
    }
    uint32_t count[256]={0},start[256],next[256];
    for(uint32_t i=0;i<n;i++)count[(vpa_block_key(&blocks[i])>>shift)&255]++;
    uint32_t sum=0;
    for(unsigned b=0;b<256;b++){start[b]=next[b]=sum;sum+=count[b];}
    for(unsigned b=0;b<256;b++) {
        const uint32_t end=start[b]+count[b];
        while(next[b]<end) {
            const uint32_t at=next[b];
            const unsigned dest=(vpa_block_key(&blocks[at])>>shift)&255;
            if(dest==b){next[b]++;continue;}
            Block value=blocks[at];blocks[at]=blocks[next[dest]];blocks[next[dest]++]=value;
        }
    }
    if(shift)for(unsigned b=0;b<256;b++)if(count[b]>1)vpa_block_radix(blocks+start[b],count[b],shift-8);
}

/* No heap allocation, ABI change or second record buffer. Strictly sorted
 * keys already have the unique qsort order. Otherwise prove key uniqueness
 * before moving a record, using unused allocation tail or at most 72 KiB of
 * the existing 4 MiB stack. Invalid geometry/keys and duplicate keys call libc
 * qsort on the ORIGINAL sequence, preserving its observable tie behavior.
 * Four bounded in-place radix passes replace comparison sorting for unique
 * coordinates. Scratch bytes never become part of the published records. */
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
    unsigned char scratch[4096u*2304u/128u];
    for(i=0;i<n;i++)
        if(((blocks[i].x|blocks[i].y)&3)||blocks[i].x>=width||blocks[i].y>=height)goto fallback;
    unsigned char *seen=bytes<=(size_t)(capacity-n)*sizeof(Block)?(unsigned char *)(blocks+n):scratch;
    memset(seen,0,bytes);
    for(i=0;i<n;i++) {
        const uint32_t point=(uint32_t)(blocks[i].y/4)*columns+blocks[i].x/4;
        const unsigned char bit=(unsigned char)(1u<<(point&7));
        if(seen[point>>3]&bit)goto fallback;
        seen[point>>3]|=bit;
    }
#ifdef VPA_BLOCK_SORT_TEST_HOOK
    VPA_BLOCK_SORT_TEST_HOOK();
#endif
    vpa_block_radix(blocks,n,24);
    return 0;
fallback:
    VPA_BLOCK_QSORT(blocks,n,sizeof(Block),vpa_block_compare);
    return 0;
}
#endif
