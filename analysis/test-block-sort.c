/* Generated records only. Run natively with ASan/UBSan and against WASM libc. */
#include <assert.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static void checked_qsort(void *,size_t,size_t,int (*)(const void *,const void *));
#ifdef __EMSCRIPTEN__
static void cancel_test_heap_enter(void);
#define VPA_BLOCK_SORT_TEST_HOOK cancel_test_heap_enter
#endif
#define VPA_BLOCK_QSORT checked_qsort
#include "voidplayer_block_sort.h"
static const Block *original;
static unsigned calls,tests;
static uint32_t random_state=0x58ac3901;
static uint32_t random32(void) { random_state^=random_state<<13;random_state^=random_state>>17;random_state^=random_state<<5;return random_state; }
static void checked_qsort(void *base,size_t n,size_t width,int (*cmp)(const void *,const void *)) {
    /* A tie fallback must see the untouched original, not a sorted permutation. */
    assert(width==12&&memcmp(base,original,n*width)==0);calls++;
    qsort(base,n,width,cmp);
}
static void check(Block *input,uint32_t n,uint32_t capacity,uint32_t w,uint32_t h,int fallback) {
    const size_t bytes=(size_t)capacity*sizeof(Block),used=(size_t)n*sizeof(Block);
    unsigned char *storage=malloc(bytes+32);
    Block *expected=malloc(used?used:1);
    assert(storage&&expected);
    memset(storage,0xa7,bytes+32);memcpy(storage,input,used);memcpy(expected,input,used);
    original=input;calls=0;
    qsort(expected,n,sizeof(Block),vpa_block_compare);
    assert(vpa_block_sort((Block *)storage,n,capacity,w,h)==0);
    assert(memcmp(storage,expected,used)==0);
    for(size_t i=bytes;i<bytes+32;i++)assert(storage[i]==0xa7);
    assert(calls<=1);if(fallback>=0)assert(calls==(unsigned)fallback);
    free(expected);free(storage);tests++;
}
static Block record(uint32_t key,uint32_t serial) {
    return (Block){(key%1024)*4,(key/1024)*4,(serial%255)+1,((serial*13)%255)+1,
                   (int16_t)(serial*7919),serial*17,serial*31};
}
#ifdef __EMSCRIPTEN__
#include <emscripten.h>
EM_JS(void,sort_cancel_started,(void),{ globalThis.__sortCancelStarted(); });
static int cancel_test_waiting;
static void cancel_test_heap_enter(void) {
    if(cancel_test_waiting){cancel_test_waiting=0;sort_cancel_started();}
}
/* Test-only export. Hard cancellation remains worker termination, not a new
 * synchronous ABI callback. Exercise the largest eligible heap repeatedly. */
void vpa_sort_cancel_work(void) {
    const uint32_t n=VPA_BLOCK_LIMIT-6144;
    Block *blocks=malloc((size_t)VPA_BLOCK_LIMIT*sizeof(Block));assert(blocks);
    cancel_test_waiting=1;
    for(unsigned round=0;round<4096;round++) {
        for(uint32_t i=0;i<n;i++)blocks[i]=record(n-i-1,i);
        original=blocks;
        assert(vpa_block_sort(blocks,n,VPA_BLOCK_LIMIT,4096,2304)==0);
    }
    free(blocks);
}
#endif
int main(void) {
    assert(sizeof(Block)==12&&offsetof(Block,qp)==8&&offsetof(Block,mode)==10&&offsetof(Block,depth)==11);
    Block tiny[8];
    check(tiny,0,0,0,0,0);
    tiny[0]=(Block){65535,65535,65535,65535,INT16_MIN,255,255};check(tiny,1,1,0,0,0);
    /* Exhaust every order of four keys for lengths2..8, with differing payloads. */
    for(uint32_t n=2;n<=8;n++)for(uint32_t pattern=0;pattern<(1u<<(2*n));pattern++) {
        uint32_t p=pattern,seen=0;int duplicate=0;
        for(uint32_t i=0;i<n;i++){uint32_t key=p&3;p>>=2;duplicate|=!!(seen&(1u<<key));seen|=1u<<key;tiny[i]=record(key,i);}
        check(tiny,n,16,16,4,duplicate?1:0);
    }
    /* Same coordinate/different every non-key field, including all-equal keys. */
    for(uint32_t i=0;i<8;i++)tiny[i]=record(3,i);
    check(tiny,8,32,16,4,1);
    tiny[0]=record(4,0);tiny[1]=record(0,1);
    check(tiny,2,2,4096,2304,1); /* no spare bytes */
    check(tiny,2,3,4096,2304,1); /* insufficient spare bytes */
    check(tiny,2,16,UINT32_MAX,2304,1);
    check(tiny,2,16,4096,UINT32_MAX,1);
    check(tiny,2,16,0,0,1);
    tiny[0].x=65535;tiny[0].y=65535;check(tiny,2,16,16,4,1);
    tiny[0]=record(3,0);tiny[0].x=3;check(tiny,2,16,16,4,1); /* non-grid coordinate */
    tiny[0]=record(3,0);check(tiny,2,3,16,4,0); /* exact-small eligible tail */
    /* Bitset end cell, non-multiple-of-four geometry and exact tail boundary. */
    tiny[0]=(Block){4092,2300,4,4,51,3,7};tiny[1]=record(0,1);
    check(tiny,2,6146,4096,2304,0);check(tiny,2,6145,4096,2304,1);
    tiny[0]=(Block){16,4,1,1,51,3,7};check(tiny,2,3,17,5,0);
    /* Thousands of deterministic random orders/payloads, with/without ties. */
    Block *large=malloc((size_t)VPA_BLOCK_LIMIT*sizeof(Block));assert(large);
    for(unsigned trial=0;trial<1000;trial++) {
        uint32_t n=2+random32()%1023;
        for(uint32_t i=0;i<n;i++)large[i]=record(trial&1?random32()%2048:i,i^random32());
        for(uint32_t i=n;i>1;i--){uint32_t j=random32()%i;Block v=large[i-1];large[i-1]=large[j];large[j]=v;}
        check(large,n,n+6144,4096,2304,trial&1?-1:0);
    }
    /* Largest eligible heap and maximum count: ordered/reversed/random/tied. */
    for(unsigned mode=0;mode<4;mode++)for(unsigned full=0;full<2;full++) {
        uint32_t n=full?VPA_BLOCK_LIMIT:VPA_BLOCK_LIMIT-6144;
        for(uint32_t i=0;i<n;i++)large[i]=record(mode==3?0:i,i);
        if(mode==1)for(uint32_t i=0;i<n/2;i++){Block v=large[i];large[i]=large[n-i-1];large[n-i-1]=v;}
        if(mode==2)for(uint32_t i=n;i>1;i--){uint32_t j=random32()%i;Block v=large[i-1];large[i-1]=large[j];large[j]=v;}
        check(large,n,VPA_BLOCK_LIMIT,4096,2304,mode==0?0:full||mode==3?1:0);
    }
    /* Reject count/capacity/index arithmetic overflow before touching a pointer. */
    assert(vpa_block_sort(NULL,0,0,0,0)==0);
    assert(vpa_block_sort(NULL,1,1,1,1)==-1);
    assert(vpa_block_sort(NULL,UINT32_MAX,UINT32_MAX,UINT32_MAX,UINT32_MAX)==-1);
    assert(vpa_block_sort(large,2,1,16,4)==-1);
    assert(vpa_block_sort(large,VPA_BLOCK_LIMIT+1,VPA_BLOCK_LIMIT+1,16,4)==-1);
    assert(vpa_block_sort(large,0,UINT32_MAX,16,4)==-1);
    free(large);printf("PASS block-sort: %u byte-exact cases; duplicate fallbacks preserve original sequence; 12-byte ABI\n",tests);
    return 0;
}
