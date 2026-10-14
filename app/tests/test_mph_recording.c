/* SPDX-License-Identifier: Apache-2.0 */
/* Synthetic phone packets only. No ADB, real phone, dialogs or Explorer. */
#include "../src/mph_recording.c"
#include <libavcodec/bsf.h>
#include <assert.h>

static void finish(struct mph_recording *r) {
    Uint64 until=SDL_GetTicks()+5000;
    while(mph_recording_state(r)!=MPH_REC_DONE && SDL_GetTicks()<until)SDL_Delay(5);
    assert(mph_recording_state(r)==MPH_REC_DONE);
}
int main(int argc,char **argv) {
    assert(argc==2);assert(SDL_Init(0));
    SDL_SetEnvironmentVariable(SDL_GetEnvironment(),"MPH_RECORDING_TEST","1",true);
    wchar_t temp[2048];assert(GetFullPathNameW(L"work/recording-unit/临时目录",2048,temp,NULL));
    assert(SDL_CreateDirectory("work/recording-unit/临时目录"));
    assert(SetEnvironmentVariableW(L"TMP",temp));assert(SetEnvironmentVariableW(L"TEMP",temp));
    FILE *input=fopen(argv[1],"rb");assert(input);
    uint32_t header[6];assert(fread(header,sizeof(header),1,input)==1);
    struct mph_recording *r=mph_recording_create();assert(r);
    assert(!mph_recording_start(r));
    AVCodecContext *contexts[2]={avcodec_alloc_context3(NULL),avcodec_alloc_context3(NULL)};
    for(int j=0;j<2;++j) {
        assert(contexts[j]);AVCodecContext *ctx=contexts[j];
        ctx->codec_type=j?AVMEDIA_TYPE_AUDIO:AVMEDIA_TYPE_VIDEO;
        ctx->codec_id=j?AV_CODEC_ID_OPUS:AV_CODEC_ID_H264;
        if(j) {ctx->sample_rate=header[2];av_channel_layout_default(&ctx->ch_layout,header[3]);}
        else {ctx->width=header[0];ctx->height=header[1];}
        struct sc_packet_sink *sink=mph_recording_sink(r,j);
        assert(sink->ops->open(sink,ctx,NULL));
        AVPacket *config=av_packet_alloc();assert(config);
        assert(av_new_packet(config,header[4+j])>=0);
        assert(fread(config->data,header[4+j],1,input)==1);config->pts=AV_NOPTS_VALUE;
        assert(sink->ops->push(sink,config));av_packet_free(&config);
    }
    long packets_offset=ftell(input);
    for(int cycle=0;cycle<3;++cycle) {
        assert(mph_recording_start(r));
        assert(mph_recording_state(r)==MPH_REC_STARTING);
        mph_recording_event(r,2000000,"record_start,0,0,0,0,-1");
        if(cycle==2) { /* Cancel while waiting for a keyframe; no deadlock. */
            mph_recording_stop(r);finish(r);assert(r->failure);mph_recording_poll(r,NULL);break;
        }
        assert(!fseek(input,packets_offset,SEEK_SET));
        AVPacket *p=av_packet_alloc();assert(p);
        uint32_t fields[3];int64_t timing[3];
        while(fread(fields,sizeof(fields),1,input)==1) {
            assert(fread(timing,sizeof(timing),1,input)==1);
            assert(av_new_packet(p,fields[2])>=0);assert(fread(p->data,p->size,1,input)==1);
            p->flags=fields[1];p->pts=timing[0];p->dts=timing[1];p->duration=timing[2];
            if(p->pts>=2200000) {
                struct sc_packet_sink *sink=mph_recording_sink(r,fields[0]);
                assert(sink->ops->push(sink,p));
                mph_recording_event(r,p->pts,"touch,1,2,0.65,0.45,1,1,0,0,1,1");
            }
            av_packet_unref(p);
        }
        av_packet_free(&p);
        if(cycle==1) { /* Disconnect saves received data, never rejects demux. */
            mph_recording_sink(r,false)->ops->close(mph_recording_sink(r,false));
        } else mph_recording_stop(r);
        finish(r);
        assert((r->failure!=NULL)==(cycle==1));
        printf("RECORDING=%s\n",r->video);
        mph_recording_poll(r,NULL);assert(mph_recording_state(r)==MPH_REC_IDLE);
        if(cycle==1)r->closed[0]=false; /* Simulate stream still available for cancel test. */
    }
    assert(mph_recording_start(r)); /* Shutdown while worker is still starting. */
    mph_recording_destroy(r);
    for(int j=0;j<2;++j)avcodec_free_context(&contexts[j]);
    fclose(input);SDL_Quit();
    puts("MPH recording PASS: original H264/Opus, mid-GOP start, repeat, disconnect, cancellation, Unicode temp.");
    return 0;
}
