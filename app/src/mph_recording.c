/* SPDX-License-Identifier: Apache-2.0 */
/* Original compressed phone streams + machine-readable diagnostics. No encoder. */
#include "mph_recording.h"
#include <libavformat/avformat.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>
#include <io.h>

#define QUEUE_BYTES (32u*1024u*1024u)
#define QUEUE_ITEMS 16384u
struct rec_item {
    struct rec_item *next;
    AVPacket *packet;
    int stream;
    Uint64 ns;
    int64_t video_pts;
    char event[768];
};
struct rec_sink {
    struct sc_packet_sink sink;
    struct mph_recording *owner;
    int stream;
};
struct mph_recording {
    struct rec_sink sinks[2];
    SDL_Mutex *mutex;
    SDL_Condition *cond;
    SDL_Thread *thread;
    enum mph_recording_state state;
    AVCodecParameters *params[2], *snapshot[2];
    bool opened[2], closed[2];
    struct rec_item *head, *tail;
    size_t bytes;
    unsigned items;
    Uint64 start_ns;
    const char *failure;
    char folder[3500], video[4096];
};
static void fail_locked(struct mph_recording *r,const char *error) {
    if(!r->failure) r->failure=error;
    if(r->state==MPH_REC_STARTING || r->state==MPH_REC_ACTIVE) r->state=MPH_REC_STOPPING;
    SDL_SignalCondition(r->cond);
}
static bool running(struct mph_recording *r) {
    return r->state==MPH_REC_STARTING || r->state==MPH_REC_ACTIVE;
}
static void free_item(struct rec_item *i) {
    av_packet_free(&i->packet);free(i);
}
static void queue_locked(struct mph_recording *r,struct rec_item *i) {
    size_t bytes=sizeof(*i)+(i->packet?(size_t)i->packet->size:0);
    if(r->bytes+bytes>QUEUE_BYTES || r->items>=QUEUE_ITEMS) {
        free_item(i);fail_locked(r,"录制写入跟不上，已停止以保护投屏。日志可能不完整。");return;
    }
    if(r->tail) r->tail->next=i; else r->head=i;
    r->tail=i;r->bytes+=bytes;++r->items;SDL_SignalCondition(r->cond);
}
static bool sink_open(struct sc_packet_sink *s,AVCodecContext *ctx,const struct sc_stream_session *session) {
    (void)session;
    struct rec_sink *sink=(struct rec_sink *)s;struct mph_recording *r=sink->owner;
    SDL_LockMutex(r->mutex);
    avcodec_parameters_free(&r->params[sink->stream]);
    r->params[sink->stream]=avcodec_parameters_alloc();
    r->opened[sink->stream]=r->params[sink->stream] && avcodec_parameters_from_context(r->params[sink->stream],ctx)>=0;
    r->closed[sink->stream]=false;
    SDL_UnlockMutex(r->mutex);return true;
}
static void sink_close(struct sc_packet_sink *s) {
    struct rec_sink *sink=(struct rec_sink *)s;struct mph_recording *r=sink->owner;
    SDL_LockMutex(r->mutex);r->closed[sink->stream]=true;
    if(running(r)) fail_locked(r,"手机音视频流已结束，录制提前停止，已保存收到的数据。");
    SDL_UnlockMutex(r->mutex);
}
static bool sink_session(struct sc_packet_sink *s,const struct sc_stream_session *session) {
    struct rec_sink *sink=(struct rec_sink *)s;struct mph_recording *r=sink->owner;
    SDL_LockMutex(r->mutex);
    AVCodecParameters *p=r->params[sink->stream];
    if(p && (p->width!=(int)session->video.width || p->height!=(int)session->video.height)) {
        if(running(r)) fail_locked(r,"手机画面尺寸发生变化，本段录制已停止。可重新开始录制。");
        p->width=session->video.width;p->height=session->video.height;
    }
    SDL_UnlockMutex(r->mutex);return true;
}
static bool sink_push(struct sc_packet_sink *s,const AVPacket *packet) {
    struct rec_sink *sink=(struct rec_sink *)s;struct mph_recording *r=sink->owner;
    SDL_LockMutex(r->mutex);
    if(packet->pts==AV_NOPTS_VALUE) {
        AVCodecParameters *p=r->params[sink->stream];
        if(p && (p->extradata_size!=packet->size || !p->extradata || memcmp(p->extradata,packet->data,packet->size))) {
            if(running(r)) fail_locked(r,"音视频编码参数发生变化，本段录制已停止。可重新开始录制。");
            av_freep(&p->extradata);p->extradata_size=0;
            p->extradata=av_mallocz(packet->size+AV_INPUT_BUFFER_PADDING_SIZE);
            if(p->extradata) {memcpy(p->extradata,packet->data,packet->size);p->extradata_size=packet->size;}
        }
    } else if(running(r)) {
        struct rec_item *i=calloc(1,sizeof(*i));
        if(i) {
            i->packet=av_packet_clone(packet);i->stream=sink->stream;i->ns=SDL_GetTicksNS();
            if(i->packet) queue_locked(r,i);
            else {free(i);fail_locked(r,"录制内存分配失败。");}
        } else fail_locked(r,"录制内存分配失败。");
    }
    SDL_UnlockMutex(r->mutex);return true; /* Recording must never break live demux. */
}
static FILE *open_utf8(const char *path,const wchar_t *mode) {
    wchar_t w[4096];
    return MultiByteToWideChar(CP_UTF8,0,path,-1,w,4096)?_wfopen(w,mode):NULL;
}
static bool write_metadata(struct mph_recording *r,FILE *meta,int64_t origin,unsigned video_packets,unsigned audio_packets,bool incomplete) {
    if(fseek(meta,0,SEEK_SET))return false;
    SDL_LockMutex(r->mutex);
    const char *reason=r->failure?r->failure:incomplete?"recording_or_interrupted":"stopped";
    SDL_UnlockMutex(r->mutex);
    fprintf(meta,"{\n\"schema\":1,\n\"container\":\"matroska\",\n\"video_file\":\"phone.mkv\",\n\"events_file\":\"events.csv\",\n\"pc_start_us\":%llu,\n\"media_origin_pts_us\":%lld,\n\"video_packets\":%u,\n\"audio_packets\":%u,\n\"video_codec\":\"%s\",\n\"audio_codec\":\"%s\",\n\"width\":%d,\n\"height\":%d,\n\"incomplete\":%s,\n\"stop_reason\":\"%s\",\n\"queue_accepted_means\":\"PC queue only, not Android or game acknowledgement\",\n\"coordinates\":\"normalized phone frame; mouse event x/y are relative PC deltas\",\n\"synchronization\":\"media seconds = (phone_pts_us - media_origin_pts_us) / 1000000; packet rows use received PTS; UI rows use last displayed frame PTS; pc_time_us is independent monotonic PC clock\"\n}\n",
            (unsigned long long)(r->start_ns/1000),(long long)origin,video_packets,audio_packets,
            avcodec_get_name(r->snapshot[0]->codec_id),avcodec_get_name(r->snapshot[1]->codec_id),
            r->snapshot[0]->width,r->snapshot[0]->height,incomplete?"true":"false",reason);
    return !ferror(meta) && !fflush(meta) && !_chsize_s(_fileno(meta),ftell(meta));
}
static int SDLCALL worker(void *userdata) {
    struct mph_recording *r=userdata;AVFormatContext *ctx=NULL;
    FILE *events=NULL,*meta=NULL;bool header=false;int64_t origin=AV_NOPTS_VALUE;
    unsigned video_packets=0,audio_packets=0;bool failed=false;Uint64 last_flush=0;
    char path[4096];
    snprintf(path,sizeof(path),"%s/events.csv",r->folder);events=open_utf8(path,L"wb");
    snprintf(path,sizeof(path),"%s/metadata.json",r->folder);meta=open_utf8(path,L"wb");
    if(meta && !write_metadata(r,meta,origin,0,0,true))goto error;
    if(!events || !meta || avformat_alloc_output_context2(&ctx,NULL,"matroska",r->video)<0 || !ctx) goto error;
    fprintf(events,"pc_time_us,phone_pts_us,event,id,action,x,y,queue_accepted,mapping,suspended,focus_lost,relative_mouse,look_down,pending_x,pending_y,refresh_phase,reanchors,rejected,frame_count,last_frame_pc_us,look_origin_x,look_origin_y,sensitivity,radius,arrow_visible\n");
    for(int j=0;j<2;++j) {
        AVStream *st=avformat_new_stream(ctx,NULL);
        if(!st || avcodec_parameters_copy(st->codecpar,r->snapshot[j])<0) goto error;
        st->time_base=(AVRational){1,1000000};st->codecpar->codec_tag=0;
    }
    AVDictionary *options=NULL;av_dict_set(&options,"cluster_time_limit","1000",0);
    int opened=avio_open(&ctx->pb,r->video,AVIO_FLAG_WRITE);
    int written=opened<0?opened:avformat_write_header(ctx,&options);
    av_dict_free(&options);if(written<0)goto error;
    header=true;
    for(;;) {
        SDL_LockMutex(r->mutex);
        while(!r->head && running(r)) SDL_WaitCondition(r->cond,r->mutex);
        struct rec_item *i=r->head;
        if(!i) {SDL_UnlockMutex(r->mutex);break;}
        r->head=i->next;if(!r->head)r->tail=NULL;
        r->bytes-=sizeof(*i)+(i->packet?(size_t)i->packet->size:0);--r->items;
        SDL_UnlockMutex(r->mutex);
        if(i->packet) {
            AVPacket *p=i->packet;
            fprintf(events,"%llu,%lld,packet,%d,%d,%d\n",(unsigned long long)(i->ns/1000),(long long)p->pts,i->stream,p->flags,p->size);
            if(origin==AV_NOPTS_VALUE && i->stream==0 && (p->flags&AV_PKT_FLAG_KEY)) {
                origin=p->pts;
                if(!write_metadata(r,meta,origin,0,0,true)) {free_item(i);goto error;}
                SDL_LockMutex(r->mutex);
                if(r->state==MPH_REC_STARTING) r->state=MPH_REC_ACTIVE;
                SDL_UnlockMutex(r->mutex);
            }
            if(origin!=AV_NOPTS_VALUE && p->pts>=origin) {
                p->pts-=origin;p->dts-=origin;
                p->stream_index=i->stream;
                if(!p->duration && i->stream==0) p->duration=16667;
                av_packet_rescale_ts(p,(AVRational){1,1000000},ctx->streams[i->stream]->time_base);
                if(av_interleaved_write_frame(ctx,p)<0) {free_item(i);goto error;}
                if(i->stream==0) ++video_packets; else ++audio_packets;
            }
        } else fprintf(events,"%llu,%lld,%s\n",(unsigned long long)(i->ns/1000),(long long)i->video_pts,i->event);
        if(i->ns-last_flush>=500000000) {
            last_flush=i->ns;
            if(fflush(events)) {free_item(i);goto error;}
            avio_flush(ctx->pb);
        }
        free_item(i);
        if(ferror(events)) goto error;
    }
    goto finish;
error:
    failed=true;
    SDL_LockMutex(r->mutex);fail_locked(r,"录制文件写入失败，请检查临时文件夹的可用空间。");SDL_UnlockMutex(r->mutex);
finish:
    if(header && av_write_trailer(ctx)<0) failed=true;
    if(ctx) {if(ctx->pb && avio_closep(&ctx->pb)<0)failed=true;avformat_free_context(ctx);}
    if(events && fclose(events))failed=true;
    SDL_LockMutex(r->mutex);
    if(!video_packets || !audio_packets) fail_locked(r,"未收到完整的手机音视频数据。本次诊断日志仍予以保留。");
    if(failed) fail_locked(r,"录制文件未能完整写入，请检查磁盘空间。");
    SDL_UnlockMutex(r->mutex);
    if(meta) {
        bool written=write_metadata(r,meta,origin,video_packets,audio_packets,r->failure!=NULL);
        bool bad=!written;if(fclose(meta))bad=true;
        if(bad) {SDL_LockMutex(r->mutex);fail_locked(r,"诊断元数据写入失败。");SDL_UnlockMutex(r->mutex);}
    }
    SDL_LockMutex(r->mutex);
    while(r->head) {struct rec_item *i=r->head;r->head=i->next;free_item(i);}
    r->tail=NULL;r->bytes=0;r->items=0;r->state=MPH_REC_DONE;
    SDL_UnlockMutex(r->mutex);return 0;
}
struct mph_recording *mph_recording_create(void) {
    struct mph_recording *r=calloc(1,sizeof(*r));if(!r)return NULL;
    r->mutex=SDL_CreateMutex();r->cond=SDL_CreateCondition();
    if(!r->mutex || !r->cond) {mph_recording_destroy(r);return NULL;}
    static const struct sc_packet_sink_ops ops={.open=sink_open,.close=sink_close,.push=sink_push,.push_session=sink_session,.disable=sink_close};
    for(int j=0;j<2;++j) {r->sinks[j].sink.ops=&ops;r->sinks[j].owner=r;r->sinks[j].stream=j;}
    return r;
}
struct sc_packet_sink *mph_recording_sink(struct mph_recording *r,bool audio) {return &r->sinks[audio?1:0].sink;}
enum mph_recording_state mph_recording_state(struct mph_recording *r) {
    if(!r)return MPH_REC_IDLE;
    SDL_LockMutex(r->mutex);enum mph_recording_state s=r->state;SDL_UnlockMutex(r->mutex);return s;
}
bool mph_recording_start(struct mph_recording *r) {
    if(!r)return SDL_SetError("录制初始化失败。");
    SDL_LockMutex(r->mutex);
    if(r->state!=MPH_REC_IDLE) {SDL_UnlockMutex(r->mutex);return false;}
    for(int j=0;j<2;++j) {
        AVCodecParameters *p=r->params[j];
        if(!r->opened[j] || r->closed[j] || !p || (!p->extradata_size && p->codec_id!=AV_CODEC_ID_PCM_S16LE)) {
            SDL_UnlockMutex(r->mutex);return SDL_SetError("请等待手机画面和声音连接完成后再录制；手机音频不可用时无法开始录制。");
        }
        avcodec_parameters_free(&r->snapshot[j]);r->snapshot[j]=avcodec_parameters_alloc();
        if(!r->snapshot[j] || avcodec_parameters_copy(r->snapshot[j],p)<0) {SDL_UnlockMutex(r->mutex);return SDL_SetError("录制内存分配失败。");}
    }
    wchar_t temp[2048];char base[3072];SYSTEMTIME st;GetLocalTime(&st);
    DWORD n=GetTempPathW(2048,temp);
    if(!n || n>=2048 || !WideCharToMultiByte(CP_UTF8,0,temp,-1,base,sizeof(base),NULL,NULL)) {
        SDL_UnlockMutex(r->mutex);return SDL_SetError("无法访问当前用户的临时文件夹。");
    }
    snprintf(r->folder,sizeof(r->folder),"%sMobilePlayHelper/%04d%02d%02d-%02d%02d%02d-%lu-%llu",base,st.wYear,st.wMonth,st.wDay,st.wHour,st.wMinute,st.wSecond,(unsigned long)GetCurrentProcessId(),(unsigned long long)SDL_GetTicksNS());
    if(!SDL_CreateDirectory(r->folder)) {SDL_UnlockMutex(r->mutex);return false;}
    snprintf(r->video,sizeof(r->video),"%s/phone.mkv",r->folder);
    r->failure=NULL;r->start_ns=SDL_GetTicksNS();r->state=MPH_REC_STARTING;
    r->thread=SDL_CreateThread(worker,"mph-recording",r);
    if(!r->thread)r->state=MPH_REC_IDLE;
    SDL_UnlockMutex(r->mutex);return r->thread!=NULL;
}
void mph_recording_stop(struct mph_recording *r) {
    if(!r || !r->mutex)return;
    SDL_LockMutex(r->mutex);if(running(r))r->state=MPH_REC_STOPPING;SDL_SignalCondition(r->cond);SDL_UnlockMutex(r->mutex);
}
void mph_recording_event(struct mph_recording *r,int64_t pts,const char *payload) {
    if(!r)return;
    SDL_LockMutex(r->mutex);
    if(running(r)) {
        struct rec_item *i=calloc(1,sizeof(*i));
        if(i) {i->ns=SDL_GetTicksNS();i->video_pts=pts;snprintf(i->event,sizeof(i->event),"%s",payload);queue_locked(r,i);}
        else fail_locked(r,"诊断日志内存分配失败。");
    }
    SDL_UnlockMutex(r->mutex);
}
void mph_recording_poll(struct mph_recording *r,SDL_Window *owner) {
    if(!r || mph_recording_state(r)!=MPH_REC_DONE)return;
    if(r->thread)SDL_WaitThread(r->thread,NULL);
    r->thread=NULL;
    /* Use argv, never a shell command: paths may contain spaces/Unicode. */
    if(!SDL_getenv("MPH_RECORDING_TEST")) {
        char selected[4120];snprintf(selected,sizeof(selected),"/select,%s",r->video);
        for(char *p=selected;*p;++p) if(*p=='/')*p='\\';
        selected[0]='/';
        const char *args[]={"explorer.exe",selected,NULL};
        SDL_Process *p=SDL_CreateProcess(args,false);if(p)SDL_DestroyProcess(p);
        if(r->failure)SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR,"录制已停止",r->failure,owner);
    }
    SDL_LockMutex(r->mutex);r->state=MPH_REC_IDLE;SDL_UnlockMutex(r->mutex);
}
void mph_recording_destroy(struct mph_recording *r) {
    if(!r)return;
    mph_recording_stop(r);
    if(r->thread) {SDL_WaitThread(r->thread,NULL);r->thread=NULL;mph_recording_poll(r,NULL);}
    for(int j=0;j<2;++j) {avcodec_parameters_free(&r->params[j]);avcodec_parameters_free(&r->snapshot[j]);}
    SDL_DestroyCondition(r->cond);SDL_DestroyMutex(r->mutex);free(r);
}
