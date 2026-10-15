/* Exercise the actual collector, independent of encoder-selected block counts.
 * A linker wrapper supplies only the sealed output used by the sort integration
 * check. Normal production decoder validation remains in test-analysis-backend. */
#include <assert.h>
#include <stdio.h>
#ifndef VPA_TEST_COLLECTOR
#define VPA_TEST_COLLECTOR "stream.c"
#endif
#include VPA_TEST_COLLECTOR
static uintptr_t test_frame_id;
int __real_avcodec_receive_frame(AVCodecContext *,AVFrame *);
int __wrap_avcodec_receive_frame(AVCodecContext *context,AVFrame *output) {
    if(!test_frame_id)return __real_avcodec_receive_frame(context,output);
    output->opaque=(void *)test_frame_id;output->pts=123456;
    output->format=AV_PIX_FMT_YUV420P;test_frame_id=0;return 0;
}
static AVCodecContext fake_decoder;
static size_t peak;
static unsigned checks;
static void reset(void) {
    decoder=NULL;ready=NULL;
    for(unsigned i=0;i<MAX_PICTURES;i++)release(&pictures[i]);
    av_frame_free(&frame);failure=0;test_frame_id=0;assert(allocated==0);checks++;
}
static VoidPlayerVachunkFrameInfo info(uintptr_t id,uint32_t width,uint32_t height) {
    VoidPlayerVachunkFrameInfo value={0};value.frame_identity=id;value.width=width;value.height=height;
    return value;
}
static void put(const VoidPlayerVachunkFrameInfo *value,uint32_t i) {
    append(value,(i%1024)*4,(i/1024)*4,4,4,i%52,1,i%16);
    if(allocated>peak)peak=allocated;
}
static int oracle_compare(const void *a,const void *b) {
    const Block *x=a,*y=b;return x->y!=y->y?(int)x->y-y->y:(int)x->x-y->x;
}
int main(void) {
    assert(sizeof(Block)==12&&MAX_BLOCKS==131072&&MAX_RECORD_BYTES==33554432);
    decoder=&fake_decoder;
    VoidPlayerVachunkFrameInfo value=info(1,4096,2304);
    append(&value,4092,2300,4,4,51,3,15);assert(!failure&&pictures[0].count==1);
    append(&value,65535,0,4,4,51,1,0);assert(pictures[0].invalid&&pictures[0].count==1);reset();
    decoder=&fake_decoder;value=info(1,4097,2304);put(&value,0);assert(failure==AVERROR(ENOMEM)&&allocated==0);reset();
    decoder=&fake_decoder;value=info(1,4096,2305);put(&value,0);assert(failure==AVERROR(ENOMEM)&&allocated==0);reset();
    decoder=&fake_decoder;value=info(0,16,16);put(&value,0);assert(failure==AVERROR_INVALIDDATA);reset();
    decoder=&fake_decoder;value=info(1,16,16);append(&value,0,0,0,4,1,1,0);assert(pictures[0].invalid&&!pictures[0].count);reset();
    decoder=&fake_decoder;value=info(1,16,16);put(&value,0);pictures[0].sealed=1;put(&value,1);assert(failure==AVERROR_INVALIDDATA);reset();
    decoder=&fake_decoder;value=info(1,4096,2304);
    for(uint32_t i=0;i<MAX_BLOCKS;i++)put(&value,i);
    assert(!failure&&pictures[0].count==MAX_BLOCKS&&allocated==(size_t)MAX_BLOCKS*12);
    put(&value,MAX_BLOCKS);assert(failure==AVERROR(ENOMEM)&&pictures[0].count==MAX_BLOCKS);reset();
    decoder=&fake_decoder;
    for(unsigned id=1;id<=MAX_PICTURES;id++){value=info(id,16,16);put(&value,0);assert(!failure);}
    value=info(MAX_PICTURES+1,16,16);put(&value,0);assert(failure==AVERROR(ENOMEM));reset();
    decoder=&fake_decoder;
    for(unsigned id=1;id<=MAX_PICTURES&&!failure;id++) {
        value=info(id,4096,2304);
        for(uint32_t i=0;i<MAX_BLOCKS&&!failure;i++)put(&value,i);
    }
    assert(failure==AVERROR(ENOMEM)&&allocated<=MAX_RECORD_BYTES&&allocated==33423360);reset();
    decoder=&fake_decoder;value=info(1,16,16);put(&value,0);ready=&pictures[0];
    assert(vpa_take(-1)==AVERROR(ENOMEM)&&vpa_take(0)==AVERROR(ENOMEM)&&vpa_take(11)==AVERROR(ENOMEM));
    assert(ready&&allocated==256*12);assert(vpa_take(12)==1&&!ready&&allocated==0);reset();
    /* Actual sealing integration, including different-payload duplicate keys. */
    for(unsigned duplicate=0;duplicate<2;duplicate++) {
        decoder=&fake_decoder;frame=av_frame_alloc();assert(frame);value=info(1,1280,720);
        for(uint32_t i=0;i<2100;i++) {
            uint32_t key=2099-i;
            if(duplicate&&i==2099)key=2099;
            append(&value,(key%320)*4,(key/320)*4,4,4,i%52,1+i%3,i%16);
        }
        assert(!failure);size_t before=allocated;Block *expected=malloc(2100*12);assert(expected);
        memcpy(expected,pictures[0].blocks,2100*12);qsort(expected,2100,12,oracle_compare);
        test_frame_id=1;assert(vpa_step()==1&&ready&&vpa_count()==2100&&vpa_depth()==8&&vpa_pts()==123456);
        assert(allocated==before&&memcmp(ready->blocks,expected,2100*12)==0);free(expected);
        assert(vpa_take(2100*12-1)==AVERROR(ENOMEM)&&allocated==before);
        assert(vpa_take(2100*12)==2100&&allocated==0);reset();
    }
    /* Bounded malformed API inputs fail before dereferencing their data. */
    assert(vpa_open(2,NULL,-1)==AVERROR(EINVAL));assert(vpa_open(2,NULL,1024*1024+1)==AVERROR(EINVAL));
    assert(vpa_open(2,NULL,0)==0);
    assert(vpa_feed_known(NULL,0,0,0,3,0)==AVERROR(EINVAL));
    assert(vpa_feed_known(NULL,8*1024*1024+1,0,0,3,0)==AVERROR(EINVAL));
    assert(vpa_feed_known(NULL,1,NAN,0,3,0)==AVERROR(EINVAL));
    assert(vpa_feed_known(NULL,1,0,INFINITY,3,0)==AVERROR(EINVAL));
    assert(vpa_feed_known(NULL,1,0,0,4,0)==AVERROR(EINVAL));
    assert(vpa_feed_known(NULL,1,0,0,3,2000000)==AVERROR(EINVAL));
    assert(vpa_feed_known(NULL,1,0,0,3,-1)==AVERROR(EINVAL));
    vpa_close();assert(allocated==0);checks++;
    printf("PASS collector: %u groups; peak record allocation %zu; retained 0; malformed/budget/seal checks\n",checks,peak);
    return 0;
}
