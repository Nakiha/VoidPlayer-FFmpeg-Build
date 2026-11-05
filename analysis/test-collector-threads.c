/* Generated concurrent records only; includes the real collector and budgets. */
#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include "stream.c"
static AVCodecContext fake_decoder;
static pthread_barrier_t barrier;
static atomic_size_t peak;
typedef struct {unsigned lane,groups,count,shared;} Work;
static void *collect(void *opaque) {
    const Work *work=opaque;
    pthread_barrier_wait(&barrier);
    for(unsigned group=0;group<work->groups&&!failure;group++) {
        VoidPlayerVachunkFrameInfo info={0};
        info.frame_identity=work->shared?1:1+work->lane*work->groups+group;
        info.width=4096;info.height=2304;
        for(unsigned i=0;i<work->count&&!failure;i++) {
            const unsigned key=i+(work->shared?work->lane*work->count:0);
            ff_voidplayer_vachunk_write_intra_cu(&info,(key%1024)*4,(key/1024)*4,4,4,0,key%52,0,0,0,0);
            size_t bytes=atomic_load(&allocated),old=atomic_load(&peak);
            assert(bytes<=MAX_RECORD_BYTES);
            while(old<bytes&&!atomic_compare_exchange_weak(&peak,&old,bytes)){}
        }
        ff_vpa_reference(info.frame_identity,42+work->lane,work->lane%2);
    }
    return NULL;
}
static void run(unsigned lanes,unsigned groups,unsigned count,int shared) {
    pthread_t threads[4];Work work[4];
    decoder=&fake_decoder;
    assert(pthread_barrier_init(&barrier,NULL,lanes+1)==0);
    for(unsigned lane=0;lane<lanes;lane++) {
        work[lane]=(Work){lane,groups,count,shared};
        assert(pthread_create(&threads[lane],NULL,collect,&work[lane])==0);
    }
    pthread_barrier_wait(&barrier);
    for(unsigned lane=0;lane<lanes;lane++)assert(pthread_join(threads[lane],NULL)==0);
    pthread_barrier_destroy(&barrier);
    size_t sum=0;
    for(unsigned i=0;i<MAX_PICTURES;i++)sum+=(size_t)pictures[i].capacity*sizeof(Block);
    assert(sum==atomic_load(&allocated)&&sum<=MAX_RECORD_BYTES);
}
static void reset(void) {
    decoder=NULL;ready=NULL;
    for(unsigned i=0;i<MAX_PICTURES;i++)release(&pictures[i]);
    assert(allocated==0);failure=0;
}
int main(void) {
    pthread_once(&locks_once,init_picture_locks);
    for(unsigned round=0;round<4;round++) {
        run(4,1,20000,0);assert(!failure);
        for(unsigned i=0;i<4;i++)assert(pictures[i].count==20000&&pictures[i].ref_count==1);
        reset();
        run(2,1,20000,1);assert(!failure&&pictures[0].count==40000&&pictures[0].ref_count==2);
        Picture *p=&pictures[0];
        assert(vpa_block_sort(p->blocks,p->count,p->capacity,4096,2304)==0);
        for(unsigned i=0;i<p->count;i++)assert(p->blocks[i].x==(i%1024)*4&&p->blocks[i].y==(i/1024)*4&&p->blocks[i].qp==i%52);
        ready=p;p->sealed=1;assert(vpa_take(40000*12)==40000&&allocated==0);
        reset();
        run(4,8,MAX_BLOCKS,0);assert(failure==AVERROR(ENOMEM));
        assert(allocated>30*1024*1024&&allocated<=MAX_RECORD_BYTES);
        reset();
    }
    printf("PASS threaded collector: separate/shared pictures, slot reuse, concurrent 32 MiB ceiling, retained 0; peak %zu\n",atomic_load(&peak));
    return 0;
}
