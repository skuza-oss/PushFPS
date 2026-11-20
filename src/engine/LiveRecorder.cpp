#include "veyra/engine/LiveRecorder.h"
#include "veyra/engine/Mp4Finalize.h"
#include "veyra/Brand.h"
#include "veyra/engine/MicrophoneCapture.h"
#include "veyra/engine/PushAudioMix.h"
#include "veyra/engine/PushAuxAudio.h"
#include "veyra/engine/PushStreamOverlay.h"
#include "veyra/engine/ReplayBuffer.h"
#include "veyra/pipeline/GpuPassUtils.h"
#include "veyra/sink/MfLiveEncoder.h"
#include "veyra/sink/NvencLiveEncoder.h"
#include "veyra/sink/RecordingAudioTap.h"
#include "veyra/Log.h"

#include <dxgi1_6.h>
#include <objbase.h>
#include <wrl/client.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstring>
#include <ctime>
#include <deque>
#include <filesystem>
#include <format>
#include <mutex>
#include <optional>
#include <thread>
#include <vector>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/audio_fifo.h>
#include <libavutil/channel_layout.h>
#include <libavutil/opt.h>
#include <libswresample/swresample.h>
}

namespace veyra::engine {
using Microsoft::WRL::ComPtr;
namespace {

constexpr unsigned kSlots=6;
constexpr uint32_t kCompareMaxHeight=1440;   // PushFPS comparison clips (split, side by side, reveal): recorded height cap unless a quality is chosen
constexpr size_t kBacklogSkipBytes=size_t(64)<<20;   // the disk is behind: skip frames
constexpr size_t kBacklogFailBytes=size_t(512)<<20;  // far behind: stop with an error
constexpr int kAudioRate=48000;
constexpr int64_t kAudioHardGap=kAudioRate*60/1000;  // beyond 60 ms: insert silence or drop
constexpr double kAudioSoftGap=kAudioRate*5/1000.0;  // beyond 5 ms: resample it away
constexpr int kCompensationWindow=kAudioRate*4;      // one soft correction per 4 s
constexpr int kCompensationMax=kCompensationWindow/1000; // at most 0.1 % speed
constexpr size_t kAudioQueueFrames=size_t(kAudioRate)*4;
constexpr uint32_t kGpuWaitMs=3000;
constexpr int64_t kReplayRetryUs=5000000;            // a failed replay encoder is retried after 5 s
constexpr int64_t kReplaySaveWaitMs=600;             // a save waits this long for the tracks to reach the request

std::string utf8(const std::wstring& text){
    if(text.empty())return {};
    const int size=WideCharToMultiByte(CP_UTF8,0,text.data(),int(text.size()),nullptr,0,nullptr,nullptr);
    std::string out(size_t(size),'\0');
    WideCharToMultiByte(CP_UTF8,0,text.data(),int(text.size()),out.data(),size,nullptr,nullptr);
    return out;
}
std::wstring wide(const std::string& text){
    if(text.empty())return {};
    const int size=MultiByteToWideChar(CP_UTF8,0,text.data(),int(text.size()),nullptr,0);
    std::wstring out(size_t(size),L'\0');
    MultiByteToWideChar(CP_UTF8,0,text.data(),int(text.size()),out.data(),size);
    return out;
}
std::wstring avError(int code){char buffer[AV_ERROR_MAX_STRING_SIZE]{};av_strerror(code,buffer,sizeof(buffer));return wide(buffer);}
std::wstring fileStamp(){
    const std::time_t now=std::time(nullptr);std::tm local{};localtime_s(&local,&now);
    wchar_t buffer[32]{};
    swprintf_s(buffer,L"%04d-%02d-%02d-%02d%02d%02d",local.tm_year+1900,local.tm_mon+1,local.tm_mday,local.tm_hour,local.tm_min,local.tm_sec);
    return buffer;
}
// PushFPS: about 0.3 bit per pixel for H.264 (HEVC ~2/3 of it), so fast game motion
// stays sharp (0.15 was soft: field report 2026-11-16). Frames above 60 per second
// count half: generated frames sit between two real ones and cost the encoder less.
// 1080p60 ~37 Mbps, 1080p120 ~56, 1440p60 ~66, 4K60 ~149 (H.264).
uint32_t autoBitrateMbps(uint32_t width,uint32_t height,uint32_t fps,bool hevc){
    const double rate=std::min(fps,60u)+std::max(int(fps)-60,0)*0.5;
    const double mbps=double(width)*height*rate*0.3*(hevc?0.65:1.0)/1e6;
    return uint32_t(std::clamp<long>(std::lround(mbps),hevc?8:10,160));
}
uint32_t adapterVendor(ID3D12Device* device){
    ComPtr<IDXGIFactory4> factory;
    if(FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))))return 0;
    ComPtr<IDXGIAdapter1> adapter;
    if(FAILED(factory->EnumAdapterByLuid(device->GetAdapterLuid(),IID_PPV_ARGS(&adapter))))return 0;
    DXGI_ADAPTER_DESC1 desc{};
    return SUCCEEDED(adapter->GetDesc1(&desc))?desc.VendorId:0;
}
// The replay buffer's memory cap when the settings leave it open: an eighth of
// the installed memory, 512 MB to 4 GB.
uint64_t defaultReplayMemory(){
    MEMORYSTATUSEX memory{};memory.dwLength=sizeof(memory);
    const uint64_t total=GlobalMemoryStatusEx(&memory)?memory.ullTotalPhys:(uint64_t(8)<<30);
    return std::clamp<uint64_t>(total/8,uint64_t(512)<<20,uint64_t(4)<<30);
}
std::wstring uniqueFile(const std::wstring& directory,const std::wstring& base,const wchar_t* extension=L".mp4"){
    for(unsigned attempt=0;;++attempt){
        auto file=std::filesystem::path(directory)/(base+(attempt?std::format(L"-{}",attempt):L"")+extension);
        std::error_code code;
        if(!std::filesystem::exists(file,code))return file.wstring();
    }
}

// The picture of a recording and of a saved replay.
bool setVideoParameters(AVCodecParameters* par,uint32_t width,uint32_t height,uint32_t hdrMode,bool hevc,const std::vector<uint8_t>& extradata){
    const bool hdr=hdrMode!=0;
    par->codec_type=AVMEDIA_TYPE_VIDEO;par->codec_id=hevc?AV_CODEC_ID_HEVC:AV_CODEC_ID_H264;
    par->width=int(width);par->height=int(height);
    par->format=hdr?AV_PIX_FMT_YUV420P10LE:AV_PIX_FMT_YUV420P;
    par->color_range=AVCOL_RANGE_MPEG;
    par->color_space=hdr?AVCOL_SPC_BT2020_NCL:AVCOL_SPC_BT709;
    par->color_primaries=hdr?AVCOL_PRI_BT2020:AVCOL_PRI_BT709;
    par->color_trc=hdr?AVCOL_TRC_SMPTE2084:AVCOL_TRC_BT709;
    if(hdr)par->profile=AV_PROFILE_HEVC_MAIN_10;
    par->extradata=static_cast<uint8_t*>(av_mallocz(extradata.size()+AV_INPUT_BUFFER_PADDING_SIZE));
    if(!par->extradata)return false;
    std::memcpy(par->extradata,extradata.data(),extradata.size());par->extradata_size=int(extradata.size());
    return true;
}

struct AudioBlock {std::vector<float> pcm;size_t frames=0;sink::AudioFormat format;int64_t atUs=0;};

// One sound source on the recording timeline: blocks stamped with the
// recording clock in (the played sound at the moment it reaches the
// endpoint, the microphone at the moment it was captured), 48 kHz stereo
// planar samples out, each at its timeline position: the fifo's front is the
// sample its track encodes next. Gaps become silence and clock drift is
// absorbed by resampling, so the track stays continuous and in sync.
struct AudioLane {
    // Producer thread (audio owner or microphone).
    std::mutex mutex;
    std::deque<AudioBlock> blocks;
    size_t queued=0;
    const void* source=nullptr;
    int64_t sourceUs=0;
    std::atomic<int64_t> arrivalUs{0};
    std::atomic<uint64_t> gaps{0},overflow{0};
    // Writer thread.
    SwrContext* swr=nullptr;
    sink::AudioFormat swrFormat;
    AVAudioFifo* fifo=nullptr;
    std::vector<float> planes[2];
    std::vector<float> silence;
    int64_t produced=0,compensationEnd=0;
    double driftEma=0;
    bool aligned=false;

    AudioLane()=default;
    AudioLane(const AudioLane&)=delete;
    AudioLane& operator=(const AudioLane&)=delete;
    ~AudioLane(){release();}
    void release(){
        if(swr)swr_free(&swr);
        if(fifo){av_audio_fifo_free(fifo);fifo=nullptr;}
    }
    bool open(){if(!fifo)fifo=av_audio_fifo_alloc(AV_SAMPLE_FMT_FLTP,2,kAudioRate);return fifo!=nullptr;}

    // Producer thread: copy and return. One source at a time; another takes
    // over only after the current one has been quiet for half a second
    // (endpoint rebuilds).
    void push(const float* pcm,size_t count,const sink::AudioFormat& format,int64_t atUs,const void* from){
        const int64_t now=sink::recordingClockUs();
        std::lock_guard lock(mutex);
        if(source&&source!=from&&now-sourceUs<500000)return;
        source=from;sourceUs=now;arrivalUs=now;
        AudioBlock block;block.pcm.assign(pcm,pcm+count*format.channels);block.frames=count;block.format=format;block.atUs=atUs;
        queued+=count;blocks.push_back(std::move(block));
        while(queued>kAudioQueueFrames&&!blocks.empty()){queued-=blocks.front().frames;blocks.pop_front();++overflow;}
    }
    bool configureSwr(const sink::AudioFormat& format){
        if(swr&&format==swrFormat)return true;
        if(swr){
            // Rare: the source layout changed. Keep what the old one buffered.
            if(!convertPcm(nullptr,0))return false;
            swr_free(&swr);
        }
        AVChannelLayout in{};
        if(av_channel_layout_from_mask(&in,format.mask)<0||in.nb_channels!=int(format.channels)){
            av_channel_layout_uninit(&in);av_channel_layout_default(&in,int(format.channels));
        }
        const AVChannelLayout out=AV_CHANNEL_LAYOUT_STEREO;
        int rc=swr_alloc_set_opts2(&swr,&out,AV_SAMPLE_FMT_FLTP,kAudioRate,&in,AV_SAMPLE_FMT_FLT,kAudioRate,0,nullptr);
        av_channel_layout_uninit(&in);
        if(rc<0||!swr)return false;
        // The resampler at an equal rate is what lets swr_set_compensation
        // absorb the source clock's drift against the recording clock.
        av_opt_set_int(swr,"flags",SWR_FLAG_RESAMPLE,0);
        if(swr_init(swr)<0){swr_free(&swr);return false;}
        swrFormat=format;compensationEnd=produced;
        silence.assign(size_t(kAudioRate/10)*format.channels,0.0f);
        return true;
    }
    bool convertPcm(const float* pcm,int count){
        const int capacity=swr_get_out_samples(swr,count)+32;
        if(capacity<=0)return true;
        for(auto& plane:planes)if(plane.size()<size_t(capacity))plane.resize(size_t(capacity));
        uint8_t* out[2]={reinterpret_cast<uint8_t*>(planes[0].data()),reinterpret_cast<uint8_t*>(planes[1].data())};
        const uint8_t* in[1]={reinterpret_cast<const uint8_t*>(pcm)};
        const int got=swr_convert(swr,out,capacity,pcm?in:nullptr,pcm?count:0);
        if(got<0)return false;
        if(got>0){
            void* data[2]={planes[0].data(),planes[1].data()};
            if(av_audio_fifo_write(fifo,data,got)<got)return false;
            produced+=got;
        }
        return true;
    }
    bool feedSilence(int64_t count){
        const int64_t chunk=int64_t(silence.size()/swrFormat.channels);
        while(count>0){
            const int n=int(std::min(count,chunk));
            if(!convertPcm(silence.data(),n))return false;
            count-=n;
        }
        return true;
    }
    int64_t outputPosition() const {return produced+(swr?swr_get_delay(swr,kAudioRate):0);}
    // Writer thread: the queued blocks into the fifo. final: up to the
    // picture's end (endTimeUs) and the resampler flushed.
    bool process(bool final,int64_t endTimeUs,int64_t t0){
        std::deque<AudioBlock> taken;
        {std::lock_guard lock(mutex);taken.swap(blocks);queued=0;}
        if(!t0)return true; // nothing recorded yet: earlier sound is not part of the file
        if(!swr&&!configureSwr(taken.empty()?sink::AudioFormat{}:taken.front().format))return false;
        for(auto& block:taken){
            if(!configureSwr(block.format))return false;
            int64_t start=(block.atUs-t0)*kAudioRate/1000000;
            size_t offset=0;
            if(start<0){const auto trim=size_t(-start);if(trim>=block.frames)continue;offset=trim;start=0;}
            int64_t drift=start-outputPosition();
            // The first block is placed exactly; later ones only beyond the
            // hard limit (gaps, rebuffering) - smaller drift is resampled away.
            const bool align=!aligned||drift>kAudioHardGap||drift< -kAudioHardGap;
            if(align&&drift>0){if(!feedSilence(drift))return false;if(aligned)++gaps;drift=0;}
            else if(align&&drift<0){const auto trim=size_t(-drift);if(trim>=block.frames-offset)continue;offset+=trim;drift=0;}
            aligned=true;
            driftEma=driftEma*0.9+double(drift)*0.1;
            if(std::abs(driftEma)>kAudioSoftGap&&produced>=compensationEnd){
                const int delta=int(std::clamp(driftEma,-double(kCompensationMax),double(kCompensationMax)));
                if(delta&&swr_set_compensation(swr,delta,kCompensationWindow)>=0)compensationEnd=produced+kCompensationWindow;
            }
            if(!convertPcm(block.pcm.data()+offset*block.format.channels,int(block.frames-offset)))return false;
        }
        if(final){
            const int64_t target=endTimeUs*kAudioRate/1000000;
            if(target>outputPosition()&&!feedSilence(target-outputPosition()))return false;
            return convertPcm(nullptr,0);
        }
        if(taken.empty()&&sink::recordingClockUs()-std::max(arrivalUs.load(),t0)>300000){
            // No sound arrives (paused, muted source, no audio monitoring, a
            // silent or missing microphone): keep the track continuous,
            // 200 ms behind real time.
            const int64_t target=(sink::recordingClockUs()-t0-200000)*kAudioRate/1000000;
            if(target>outputPosition()&&!feedSilence(target-outputPosition()))return false;
        }
        return true;
    }
};

// One AAC track: one lane, or the played sound with the microphone mixed in.
struct AudioTrack {
    AudioLane* lane=nullptr;
    AudioLane* mix=nullptr;
    AudioLane* mix2=nullptr;          // PushFPS: desktop audio and the extra input (PushAuxAudio.h)
    std::string name;                 // MP4 handler name
    AVCodecContext* codec=nullptr;
    AVFrame* frame=nullptr;
    std::vector<float> extra[2];
    int64_t encodedSamples=0;
    AVStream* stream=nullptr;         // a recording's stream
    std::shared_ptr<const AVCodecParameters> parameters;

    AudioTrack()=default;
    AudioTrack(const AudioTrack&)=delete;
    AudioTrack& operator=(const AudioTrack&)=delete;
    ~AudioTrack(){release();}
    void release(){
        if(codec)avcodec_free_context(&codec);
        if(frame)av_frame_free(&frame);
    }
    int frameSize() const {return codec&&codec->frame_size>0?codec->frame_size:1024;}
    int64_t bitRate=192000;           // PushFPS advanced recording: AAC bitrate
    bool open(){
        const AVCodec* encoder=avcodec_find_encoder(AV_CODEC_ID_AAC);
        if(!encoder)return false;
        codec=avcodec_alloc_context3(encoder);
        if(!codec)return false;
        codec->sample_fmt=AV_SAMPLE_FMT_FLTP;codec->sample_rate=kAudioRate;
        const AVChannelLayout stereo=AV_CHANNEL_LAYOUT_STEREO;
        av_channel_layout_copy(&codec->ch_layout,&stereo);
        codec->bit_rate=bitRate;codec->time_base=AVRational{1,kAudioRate};
        // MP4 (recordings and saved replays) keeps the decoder configuration
        // in the sample description.
        codec->flags|=AV_CODEC_FLAG_GLOBAL_HEADER;
        if(avcodec_open2(codec,encoder,nullptr)<0){avcodec_free_context(&codec);return false;}
        frame=av_frame_alloc();
        if(!frame)return false;
        frame->nb_samples=frameSize();
        frame->format=AV_SAMPLE_FMT_FLTP;frame->sample_rate=kAudioRate;
        av_channel_layout_copy(&frame->ch_layout,&stereo);
        if(av_frame_get_buffer(frame,0)<0)return false;
        AVCodecParameters* par=avcodec_parameters_alloc();
        if(!par||avcodec_parameters_from_context(par,codec)<0){avcodec_parameters_free(&par);return false;}
        parameters=std::shared_ptr<const AVCodecParameters>(par,[](const AVCodecParameters* p){auto* q=const_cast<AVCodecParameters*>(p);avcodec_parameters_free(&q);});
        return true;
    }
    template<class Out> bool send(AVFrame* input,Out& out){
        if(avcodec_send_frame(codec,input)<0)return false;
        AVPacket* packet=av_packet_alloc();
        if(!packet)return false;
        int rc=0;
        while((rc=avcodec_receive_packet(codec,packet))>=0){
            const bool ok=out(packet);
            av_packet_unref(packet);
            if(!ok){av_packet_free(&packet);return false;}
        }
        av_packet_free(&packet);
        return rc==AVERROR(EAGAIN)||rc==AVERROR_EOF;
    }
    // What every lane of the track holds; the final pass encodes up to
    // `limit` samples (the picture's end), discards the rest and flushes.
    template<class Out> bool encode(bool final,int64_t limit,Out&& out){
        const int size=frameSize();
        for(;;){
            int64_t available=av_audio_fifo_size(lane->fifo);
            if(mix)available=std::min<int64_t>(available,av_audio_fifo_size(mix->fifo));
            if(mix2)available=std::min<int64_t>(available,av_audio_fifo_size(mix2->fifo));
            if(final)available=std::min(available,limit-encodedSamples);
            if(available<=0||(!final&&available<size))break;
            const int n=int(std::min<int64_t>(available,size));
            if(av_frame_make_writable(frame)<0)return false;
            frame->nb_samples=n;
            if(av_audio_fifo_read(lane->fifo,reinterpret_cast<void**>(frame->data),n)<n)return false;
            for(AudioLane* each:{mix,mix2}){
                if(!each)continue;
                for(auto& plane:extra)if(plane.size()<size_t(n))plane.resize(size_t(n));
                void* data[2]={extra[0].data(),extra[1].data()};
                if(av_audio_fifo_read(each->fifo,data,n)<n)return false;
                for(int c=0;c<2;++c){
                    auto* samples=reinterpret_cast<float*>(frame->data[c]);
                    for(int i=0;i<n;++i)samples[i]=std::clamp(samples[i]+extra[c][size_t(i)],-1.0f,1.0f);
                }
            }
            frame->pts=encodedSamples;encodedSamples+=n;
            if(!send(frame,out))return false;
            frame->nb_samples=size;
        }
        if(!final)return true;
        for(auto* each:{lane,mix,mix2})
            if(each&&av_audio_fifo_size(each->fifo)>0)av_audio_fifo_drain(each->fifo,av_audio_fifo_size(each->fifo));
        return send(nullptr,out);
    }
};

// A saved replay: the clip's packets in time order into a new MP4. The
// timeline starts at the clip's first keyframe; sound before it (decoder
// priming) gets negative times, which the MP4 edit list cuts.
bool writeReplayFile(const std::wstring& path,const ReplayBuffer::Clip& clip,std::wstring& error){
    AVFormatContext* mux=nullptr;
    struct Close {AVFormatContext*& mux;~Close(){if(mux){if(mux->pb)avio_closep(&mux->pb);avformat_free_context(mux);}}} close{mux};
    auto fail=[&](std::wstring reason){
        error=std::move(reason);
        if(mux&&mux->pb)avio_closep(&mux->pb);
        std::error_code code;std::filesystem::remove(path,code);
        return false;
    };
    if(avformat_alloc_output_context2(&mux,nullptr,"mp4",nullptr)<0||!mux)return fail(L"无法创建回放文件封装器");
    AVStream* video=avformat_new_stream(mux,nullptr);
    if(!video)return fail(L"无法创建回放视频流");
    video->time_base=AVRational{1,1000000};
    video->avg_frame_rate=AVRational{int(std::max(1u,clip.video.fps)),1};
    if(!setVideoParameters(video->codecpar,clip.video.width,clip.video.height,clip.video.hdrMode,clip.video.hevc,clip.video.extradata))
        return fail(L"无法写入回放文件头");
    std::vector<AVStream*> audio;
    for(const auto& format:clip.audio){
        AVStream* stream=avformat_new_stream(mux,nullptr);
        if(!stream||!format.parameters||avcodec_parameters_copy(stream->codecpar,format.parameters.get())<0)return fail(L"无法创建回放音频流");
        stream->time_base=AVRational{1,kAudioRate};
        if(!format.name.empty())av_dict_set(&stream->metadata,"handler_name",format.name.c_str(),0);
        audio.push_back(stream);
    }
    int rc=avio_open(&mux->pb,utf8(path).c_str(),AVIO_FLAG_WRITE);
    if(rc<0)return fail(L"无法创建回放文件："+avError(rc));
    if((rc=avformat_write_header(mux,nullptr))<0)return fail(L"无法写入回放文件头："+avError(rc));
    const int64_t startSamples=av_rescale(clip.startUs,kAudioRate,1000000);
    auto write=[&](AVStream* stream,const ReplayBuffer::Packet& packet,int64_t pts,int64_t duration,AVRational base){
        AVPacket* out=av_packet_alloc();
        if(!out||av_new_packet(out,int(packet.data->size()))<0){av_packet_free(&out);return false;}
        std::memcpy(out->data,packet.data->data(),packet.data->size());
        out->stream_index=stream->index;out->pts=out->dts=pts;out->duration=std::max<int64_t>(1,duration);
        if(packet.key)out->flags|=AV_PKT_FLAG_KEY;
        av_packet_rescale_ts(out,base,stream->time_base);
        const int written=av_interleaved_write_frame(mux,out);
        av_packet_free(&out);
        return written>=0;
    };
    const auto& pictures=clip.videoPackets;
    size_t v=0;std::vector<size_t> a(audio.size(),0);
    for(;;){
        // The earliest next packet, in µs from the clip start.
        int64_t best=INT64_MAX;int which=-1;
        if(v<pictures.size()){best=pictures[v].pts-clip.startUs;which=0;}
        for(size_t t=0;t<audio.size();++t){
            if(a[t]>=clip.audioPackets[t].size())continue;
            const int64_t at=av_rescale(clip.audioPackets[t][a[t]].pts-startSamples,1000000,kAudioRate);
            if(at<best){best=at;which=int(t)+1;}
        }
        if(which<0)break;
        bool ok=true;
        if(which==0){
            const int64_t next=v+1<pictures.size()?pictures[v+1].pts:clip.endUs;
            ok=write(video,pictures[v],pictures[v].pts-clip.startUs,next-pictures[v].pts,AVRational{1,1000000});++v;
        }else{
            const size_t t=size_t(which-1);const auto& packet=clip.audioPackets[t][a[t]++];
            ok=write(audio[t],packet,packet.pts-startSamples,packet.duration,AVRational{1,kAudioRate});
        }
        if(!ok)return fail(L"写入回放文件失败（磁盘已满或没有写入权限？）");
    }
    if((rc=av_write_trailer(mux))<0)return fail(L"回放文件收尾失败："+avError(rc));
    avio_closep(&mux->pb);
    return true;
}

} // namespace

// One output file, or one stretch of the instant replay buffer: the capture
// slots and conversion (engine thread), the encoder thread and the writer
// thread. Created by record() on the engine thread; LiveRecorder keeps it
// until both threads are done.
struct LiveRecorder::Segment final : sink::RecordingAudioSink, MicrophoneSink, PushAuxSink {
    // Fixed at creation.
    ComPtr<ID3D12Device> device;
    uint32_t width=0,height=0,hdrMode=0,fps=60,bitrateMbps=20,vendor=0;   // width x height: what is encoded
    uint32_t bitrateKbps=0;                      // PushFPS presets: kbps precision when set
    uint32_t frameWidth=0,frameHeight=0;        // the presented frame (= width x height except vertical comparison clips)
    // PushFPS outputs and canvas: the recorder's planes are planeWidth x planeHeight (the frame for
    // comparison clips and full-frame outputs, else the output size), filled by PushCanvasConvert.
    uint32_t planeWidth=0,planeHeight=0;
    bool canvasPass=false;                      // planes differ from the frame: every frame goes through PushCanvasConvert
    PushCanvas canvas;
    // Live layout (setPushLiveCanvas): a full-frame output can still get a box, crop or background
    // while it runs, through layoutPass (PushCanvasConvert on its own descriptor heap).
    bool liveLayout=false;
    bool layoutReady=false;
    bool hevc=false,nvenc=false;
    // PushFPS advanced recording (RecordingSettings).
    uint32_t rateControl=1,quality=22,preset=4,keyframeSeconds=2,audioKbps=192;
    uint32_t multipass=0;bool psychoAq=false;   // PushFPS encoder tuning (NVENC)
    bool matroska=false;
    bool compareClip=false;                     // RecordingCompare: split comparison picture
    RecordingLabelRenderer compareLabels;
    CompareLayout compareLayout=CompareLayout::Split;
    double compareRevealSeconds=3.0;            // Reveal: a sweep starts every so many seconds
    uint32_t compareQuality=0;                  // the height the user chose for the clip (0: automatic)
    std::unique_ptr<RecordingCompare> compare;
    // PushFPS streaming mode scenes (PushStreamOverlay.h): the scene picture blended onto every frame
    // (not on comparison clips). A replaced one stays alive until the frames that used it are done.
    std::unique_ptr<FrameOverlay> overlay;
    uint64_t overlayVersion=0,overlayFrames=0;
    std::vector<std::pair<std::unique_ptr<FrameOverlay>,uint64_t>> retiredOverlays;
    // Scene transitions: the previous scene picture fading out under the new one fading in.
    std::unique_ptr<FrameOverlay> fadingOverlay;
    std::chrono::steady_clock::time_point fadeStart{};
    uint32_t fadeMs=0;
    // Live layers (webcam, window, video file) over the scene picture.
    std::unique_ptr<LiveLayerBlend> layers;
    bool layersFailed=false;
    std::atomic<uint64_t> generated{0};         // comparison: generated frames recorded
    bool sound=false,microphone=false,microphoneTrack=false; // what the sound tracks take
    std::atomic<bool> audio{false};             // the file (or buffer) has sound tracks
    std::wstring path;                          // a recording's file
    std::shared_ptr<ReplayBuffer> replay;       // the instant replay buffer instead of a file
    std::vector<stream::Target> streamTargets;  // going live instead of a file
    std::shared_ptr<stream::Outputs> outputs;   // writer thread creates it; status() reads it

    // Engine thread: conversion into the slots.
    pipeline::ComputePass convert;
    pipeline::ComputePass layoutPass;
    pipeline::DescriptorStager stager;
    pipeline::StateTracker states;
    ComPtr<ID3D12Resource> y,uv;
    struct Slot {
        ComPtr<ID3D12Resource> nv12;            // NVENC input
        ComPtr<ID3D12Resource> yRead,uvRead;    // Media Foundation readback
        UINT yPitch=0,uvPitch=0;
        std::atomic<int> state{0};              // 0 free, 1 recorded, 2 queued for the encoder
        ComPtr<ID3D12Fence> fence;              // the presenter's fence; kept alive past the session
        uint64_t value=0;
        int64_t ptsUs=0;
    };
    std::array<Slot,kSlots> slots;
    int pending=-1;
    std::atomic<int64_t> t0Us{0};               // recording clock of the first frame
    int64_t lastPtsUs=-1;
    std::atomic<uint64_t> frames{0},dropped{0};
    std::atomic<uint64_t> droppedReal{0}; // comparison clips: real frames that could not be recorded
    // PushFPS: what the game's sound track was given, for the file-closed log line. A silent clip is
    // then told apart at once: no sound arrived (soundBlocks 0) or the mixer silenced it (mixerSilenced).
    std::atomic<uint64_t> soundBlocks{0},mixerSilenced{0};

    // Encoder thread.
    std::thread encoderThread;
    std::mutex jobMutex;
    std::condition_variable jobCv;
    std::deque<unsigned> jobs;
    bool jobsClosed=false;
    std::atomic<bool> encoderReady{false};

    // Writer thread.
    std::thread writerThread;
    struct VideoPacket {std::vector<uint8_t> data;int64_t ptsUs=0;bool key=false;};
    std::mutex outMutex;
    std::condition_variable outCv;
    std::deque<VideoPacket> videoPackets;
    std::vector<uint8_t> headers;
    bool headersReady=false,encoderDone=false;
    std::atomic<size_t> backlogBytes{0};
    std::atomic<int64_t> endUs{-1};
    std::atomic<uint64_t> videoWritten{0};
    std::atomic<int64_t> writtenUs{0};

    // Sound: the played sound (tap) and the microphone, each on its own lane;
    // up to two AAC tracks take them (sound / sound + microphone / microphone).
    AudioLane soundLane,micLane;
    AudioLane auxLane;                        // PushFPS desktop audio + extra input
    bool aux=false;
    std::array<AudioTrack,2> tracks;
    unsigned trackCount=0;

    // Writer-owned FFmpeg state of a recording.
    AVFormatContext* mux=nullptr;
    AVStream* videoStream=nullptr;
    int64_t lastVideoDts=INT64_MIN;
    bool headerWritten=false,originSet=false;

    // Shared state.
    std::atomic<bool> closing{false},failed{false},encoderFinished{false},writerFinished{false};
    mutable std::mutex textMutex;
    std::wstring error,encoderName,note;

    ~Segment() override {
        if(encoderThread.joinable())encoderThread.join();
        if(writerThread.joinable())writerThread.join();
        releaseMux();
    }

    bool done() const {
        if(!encoderFinished.load()||!writerFinished.load()||pending>=0)return false;
        // An encoder failure may leave submitted capture commands in flight.
        // Keep their textures alive without making the playback thread wait.
        for(const auto& slot:slots)
            if(slot.fence&&slot.fence->GetCompletedValue()<slot.value)return false;
        return true;
    }
    void join(){if(encoderThread.joinable())encoderThread.join();if(writerThread.joinable())writerThread.join();}
    std::wstring errorText() const {std::lock_guard lock(textMutex);return error;}
    std::wstring encoderText() const {std::lock_guard lock(textMutex);return encoderName;}
    std::wstring noteText() const {std::lock_guard lock(textMutex);return note;}
    void setNote(std::wstring text){std::lock_guard lock(textMutex);note=std::move(text);}
    std::string name() const {return replay?std::string("replay buffer"):!streamTargets.empty()?std::string("live stream"):utf8(path);}

    void fail(const std::wstring& reason){
        {std::lock_guard lock(textMutex);if(error.empty())error=reason;}
        if(!failed.exchange(true))veyra::log::error("live-recorder",std::format("segment failed path={} reason={}",name(),utf8(reason)));
        closeJobs();
    }
    void closeJobs(){
        closing=true;
        {std::lock_guard lock(jobMutex);jobsClosed=true;}
        jobCv.notify_all();
    }
    // Any thread: no more frames; the threads finish the file.
    void finish(int64_t endClockUs){
        const int64_t t0=t0Us.load();
        if(t0&&endUs.load()<0)endUs=std::max<int64_t>(0,endClockUs-t0);
        // UI stop can arrive between record() and submitted(). Keep the
        // encoder alive until that command list is submitted or discarded.
        closing=true;
        if(pending<0)closeJobs();
    }
    bool matches(const PresentCapture::Frame& frame) const {
        return frame.width==frameWidth&&frame.height==frameHeight&&frame.hdrMode==hdrMode;
    }

    // ---- creation (engine thread) -------------------------------------------
    bool create(std::wstring& reason){
        std::vector<uint8_t> shader;
        if(!planeWidth||!planeHeight){planeWidth=frameWidth;planeHeight=frameHeight;}
        if(!convert.loadShader(canvasPass?"PushCanvasConvert.dxil":"RgbToNv12.dxil",shader)||
           !convert.create(device.Get(),shader,kSlots+2,1,2,canvasPass?16u:8u)){reason=L"录制转换着色器加载失败";return false;}
        if(!stager.initialize(device.Get(),kSlots)){reason=L"录制描述符分配失败";return false;}
        // A full-frame output keeps RgbToNv12 and loads the canvas pass beside it for live layout
        // changes; without it the layout applies from the next recording.
        if(liveLayout&&!canvasPass){
            std::vector<uint8_t> canvasShader;
            layoutReady=layoutPass.loadShader("PushCanvasConvert.dxil",canvasShader)&&layoutPass.create(device.Get(),canvasShader,kSlots+2,1,2,16u);
            if(!layoutReady)veyra::log::warn("live-recorder","live layout unavailable: PushCanvasConvert did not load");
        }
        const bool hdr=hdrMode!=0;
        y=pipeline::makeTexture(device.Get(),planeWidth,planeHeight,hdr?DXGI_FORMAT_R16_UNORM:DXGI_FORMAT_R8_UNORM,true);
        uv=pipeline::makeTexture(device.Get(),planeWidth/2,planeHeight/2,hdr?DXGI_FORMAT_R16G16_UNORM:DXGI_FORMAT_R8G8_UNORM,true);
        if(!y||!uv){reason=L"录制显存分配失败";return false;}
        pipeline::makeUav(device.Get(),y.Get(),hdr?DXGI_FORMAT_R16_UNORM:DXGI_FORMAT_R8_UNORM,pipeline::cpuHandleOf(convert,kSlots));
        pipeline::makeUav(device.Get(),uv.Get(),hdr?DXGI_FORMAT_R16G16_UNORM:DXGI_FORMAT_R8G8_UNORM,pipeline::cpuHandleOf(convert,kSlots+1));
        if(layoutReady){
            pipeline::makeUav(device.Get(),y.Get(),hdr?DXGI_FORMAT_R16_UNORM:DXGI_FORMAT_R8_UNORM,pipeline::cpuHandleOf(layoutPass,kSlots));
            pipeline::makeUav(device.Get(),uv.Get(),hdr?DXGI_FORMAT_R16G16_UNORM:DXGI_FORMAT_R8G8_UNORM,pipeline::cpuHandleOf(layoutPass,kSlots+1));
        }
        for(auto& slot:slots){
            if(nvenc){
                slot.nv12=pipeline::makeTexture(device.Get(),width,height,hdr?DXGI_FORMAT_P010:DXGI_FORMAT_NV12,false);
                if(!slot.nv12){reason=L"录制显存分配失败";return false;}
                continue;
            }
            auto readback=[&](UINT rowBytes,UINT rows,ComPtr<ID3D12Resource>& out,UINT& pitch){
                pitch=(rowBytes+255)/256*256;
                D3D12_HEAP_PROPERTIES hp{};hp.Type=D3D12_HEAP_TYPE_READBACK;
                D3D12_RESOURCE_DESC bd{};bd.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER;bd.Width=uint64_t(pitch)*rows;bd.Height=1;
                bd.DepthOrArraySize=1;bd.MipLevels=1;bd.SampleDesc.Count=1;bd.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
                return SUCCEEDED(device->CreateCommittedResource(&hp,D3D12_HEAP_FLAG_NONE,&bd,D3D12_RESOURCE_STATE_COPY_DEST,nullptr,IID_PPV_ARGS(&out)));
            };
            if(!readback(width,height,slot.yRead,slot.yPitch)||!readback(width,height/2,slot.uvRead,slot.uvPitch)){reason=L"录制回读缓冲分配失败";return false;}
        }
        if(compareClip){
            RecordingLabel left,right;
            if(compareLabels){
                try{compareLabels(width,height,left,right);}
                catch(...){left={};right={};veyra::log::warn("live-recorder","comparison labels could not be drawn; recording without them");}
            }
            compare=std::make_unique<RecordingCompare>();
            if(!compare->create(device.Get(),planeWidth,planeHeight,hdr,left,right,reason,compareLayout))return false;
            compare->setRevealInterval(compareRevealSeconds);
            if(compare->outputY()&&(compare->outputWidth()!=width||compare->outputHeight()!=height)){reason=L"对比片段尺寸不一致";return false;}
            const auto crop=compare->crop();
            veyra::log::info("live-recorder",std::format("comparison clip layout={} extent={}x{} frame={}x{} crop={}x{}+{}+{} hdr={} copied={} blended={} rawConsole={} labels={}x{} / {}x{} quality={} revealEvery={:.1f}s line={}",
                compareLayout==CompareLayout::SideBySide?"side-by-side":compareLayout==CompareLayout::Vertical?"vertical":compareLayout==CompareLayout::Reveal?"reveal":"split",width,height,frameWidth,frameHeight,
                crop.width,crop.height,crop.x,crop.y,hdr,compare->patches().size(),compare->overlays().size(),
                compare->rawAvailable(),left.width,left.height,right.width,right.height,compareQuality,compareRevealSeconds,compare->revealLineWidth()));
        }
        encoderThread=std::thread([this]{encoderMain();});
        writerThread=std::thread([this]{writerMain();});
        return true;
    }

    // ---- capture (engine thread) ---------------------------------------------
    bool capture(ID3D12GraphicsCommandList* list,const PresentCapture::Frame& frame,int64_t nowUs){
        if(closing.load()||!encoderReady.load())return false;
        if(backlogBytes.load()>=kBacklogFailBytes){fail(L"磁盘写入跟不上录制，已停止录制（请换更快的磁盘或降低码率）");return false;}
        // PushFPS comparison clips: the console side moves only on real frames, so when the
        // encoder or the disk falls behind, generated frames are skipped first and one slot stays
        // free for the next real frame. Skipping real frames instead could line up with their
        // steady rhythm and freeze the console side for seconds (field report 2026-11-16).
        const bool keepReal=compare&&!frame.generated;
        const bool diskBehind=backlogBytes.load()>=kBacklogSkipBytes;
        if(compare){
            unsigned freeSlots=0;
            for(unsigned i=0;i<kSlots;++i)if(slots[i].state.load()==0)++freeSlots;
            if(compareSkipsFrame(frame.generated,freeSlots,diskBehind)){++dropped;return false;}
        }else if(diskBehind){++dropped;return false;}
        int index=-1;
        for(unsigned i=0;i<kSlots;++i){int expected=0;if(slots[i].state.compare_exchange_strong(expected,1)){index=int(i);break;}}
        if(index<0){++dropped;if(keepReal)++droppedReal;return false;}
        auto& slot=slots[size_t(index)];
        if(!t0Us.load())t0Us=nowUs;
        int64_t pts=nowUs-t0Us.load();
        if(pts<=lastPtsUs)pts=lastPtsUs+1;
        lastPtsUs=pts;slot.ptsUs=pts;
        // Per-frame SRV through the staging heap (GpuPassUtils.h driver note:
        // a direct SRV write into a shader-visible heap can corrupt Present).
        D3D12_SHADER_RESOURCE_VIEW_DESC view{};view.Format=frame.format;view.ViewDimension=D3D12_SRV_DIMENSION_TEXTURE2D;
        view.Shader4ComponentMapping=D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;view.Texture2D.MipLevels=1;
        // The live layout (box, crop, background) with this output's own fit.
        PushCanvas layout=canvas;
        if(liveLayout){
            PushCanvas live;
            // A stretched game picture (layout editor, Ctrl resize) stretches in every output.
            if(pushLiveCanvas(live)){const auto fit=layout.fit;const bool stretched=live.fit==PushCanvasFit::Stretch;layout=live;layout.fit=stretched?PushCanvasFit::Stretch:fit;}
        }
        const bool viaLayout=!canvasPass&&layoutReady&&!layout.fullFrame();
        pipeline::ComputePass& pass=viaLayout?layoutPass:convert;
        stager.stageSrv(frame.texture,&view,pass.heap.Get(),UINT(index));
        D3D12_RESOURCE_BARRIER source{};source.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;source.Transition.pResource=frame.texture;
        source.Transition.Subresource=D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        source.Transition.StateBefore=D3D12_RESOURCE_STATE_COMMON;source.Transition.StateAfter=D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
        list->ResourceBarrier(1,&source);
        states.transition(list,y.Get(),D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        states.transition(list,uv.Get(),D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        if(canvasPass||viaLayout){
            // PushFPS outputs and canvas (PushCanvasConvert.hlsl): the picture sampled into this output.
            const auto map=mapPushCanvas(layout,frameWidth,frameHeight,planeWidth,planeHeight);
            const uint32_t taps=pushCanvasTaps(map.srcW,map.dstW)|(pushCanvasTaps(map.srcH,map.dstH)<<8);
            const float constants[16]={std::bit_cast<float>(planeWidth),std::bit_cast<float>(planeHeight),std::bit_cast<float>(hdrMode),frame.ditherStep,
                float(map.dstX),float(map.dstY),float(map.dstW),float(map.dstH),float(map.srcX),float(map.srcY),float(map.srcW),float(map.srcH),
                std::bit_cast<float>(frameWidth),std::bit_cast<float>(frameHeight),std::bit_cast<float>(taps),std::bit_cast<float>(layout.background&0xFFFFFFu)};
            pass.bind(list,constants,pipeline::gpuHandleOf(pass,UINT(index)).ptr,pipeline::gpuHandleOf(pass,kSlots).ptr);
            list->Dispatch((planeWidth+15)/16,(planeHeight+15)/16,1);
        }else{
            // reserved.x = HDR mode, reserved.y = output dither step (RgbToNv12.hlsl).
            const float constants[8]={std::bit_cast<float>(frameWidth),std::bit_cast<float>(frameHeight),std::bit_cast<float>(hdrMode),frame.ditherStep,0,0,0,0};
            convert.bind(list,constants,pipeline::gpuHandleOf(convert,UINT(index)).ptr,pipeline::gpuHandleOf(convert,kSlots).ptr);
            list->Dispatch((frameWidth+15)/16,(frameHeight+15)/16,1);
        }
        states.uavBarrier(list,y.Get());states.uavBarrier(list,uv.Get());
        if(!compare){
            ++overlayFrames;
            const uint64_t version=pushStreamOverlayVersion();
            if(version!=overlayVersion){
                overlayVersion=version;
                uint64_t got=0;
                const auto image=pushStreamOverlay(got);
                // The old picture fades out while the new one fades in (scene transitions).
                if(fadingOverlay)retiredOverlays.emplace_back(std::move(fadingOverlay),overlayFrames);
                fadeMs=overlayFrames>1?pushStreamTransitionMs():0;
                fadeStart=std::chrono::steady_clock::now();
                if(overlay){
                    if(fadeMs>0)fadingOverlay=std::move(overlay);
                    else retiredOverlays.emplace_back(std::move(overlay),overlayFrames);
                }
                if(image){
                    auto next=std::make_unique<FrameOverlay>();
                    if(next->create(device.Get(),planeWidth,planeHeight,hdrMode!=0,*image))overlay=std::move(next);
                    else veyra::log::warn("live-recorder","scene overlay could not be prepared; recording without it");
                }
                veyra::log::info("live-recorder",std::format("scene overlay {} {}x{}",overlay?"on":"off",image?image->width:0,image?image->height:0));
            }
            // Every slot has been reused at least twice since: the frames that used them are finished.
            std::erase_if(retiredOverlays,[&](const auto& r){return overlayFrames>r.second+2*kSlots;});
            const float shown=pushTransitionProgress(std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-fadeStart).count(),fadeMs);
            if(fadingOverlay){
                if(shown>=1.0f)retiredOverlays.emplace_back(std::move(fadingOverlay),overlayFrames);
                else fadingOverlay->apply(list,states,y.Get(),uv.Get(),1.0f-shown);
            }
            if(overlay)overlay->apply(list,states,y.Get(),uv.Get(),shown);
            if(!layersFailed&&(layers||pushLiveLayersActive())){
                if(!layers){
                    layers=std::make_unique<LiveLayerBlend>();
                    if(!layers->create(device.Get(),planeWidth,planeHeight,hdrMode!=0,kSlots)){
                        layers.reset();layersFailed=true;
                        veyra::log::warn("live-recorder","live layers could not be prepared; recording without camera, window and video sources");
                    }else veyra::log::info("live-recorder","live layers on");
                }
                if(layers)layers->apply(list,states,y.Get(),uv.Get());
            }
        }
        if(compare){
            const RecordingRawFrame raw{frame.raw,frame.rawSdrInHdr};
            // The frame's own time in the clip places the Reveal layout's line.
            compare->compose(list,states,y.Get(),uv.Get(),frame.generated,frame.raw?&raw:nullptr,unsigned(index),double(pts)/1e6);
            if(frame.generated)++generated;
        }
        // What is encoded: the recorder's planes, or a vertical comparison clip's own picture.
        ID3D12Resource* const encY=compare&&compare->outputY()?compare->outputY():y.Get();
        ID3D12Resource* const encUv=compare&&compare->outputUv()?compare->outputUv():uv.Get();
        states.transition(list,encY,D3D12_RESOURCE_STATE_COPY_SOURCE);
        states.transition(list,encUv,D3D12_RESOURCE_STATE_COPY_SOURCE);
        std::swap(source.Transition.StateBefore,source.Transition.StateAfter);
        list->ResourceBarrier(1,&source);
        if(nvenc){
            D3D12_RESOURCE_BARRIER target{};target.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;target.Transition.pResource=slot.nv12.Get();
            target.Transition.Subresource=D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            target.Transition.StateBefore=D3D12_RESOURCE_STATE_COMMON;target.Transition.StateAfter=D3D12_RESOURCE_STATE_COPY_DEST;
            list->ResourceBarrier(1,&target);
            for(UINT plane=0;plane<2;++plane){
                D3D12_TEXTURE_COPY_LOCATION from{},to{};
                from.pResource=plane?encUv:encY;from.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;from.SubresourceIndex=0;
                to.pResource=slot.nv12.Get();to.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;to.SubresourceIndex=plane;
                list->CopyTextureRegion(&to,0,0,0,&from,nullptr);
            }
            std::swap(target.Transition.StateBefore,target.Transition.StateAfter);
            list->ResourceBarrier(1,&target);
        }else{
            auto copyPlane=[&](ID3D12Resource* from,ID3D12Resource* to,DXGI_FORMAT format,UINT w,UINT h,UINT pitch){
                D3D12_TEXTURE_COPY_LOCATION a{},b{};
                a.pResource=from;a.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;a.SubresourceIndex=0;
                b.pResource=to;b.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
                b.PlacedFootprint.Footprint.Format=format;b.PlacedFootprint.Footprint.Width=w;b.PlacedFootprint.Footprint.Height=h;
                b.PlacedFootprint.Footprint.Depth=1;b.PlacedFootprint.Footprint.RowPitch=pitch;
                list->CopyTextureRegion(&b,0,0,0,&a,nullptr);
            };
            copyPlane(encY,slot.yRead.Get(),DXGI_FORMAT_R8_UNORM,width,height,slot.yPitch);
            copyPlane(encUv,slot.uvRead.Get(),DXGI_FORMAT_R8G8_UNORM,width/2,height/2,slot.uvPitch);
        }
        pending=index;
        return true;
    }
    void submittedPending(ID3D12Fence* fence,uint64_t value){
        if(pending<0)return;
        auto& slot=slots[size_t(pending)];pending=-1;
        slot.fence=fence;slot.value=value;slot.state=2;
        {std::lock_guard lock(jobMutex);if(!encoderFinished.load())jobs.push_back(unsigned(&slot-slots.data()));}
        jobCv.notify_one();
        ++frames;
        if(closing.load())closeJobs();
    }
    void discardPending(){
        if(pending<0)return;
        slots[size_t(pending)].state=0;pending=-1;
        if(closing.load())closeJobs();
    }
    void releaseSlot(unsigned index){slots[index].fence.Reset();slots[index].state=0;}

    // ---- encoder thread -------------------------------------------------------
    void publishHeaders(std::vector<uint8_t> bytes,std::wstring name){
        {std::lock_guard lock(textMutex);encoderName=std::move(name);}
        {std::lock_guard lock(outMutex);headers=std::move(bytes);headersReady=true;}
        outCv.notify_all();
        encoderReady=true;
    }
    void pushVideo(std::vector<uint8_t> data,int64_t ptsUs,bool key){
        const size_t size=data.size();
        // Publish the size before the writer can take the packet. Otherwise
        // it can subtract first, underflow size_t and stop a healthy recording.
        {std::lock_guard lock(outMutex);videoPackets.push_back({std::move(data),ptsUs,key});backlogBytes+=size;}
        outCv.notify_one();
    }
    void encoderMain(){
        if(nvenc)encodeNvenc();else encodeMediaFoundation();
        encoderReady=false;closing=true;
        // Failed inputs may still be used by the presentation queue. Leave
        // their fence references for done() to check before releasing memory.
        {std::lock_guard lock(outMutex);encoderDone=true;}
        outCv.notify_all();
        encoderFinished=true;
    }
    void encodeNvenc(){
        sink::NvencLiveEncoder encoder;
        sink::NvencLiveEncoder::Config config;
        config.width=width;config.height=height;config.hevc=hevc;config.hdr=hdrMode!=0;config.fps=fps;config.bitrateMbps=bitrateMbps;config.bitrateKbps=bitrateKbps;
        config.rateControl=rateControl;config.qp=quality;config.preset=preset;config.keyframeSeconds=keyframeSeconds;
        config.multipass=multipass;config.psychoAq=psychoAq;
        std::vector<ID3D12Resource*> inputs;for(auto& slot:slots)inputs.push_back(slot.nv12.Get());
        bool opened=encoder.open(device.Get(),config,inputs);
        if(!opened&&(config.multipass||config.psychoAq)){
            // The tuning first: a GPU that refuses two-pass or AQ keeps the other chosen settings.
            veyra::log::warn("live-recorder",std::format("encoder tuning refused ({}); encoding without two-pass and AQ",utf8(encoder.lastError())));
            encoder.close();
            config.multipass=0;config.psychoAq=false;
            opened=encoder.open(device.Get(),config,inputs);
            if(opened)setNote(L"显卡不支持二次编码或心理视觉调优，已按不带这两项的设置编码");
        }
        if(!opened){
            // PushFPS: advanced settings this GPU refuses fall back to the standard ones.
            // Going live stays on a constant bitrate: the platforms need it (this fallback used to
            // switch a refused stream encoder to VBR).
            const uint32_t standardRate=streamTargets.empty()?1u:0u;
            const bool standard=config.rateControl==standardRate&&config.preset==4&&config.keyframeSeconds==2;
            if(standard){fail(L"NVENC 无法开始录制："+encoder.lastError());return;}
            veyra::log::warn("live-recorder",std::format("advanced encoder settings refused ({}); using the standard ones",utf8(encoder.lastError())));
            encoder.close();
            config.rateControl=standardRate;config.preset=4;config.keyframeSeconds=2;
            if(!encoder.open(device.Get(),config,inputs)){fail(L"NVENC 无法开始录制："+encoder.lastError());return;}
            setNote(L"显卡不支持所选的高级编码设置，已改用标准设置录制");
        }
        publishHeaders(encoder.headers(),encoder.describe());
        veyra::log::info("live-recorder",std::format("encoder ready {} path={}",utf8(encoder.describe()),name()));
        for(;;){
            std::vector<unsigned> batch;bool closed=false;
            {
                std::unique_lock lock(jobMutex);
                if(jobs.empty()&&!jobsClosed&&encoder.inFlight()==0)jobCv.wait(lock,[&]{return !jobs.empty()||jobsClosed;});
                while(!jobs.empty()&&batch.size()+encoder.inFlight()<encoder.capacity()){batch.push_back(jobs.front());jobs.pop_front();}
                closed=jobsClosed&&jobs.empty();
            }
            for(const auto index:batch){
                auto& slot=slots[index];
                if(!encoder.submit(index,slot.fence.Get(),slot.value,slot.ptsUs)){fail(L"NVENC 编码失败："+encoder.lastError());break;}
            }
            if(failed.load())break;
            // Hand finished frames to the writer; block on the oldest only
            // when nothing new can be submitted meanwhile.
            bool blocked=false;
            while(encoder.inFlight()){
                bool more=false;{std::lock_guard lock(jobMutex);more=!jobs.empty();}
                const bool wait=!(more&&encoder.inFlight()<encoder.capacity())&&!blocked;
                sink::NvencLiveEncoder::Packet packet;bool ready=false;
                if(!encoder.takeOldest(packet,wait,kGpuWaitMs,ready)){fail(L"NVENC 编码失败："+encoder.lastError());break;}
                if(!ready)break;
                blocked|=wait;
                releaseSlot(packet.input);
                pushVideo(std::move(packet.data),packet.ptsUs,packet.key);
            }
            if(failed.load())break;
            if(closed&&!encoder.inFlight()){std::lock_guard lock(jobMutex);if(jobs.empty())break;}
        }
        encoder.close();
    }
    void encodeMediaFoundation(){
        const bool com=SUCCEEDED(CoInitializeEx(nullptr,COINIT_MULTITHREADED));
        {
            auto encoder=sink::createMediaFoundationLiveEncoder();
            sink::EncoderConfig config;config.hevc=hevc;config.fpsNum=fps;config.fpsDen=1;config.bitrateMbps=bitrateMbps;config.bitrateKbps=bitrateKbps;
            config.rateControl=sink::ExportRateControl(std::min(rateControl,2u));config.adapterVendorId=vendor;config.drainTimeoutMs=kGpuWaitMs;
            // PushFPS advanced recording: encoder preset 1..7 onto Media Foundation's
            // 0..100 speed/quality scale (4 is the standard 70); quality level 1..51
            // onto its 1..100 quality (22 is the standard 75).
            static constexpr uint32_t speed[7]={10,30,50,70,80,90,100};
            config.qualityVsSpeed=speed[std::clamp(preset,1u,7u)-1];
            config.constantQuality=uint32_t(std::clamp(119-int(quality)*2,1,100));
            config.keyframeSeconds=keyframeSeconds;
            auto writer=[this](const uint8_t* bytes,size_t size,int64_t ptsUs,bool key){pushVideo(std::vector<uint8_t>(bytes,bytes+size),ptsUs,key);return true;};
            if(!encoder->openLive(width,height,config,writer)){fail(L"系统硬件编码器无法开始录制："+encoder->liveError());}
            else{
                publishHeaders(encoder->liveHeaders(),encoder->liveDescription());
                veyra::log::info("live-recorder",std::format("encoder ready {} path={}",utf8(encoder->liveDescription()),name()));
                HANDLE event=CreateEventW(nullptr,FALSE,FALSE,nullptr);
                for(;;){
                    unsigned index=0;
                    {
                        std::unique_lock lock(jobMutex);
                        jobCv.wait(lock,[&]{return !jobs.empty()||jobsClosed;});
                        if(jobs.empty())break;
                        index=jobs.front();jobs.pop_front();
                    }
                    auto& slot=slots[index];
                    if(slot.fence->GetCompletedValue()<slot.value&&(!event||FAILED(slot.fence->SetEventOnCompletion(slot.value,event))||WaitForSingleObject(event,kGpuWaitMs)!=WAIT_OBJECT_0)){
                        fail(L"录制画面回读超时");releaseSlot(index);break;
                    }
                    void* yData=nullptr;void* uvData=nullptr;
                    const D3D12_RANGE yRange{0,size_t(slot.yPitch)*height},uvRange{0,size_t(slot.uvPitch)*(height/2)};
                    bool ok=SUCCEEDED(slot.yRead->Map(0,&yRange,&yData))&&SUCCEEDED(slot.uvRead->Map(0,&uvRange,&uvData));
                    if(ok)ok=encoder->encodeNv12(static_cast<const uint8_t*>(yData),slot.yPitch,static_cast<const uint8_t*>(uvData),slot.uvPitch,slot.ptsUs);
                    const D3D12_RANGE none{0,0};
                    if(yData)slot.yRead->Unmap(0,&none);
                    if(uvData)slot.uvRead->Unmap(0,&none);
                    releaseSlot(index);
                    if(!ok){fail(L"系统硬件编码失败："+encoder->liveError());break;}
                }
                if(event)CloseHandle(event);
                if(!failed.load()&&!encoder->finishLive())fail(L"系统硬件编码器收尾失败："+encoder->liveError());
            }
        }
        if(com)CoUninitialize();
    }

    // ---- sound (audio owner and microphone threads) ----------------------------
    void renderedPcm(const float* pcm,size_t count,const sink::AudioFormat& format,int64_t playoutUs,const void* renderer) override {
        if(!sound||!audio||closing.load()||!format.valid()||!count)return;
        // PushFPS streaming mode mixer: the game fader and mute (PushAudioMix.h). What the player
        // hears keeps its own volume; this changes only recordings, the replay and streams.
        // A card scene silences the game too, except in a comparison clip, which never shows the card.
        const float gain=pushRecordingGameGain(compareClip);
        ++soundBlocks;
        if(gain<=0.0f)++mixerSilenced;
        if(gain!=1.0f){
            thread_local std::vector<float> scaled;
            scaled.resize(count*format.channels);
            for(size_t i=0;i<scaled.size();++i)scaled[i]=pcm[i]*gain;
            soundLane.push(scaled.data(),count,format,playoutUs,renderer);
            return;
        }
        soundLane.push(pcm,count,format,playoutUs,renderer);
    }
    void microphonePcm(const float* stereo,size_t count,int64_t capturedUs) override {
        if(!microphone||!audio||closing.load()||!count)return;
        micLane.push(stereo,count,sink::AudioFormat{},capturedUs,this);
    }
    void auxPcm(const float* stereo,size_t count,int64_t capturedUs) override {
        if(!aux||!audio||closing.load()||!count)return;
        auxLane.push(stereo,count,sink::AudioFormat{},capturedUs,static_cast<PushAuxSink*>(this));
    }

    // ---- writer thread ---------------------------------------------------------
    void releaseMux(){
        if(mux){
            if(mux->pb)avio_closep(&mux->pb);
            avformat_free_context(mux);mux=nullptr;
        }
        for(auto& track:tracks)track.release();
        soundLane.release();micLane.release();auxLane.release();
    }
    // The tracks the settings ask for: the played sound, the microphone
    // mixed into it, or the microphone on a second track.
    bool openAudio(){
        trackCount=0;
        if((sound&&!soundLane.open())||(microphone&&!micLane.open())||(aux&&!auxLane.open()))return false;
        // Desktop audio and the extra input ride on the first track (the one streams send).
        AudioLane* auxMix=aux?&auxLane:nullptr;
        auto add=[&](AudioLane* lane,AudioLane* mix,const char* label){
            auto& track=tracks[trackCount];
            track.lane=lane;track.mix=mix;track.name=label;
            track.mix2=trackCount==0?auxMix:nullptr;
            track.bitRate=int64_t(std::clamp(audioKbps,64u,320u))*1000;
            if(!track.open())return false;
            ++trackCount;return true;
        };
        if(sound&&microphone&&!microphoneTrack)return add(&soundLane,&micLane,"Veyra sound + microphone");
        if(sound&&!add(&soundLane,nullptr,"Veyra sound"))return false;
        if(microphone&&!add(&micLane,nullptr,"Veyra microphone"))return false;
        if(aux&&trackCount==0){auxMix=nullptr;if(!add(&auxLane,nullptr,"PushFPS desktop audio"))return false;}
        return true;
    }
    void openSound(){
        if(!audio)return;
        if(!openAudio()){
            audio=false;trackCount=0;
            for(auto& track:tracks)track.release();
            setNote(L"没有可用的 AAC 编码器，本次录制不含声音");
            veyra::log::warn("live-recorder","AAC encoder unavailable; recording without sound");
        }
    }
    bool openFile(std::vector<uint8_t> extradata){
        if(avformat_alloc_output_context2(&mux,nullptr,matroska?"matroska":"mp4",nullptr)<0||!mux){fail(L"无法创建录制封装器");return false;}
        videoStream=avformat_new_stream(mux,nullptr);
        if(!videoStream){fail(L"无法创建录制视频流");return false;}
        videoStream->time_base=AVRational{1,1000000};
        videoStream->avg_frame_rate=AVRational{int(fps),1};
        if(!setVideoParameters(videoStream->codecpar,width,height,hdrMode,hevc,extradata)){fail(L"无法写入录制文件头");return false;}
        openSound();
        for(unsigned i=0;i<trackCount;++i){
            auto& track=tracks[i];
            track.stream=avformat_new_stream(mux,nullptr);
            if(!track.stream||avcodec_parameters_copy(track.stream->codecpar,track.parameters.get())<0){fail(L"无法创建录制音频流");return false;}
            track.stream->time_base=AVRational{1,kAudioRate};
            av_dict_set(&track.stream->metadata,"handler_name",track.name.c_str(),0);
        }
        int rc=avio_open(&mux->pb,utf8(path).c_str(),AVIO_FLAG_WRITE);
        if(rc<0){fail(L"无法创建录制文件："+avError(rc));return false;}
        // Fragmented MP4: every fragment is playable on its own, so an
        // interrupted recording keeps what was written.
        // MKV (PushFPS advanced recording) is readable up to the last cluster anyway.
        AVDictionary* options=nullptr;
        if(!matroska)av_dict_set(&options,"movflags","frag_keyframe+empty_moov+default_base_moof",0);
        rc=avformat_write_header(mux,&options);
        av_dict_free(&options);
        if(rc<0){fail(L"无法写入录制文件头："+avError(rc));return false;}
        headerWritten=true;
        veyra::log::info("live-recorder",std::format("file open path={} extent={}x{} codec={} hdr={} fps={} bitrateMbps={} audio={} sound={} microphone={} tracks={} rc={} quality={} preset={} keyframeS={} audioKbps={} container={}",
            utf8(path),width,height,hevc?"HEVC":"H264",hdrMode,fps,bitrateMbps,audio.load(),sound,microphone?(microphoneTrack?"track":"mixed"):"off",trackCount,
            rateControl,quality,preset,keyframeSeconds,audioKbps,matroska?"mkv":"mp4"));
        if(multipass||psychoAq)veyra::log::info("live-recorder",std::format("encoder tuning multipass={} psychoAq={} (NVENC only)",multipass,psychoAq));
        return true;
    }
    // The instant replay buffer takes what a file would: the encoder's
    // parameter sets and the sound tracks' decoder configuration.
    bool openReplay(std::vector<uint8_t> extradata){
        openSound();
        ReplayBuffer::VideoFormat format;
        format.width=width;format.height=height;format.hdrMode=hdrMode;format.fps=fps;format.hevc=hevc;format.extradata=std::move(extradata);
        std::vector<ReplayBuffer::AudioFormat> sounds;
        for(unsigned i=0;i<trackCount;++i)sounds.push_back({tracks[i].parameters,tracks[i].name});
        replay->setFormat(std::move(format),std::move(sounds));
        veyra::log::info("live-recorder",std::format("replay buffer open extent={}x{} codec={} hdr={} fps={} bitrateMbps={} keepSeconds={:.0f} sound={} microphone={} tracks={}",
            width,height,hevc?"HEVC":"H264",hdrMode,fps,bitrateMbps,double(replay->keepUs())/1e6,sound,microphone?(microphoneTrack?"track":"mixed"):"off",trackCount));
        return true;
    }
    // Going live: the sound tracks as for a file, then one connection per target. Only the first
    // sound track goes out (FLV carries one); stream mode mixes the microphone into it.
    bool openStream(std::vector<uint8_t> extradata){
        openSound();
        outputs=std::make_shared<stream::Outputs>([](const std::string& line){veyra::log::info("live-stream",line);});
        stream::VideoFormat format;
        format.width=int(width);format.height=int(height);format.fps=int(fps);format.hevc=hevc;
        format.extradata=std::move(extradata);format.bitRate=bitrateKbps?int64_t(bitrateKbps)*1000:int64_t(bitrateMbps)*1000000;
        std::string reason;
        if(!outputs->start(streamTargets,format,trackCount?tracks[0].parameters.get():nullptr,reason)){fail(L"无法开始直播："+wide(reason));return false;}
        std::string names;
        for(const auto& t:streamTargets)names+=(names.empty()?"":", ")+t.name+" "+stream::redact(t.url);
        veyra::log::info("live-recorder",std::format("live stream open extent={}x{} fps={} bitrateMbps={} audio={} targets=[{}]",width,height,fps,bitrateMbps,trackCount>0,names));
        return true;
    }
    bool writeVideo(const VideoPacket& packet,int64_t durationUs){
        AVPacket* out=av_packet_alloc();
        if(!out||av_new_packet(out,int(packet.data.size()))<0){av_packet_free(&out);return false;}
        std::memcpy(out->data,packet.data.data(),packet.data.size());
        out->stream_index=videoStream->index;out->pts=out->dts=packet.ptsUs;out->duration=std::max<int64_t>(1,durationUs);
        if(packet.key)out->flags|=AV_PKT_FLAG_KEY;
        av_packet_rescale_ts(out,AVRational{1,1000000},videoStream->time_base);
        if(out->dts<=lastVideoDts)out->pts=out->dts=lastVideoDts+1;
        lastVideoDts=out->dts;
        const int rc=av_interleaved_write_frame(mux,out);
        av_packet_free(&out);
        if(rc<0){veyra::log::error("live-recorder",std::format("video write failed rc={}",rc));return false;}
        ++videoWritten;writtenUs=packet.ptsUs;
        return true;
    }
    bool processAudio(bool final,int64_t endTimeUs){
        const int64_t t0=t0Us.load();
        if(sound&&!soundLane.process(final,endTimeUs,t0))return false;
        if(microphone&&!micLane.process(final,endTimeUs,t0))return false;
        if(aux&&!auxLane.process(final,endTimeUs,t0))return false;
        if(!t0)return true;
        const int64_t limit=final?std::max<int64_t>(endTimeUs*kAudioRate/1000000,0):INT64_MAX;
        for(unsigned i=0;i<trackCount;++i){
            auto& track=tracks[i];
            const bool ok=track.encode(final,limit,[&](AVPacket* packet){
                if(outputs){
                    if(i==0)outputs->pushAudio(std::make_shared<const std::vector<uint8_t>>(packet->data,packet->data+packet->size),
                                               packet->pts*1000000/kAudioRate,std::max<int64_t>(1,packet->duration)*1000000/kAudioRate);
                    return true;
                }
                if(replay){
                    replay->pushAudio(i,{std::make_shared<const std::vector<uint8_t>>(packet->data,packet->data+packet->size),packet->pts,packet->duration,false});
                    return true;
                }
                packet->stream_index=track.stream->index;
                av_packet_rescale_ts(packet,AVRational{1,kAudioRate},track.stream->time_base);
                return av_interleaved_write_frame(mux,packet)>=0;
            });
            if(!ok)return false;
        }
        return true;
    }
    void writerMain(){
        std::vector<uint8_t> extradata;
        {
            std::unique_lock lock(outMutex);
            outCv.wait(lock,[&]{return headersReady||encoderDone;});
            if(headersReady)extradata=headers;
        }
        bool ok=!extradata.empty()&&(replay?openReplay(std::move(extradata)):!streamTargets.empty()?openStream(std::move(extradata)):openFile(std::move(extradata)));
        std::optional<VideoPacket> held;
        const int64_t frameUs=1000000/std::max(1u,fps);
        while(ok){
            std::deque<VideoPacket> batch;bool ended=false;
            {
                std::unique_lock lock(outMutex);
                outCv.wait_for(lock,std::chrono::milliseconds(20),[&]{return !videoPackets.empty()||encoderDone;});
                batch.swap(videoPackets);
                ended=encoderDone;
            }
            for(auto& packet:batch){
                backlogBytes-=packet.data.size();
                if(outputs){
                    ++videoWritten;writtenUs=packet.ptsUs;
                    outputs->pushVideo(std::make_shared<const std::vector<uint8_t>>(std::move(packet.data)),packet.ptsUs,frameUs,packet.key);
                    continue;
                }
                if(replay){
                    if(!originSet){replay->setOrigin(t0Us.load());originSet=true;}
                    ++videoWritten;writtenUs=packet.ptsUs;
                    replay->pushVideo({std::make_shared<const std::vector<uint8_t>>(std::move(packet.data)),packet.ptsUs,0,packet.key});
                    continue;
                }
                if(ok&&held)ok=writeVideo(*held,packet.ptsUs-held->ptsUs);
                held=std::move(packet);
            }
            if(ok&&audio)ok=processAudio(false,0);
            if(!ok){fail(replay?L"即时回放写入失败":outputs?L"直播的声音编码失败":L"写入录制文件失败（磁盘已满或没有写入权限？）");break;}
            if(ended)break;
        }
        // Drop what can no longer be written.
        {std::lock_guard lock(outMutex);for(auto& packet:videoPackets)backlogBytes-=packet.data.size();videoPackets.clear();}
        if(outputs){
            outputs->stop();
            std::string summary;
            for(const auto& t:outputs->status())
                summary+=std::format(" [{} {} sent={}B frames={} dropped={} reconnects={}{}]",t.name,stream::stateName(t.state),t.bytesSent,t.framesSent,t.framesDropped,t.reconnects,t.error.empty()?"":" error="+t.error);
            veyra::log::info("live-recorder",std::format("live stream closed videoFrames={} dropped={}{}",videoWritten.load(),dropped.load(),summary));
        }else if(replay){
            int64_t end=endUs.load();
            if(videoWritten.load())end=std::max(end,writtenUs.load()+frameUs);
            if(ok&&audio&&videoWritten.load())ok=processAudio(true,end);
            replay->finish(std::max<int64_t>(end,0));
            veyra::log::info("live-recorder",std::format("replay buffer closed videoFrames={} seconds={:.2f} audioGaps={} audioOverflow={} microphoneGaps={} microphoneOverflow={} dropped={} ok={}",
                videoWritten.load(),double(std::max<int64_t>(end,0))/1e6,soundLane.gaps.load(),soundLane.overflow.load(),micLane.gaps.load(),micLane.overflow.load(),dropped.load(),ok));
        }else if(headerWritten){
            int64_t end=endUs.load();
            if(held)end=std::max(end,held->ptsUs+frameUs);
            if(ok&&held)ok=writeVideo(*held,end-held->ptsUs);
            if(ok&&audio&&videoWritten.load())ok=processAudio(true,end);
            const int rc=av_write_trailer(mux);
            if(rc<0)ok=false;
            if(!ok&&!failed.load())fail(L"录制文件收尾失败");
            avio_closep(&mux->pb);
            if(!videoWritten.load()){
                std::error_code code;std::filesystem::remove(path,code);
                setNote(L"没有录到画面，未保存文件");
            }else if(ok&&!matroska){
                // PushFPS: rewrite the fragmented MP4 as a standard one that editors such as
                // Vegas Pro open (Mp4Finalize.h); on any problem the fragmented file stays.
                const auto started=std::chrono::steady_clock::now();
                const auto finished=finalizeMp4(std::filesystem::path(path));
                const double ms=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-started).count();
                if(finished.ok)veyra::log::info("live-recorder",std::format("file finalized path={} videoPackets={} audioPackets={} ms={:.0f}",utf8(path),finished.videoPackets,finished.audioPackets,ms));
                else{
                    veyra::log::warn("live-recorder",std::format("file kept fragmented path={} reason={} ms={:.0f}",utf8(path),finished.error,ms));
                    // A long recording on a nearly full disk: saved as it was written, which plays everywhere
                    // but which some editors refuse. Said, so nobody wonders why Vegas will not open it.
                    if(finished.noSpace&&noteText().empty())setNote(L"磁盘剩余空间不足，无法整理成标准 MP4：录制已按原样保存，可以正常播放，但部分剪辑软件打不开");
                }
            }
            // A clip at a chosen quality that lost frames: said, with what to do about it. (Automatic
            // quality keeps its silence: nothing about those clips changes.)
            if(compareQuality&&videoWritten.load()&&compareClipFellBehind(videoWritten.load(),dropped.load())&&noteText().empty())
                setNote(L"这段对比片段有一部分帧没能录下：显卡跟不上所选的片段画质，请选低一档再录");
            veyra::log::info("live-recorder",std::format("file closed path={} videoFrames={} seconds={:.2f} originUs={} audio={} audioGaps={} audioOverflow={} microphoneGaps={} microphoneOverflow={} soundBlocks={} mixerSilenced={} dropped={} ok={}{}",
                utf8(path),videoWritten.load(),double(end)/1e6,t0Us.load(),audio.load(),soundLane.gaps.load(),soundLane.overflow.load(),micLane.gaps.load(),micLane.overflow.load(),
                soundBlocks.load(),mixerSilenced.load(),dropped.load(),ok,
                compare?std::format(" compare real={} generated={} rawConsole={} droppedReal={}",compare->realFrames(),compare->generatedFrames(),compare->rawFrames(),droppedReal.load()):std::string{}));
        }
        releaseMux();
        writerFinished=true;
    }
};

// A replay save in progress: its own thread, so writing a long clip never
// holds up the buffer. Shared with that thread, which may outlive the
// recorder at exit (waitIdle detaches a stuck one).
struct ReplaySave {
    std::mutex m;
    bool done=false,ok=false;
    std::wstring path,error;
    double seconds=0;
    std::thread thread;
};

struct LiveRecorder::State {
    mutable std::mutex m;
    bool replayMode=false;
    bool streamMode=false;                  // going live (Mode::Stream)
    std::vector<stream::TargetStatus> lastStreams; // the outputs of the last stream segment
    RecordingSettings settings;
    bool armed=false;
    std::shared_ptr<Segment> current,pendingCapture;
    std::vector<std::shared_ptr<Segment>> finishing;
    std::wstring baseName;
    unsigned nextNumber=1;
    int64_t firstFrameUs=0,nextDueUs=0;
    double lastSeconds=0;
    std::atomic<double> presentFps{0};
    bool failed=false;
    std::wstring path,error,encoder,note;
    uint64_t framesBase=0,droppedBase=0,generatedBase=0;
    uint32_t files=0;
    uint64_t started=0,saved=0;
    bool audioTrack=false;
    // Instant replay recorder.
    std::shared_ptr<ReplayBuffer> frozen;   // the last stretch before a picture change or a new session
    std::shared_ptr<ReplaySave> save;
    int64_t retryAfterUs=0;

    void retire(const std::shared_ptr<Segment>& segment,const std::wstring& why){
        if(!segment)return;
        sink::removeRecordingAudioSink(segment.get());
        detachMicrophone(segment.get());
        detachPushAux(segment.get());
        if(!why.empty()&&segment->noteText().empty())segment->setNote(why);
        if(segment->outputs)lastStreams=segment->outputs->status();
        if(const auto name=segment->encoderText();!name.empty())encoder=name;
        segment->finish(sink::recordingClockUs());
        framesBase+=segment->frames.load();droppedBase+=segment->dropped.load();generatedBase+=segment->generated.load();
        // What it buffered stays saveable until the next stretch is as long.
        if(segment->replay&&segment->replay->coverage().picture)frozen=segment->replay;
        finishing.push_back(segment);
    }
    void reap(){
        if(current&&current->failed.load()){
            failed=true;error=current->errorText();
            if(replayMode||streamMode){
                // The buffer stays armed; the next picture tries again a little later.
                retryAfterUs=sink::recordingClockUs()+kReplayRetryUs;
                veyra::log::warn("live-recorder",std::format("replay buffer stopped on error, retrying in 5 s: {}",utf8(error)));
            }else{
                armed=false;
                if(firstFrameUs)lastSeconds=double(sink::recordingClockUs()-firstFrameUs)/1e6;
                veyra::log::warn("live-recorder",std::format("recording stopped on error: {}",utf8(error)));
            }
            retire(current,{});current.reset();
        }
        for(auto it=finishing.begin();it!=finishing.end();){
            auto& segment=*it;
            if(!segment->done()){++it;continue;}
            segment->join();
            if(!replayMode&&!streamMode&&segment->videoWritten.load()){path=segment->path;++saved;}
            if(segment->failed.load()&&error.empty()){failed=true;error=segment->errorText();}
            if(const auto text=segment->noteText();!text.empty())note=text;
            it=finishing.erase(it);
        }
        if(frozen&&current&&current->replay&&current->replay->coverage().seconds*1e6>=double(current->replay->keepUs())-1e5)frozen.reset();
        if(save){
            std::unique_lock lock(save->m);
            if(save->done){
                if(save->ok){path=save->path;++saved;failed=false;error.clear();note.clear();}
                else{failed=true;error=save->error;}
                lock.unlock();
                if(save->thread.joinable())save->thread.join();
                save.reset();
            }
        }
    }
    std::shared_ptr<ReplayBuffer> replayToSave() const {
        const auto ownBuffer=current&&current->replay?current->replay:nullptr;
        if(ownBuffer&&ownBuffer->coverage().seconds>=1.0)return ownBuffer;
        if(frozen&&frozen->coverage().picture)return frozen;
        if(ownBuffer&&ownBuffer->coverage().picture)return ownBuffer;
        return nullptr;
    }
};

LiveRecorder::LiveRecorder(bool replay):s_(std::make_unique<State>()){s_->replayMode=replay;}
uint32_t recordingAutoBitrateMbps(uint32_t width,uint32_t height,uint32_t fps,bool hevc){return autoBitrateMbps(width,height,fps,hevc);}
uint64_t replayMemoryCapBytes(){return defaultReplayMemory();}

LiveRecorder::LiveRecorder(Mode mode):s_(std::make_unique<State>()){s_->replayMode=mode==Mode::Replay;s_->streamMode=mode==Mode::Stream;}
LiveRecorder::~LiveRecorder(){waitIdle(8000);}

bool LiveRecorder::start(const RecordingSettings& settings,std::wstring& error){
    error.clear();
    auto& s=*s_;
    std::lock_guard lock(s.m);
    s.reap();
    if(s.streamMode){
        if(settings.streams.empty()){error=L"没有可用的直播目标";return false;}
        for(const auto& t:settings.streams)
            if(!stream::validServer(t.url)){error=L"直播地址无效："+wide(t.name);return false;}
        // Started again with other settings: the next picture opens new connections with them.
        if(s.current){s.retire(s.current,{});s.current.reset();}
        s.settings=settings;
        s.armed=true;s.failed=false;s.error.clear();s.note.clear();s.encoder.clear();s.framesBase=s.droppedBase=0;
        s.firstFrameUs=0;s.nextDueUs=0;s.retryAfterUs=0;s.lastStreams.clear();++s.started;
        std::string names;
        for(const auto& t:settings.streams)names+=(names.empty()?"":", ")+t.name+" "+stream::redact(t.url);
        veyra::log::info("live-recorder",std::format("live stream armed bitrateMbps={} fpsLimit={} audio={} microphone={} targets=[{}]",
            settings.bitrateMbps,settings.fpsLimit,settings.audio,settings.microphone,names));
        return true;
    }
    if(s.replayMode){
        // Armed again with other settings: the next picture starts a new
        // stretch with them; the current one stays saveable meanwhile.
        const auto& o=s.settings;
        const bool same=s.armed&&o.bitrateMbps==settings.bitrateMbps&&o.hevc==settings.hevc&&o.fpsLimit==settings.fpsLimit&&
            o.audio==settings.audio&&o.microphone==settings.microphone&&o.microphoneId==settings.microphoneId&&
            o.microphoneTrack==settings.microphoneTrack&&o.replaySeconds==settings.replaySeconds&&o.replayMemoryBytes==settings.replayMemoryBytes&&
            o.rateControl==settings.rateControl&&o.quality==settings.quality&&o.preset==settings.preset&&
            o.keyframeSeconds==settings.keyframeSeconds&&o.audioKbps==settings.audioKbps&&o.bitrateKbps==settings.bitrateKbps&&
            o.multipass==settings.multipass&&o.psychoAq==settings.psychoAq&&
            o.outputWidth==settings.outputWidth&&o.outputHeight==settings.outputHeight&&o.canvas.fit==settings.canvas.fit;
        s.settings=settings;
        if(s.settings.replaySeconds==0)s.settings.replaySeconds=60;
        if(same)return true;
        if(s.current){s.retire(s.current,{});s.current.reset();}
        if(!s.armed){s.failed=false;s.error.clear();s.note.clear();s.framesBase=s.droppedBase=0;++s.started;}
        s.armed=true;s.nextDueUs=0;s.retryAfterUs=0;
        veyra::log::info("live-recorder",std::format("replay buffer armed keepSeconds={} memoryCapMiB={} bitrateMbps={} hevc={} fpsLimit={} audio={} microphone={}",
            s.settings.replaySeconds,(s.settings.replayMemoryBytes?s.settings.replayMemoryBytes:defaultReplayMemory())>>20,settings.bitrateMbps,settings.hevc,settings.fpsLimit,settings.audio,
            settings.microphone?(settings.microphoneTrack?"track":"mixed"):"off"));
        return true;
    }
    if(s.armed){error=L"录制已在进行中";return false;}
    if(!s.finishing.empty()){error=L"上一段录制仍在保存，请稍后再试";return false;}
    if(settings.directory.empty()){error=L"未设置录制保存位置";return false;}
    std::error_code code;
    std::filesystem::create_directories(settings.directory,code);
    if(!std::filesystem::is_directory(settings.directory,code)){error=L"无法创建录制目录："+settings.directory;return false;}
    ULARGE_INTEGER available{};
    if(GetDiskFreeSpaceExW(settings.directory.c_str(),&available,nullptr,nullptr)&&available.QuadPart<(uint64_t(1)<<30)){
        error=L"录制目录所在磁盘剩余空间不足 1 GB";return false;
    }
    s.settings=settings;s.armed=true;s.failed=false;s.error.clear();s.note.clear();s.encoder.clear();
    s.framesBase=s.droppedBase=0;s.files=0;s.firstFrameUs=0;s.nextDueUs=0;s.lastSeconds=0;s.audioTrack=false;
    s.baseName=std::wstring(veyra::brand::kProductNameW)+(settings.compare?L"-compare-":L"-")+fileStamp();s.nextNumber=1;++s.started;
    s.generatedBase=0;
    veyra::log::info("live-recorder",std::format("armed directory={} bitrateMbps={} hevc={} fpsLimit={} audio={} microphone={}",
        utf8(settings.directory),settings.bitrateMbps,settings.hevc,settings.fpsLimit,settings.audio,
        settings.microphone?(settings.microphoneTrack?"track":"mixed"):"off"));
    return true;
}

void LiveRecorder::stop(const std::wstring& note){
    auto& s=*s_;
    std::lock_guard lock(s.m);
    s.reap();
    if(!s.armed)return;
    s.armed=false;
    if(s.firstFrameUs)s.lastSeconds=double(sink::recordingClockUs()-s.firstFrameUs)/1e6;
    if(s.current){s.retire(s.current,note);s.current.reset();}
    else if(!note.empty())s.note=note;
    // A replay buffer turned off lets go of its memory (a save in progress keeps its clip).
    if(s.replayMode)s.frozen.reset();
    veyra::log::info("live-recorder",std::format("{} requested note={}",s.replayMode?"replay buffer stop":s.streamMode?"live stream stop":"stop",utf8(note)));
}

bool LiveRecorder::active() const{std::lock_guard lock(s_->m);return s_->armed;}
bool LiveRecorder::wantsRawFrames() const{std::lock_guard lock(s_->m);return s_->armed&&s_->settings.compare&&!s_->replayMode&&!s_->streamMode;}

RecordingStatus LiveRecorder::status() const{
    auto& s=*s_;
    std::lock_guard lock(s.m);
    s.reap();
    RecordingStatus out;
    out.active=s.armed;out.failed=s.failed;out.finishing=!s.finishing.empty();
    out.error=s.error;out.note=s.note;out.files=s.files;out.started=s.started;out.saved=s.saved;
    out.frames=s.framesBase;out.dropped=s.droppedBase;out.path=s.path;out.encoder=s.encoder;out.audio=s.audioTrack;
    out.compare=s.settings.compare&&!s.replayMode;out.generatedFrames=s.generatedBase;
    if(s.current){
        if(!s.replayMode)out.path=s.current->path;
        out.frames+=s.current->frames.load();out.dropped+=s.current->dropped.load();
        if(s.current->compare)out.generatedFrames+=s.current->generated.load();
        if(const auto name=s.current->encoderText();!name.empty())out.encoder=name;
        out.audio=s.current->audio;
        out.microphone=s.current->microphone&&s.current->audio;
        if(const auto text=s.current->noteText();!text.empty())out.note=text;
    }
    if(out.microphone){
        const auto mic=microphoneStatus();
        if(!mic.open)out.microphoneError=mic.error;
        out.microphoneName=mic.device;
    }
    if(s.streamMode){
        out.streams=s.current&&s.current->outputs?s.current->outputs->status():s.lastStreams;
        out.path.clear();
        out.seconds=s.armed&&s.firstFrameUs?double(sink::recordingClockUs()-s.firstFrameUs)/1e6:s.lastSeconds;
        return out;
    }
    if(s.replayMode){
        out.saving=s.save!=nullptr;
        if(const auto buffer=s.replayToSave()){
            const auto coverage=buffer->coverage();
            out.bufferedSeconds=coverage.seconds;out.memoryLimited=coverage.limited;
        }
        for(const auto& buffer:{s.current&&s.current->replay?s.current->replay:nullptr,s.frozen})
            if(buffer)out.bufferedBytes+=buffer->coverage().bytes;
        out.seconds=out.bufferedSeconds;
        out.finishing=out.saving;
        return out;
    }
    out.seconds=s.armed?(s.firstFrameUs?double(sink::recordingClockUs()-s.firstFrameUs)/1e6:0.0):s.lastSeconds;
    return out;
}

bool LiveRecorder::saveReplay(const std::wstring& directory,std::wstring& error){
    error.clear();
    auto& s=*s_;
    std::lock_guard lock(s.m);
    s.reap();
    if(!s.replayMode||!s.armed){error=L"即时回放未开启";return false;}
    if(s.save){error=L"上一段回放仍在保存，请稍后再试";return false;}
    const auto buffer=s.replayToSave();
    if(!buffer){error=L"回放里还没有画面";return false;}
    if(directory.empty()){error=L"未设置录制保存位置";return false;}
    std::error_code code;
    std::filesystem::create_directories(directory,code);
    if(!std::filesystem::is_directory(directory,code)){error=L"无法创建录制目录："+directory;return false;}
    ULARGE_INTEGER available{};
    if(GetDiskFreeSpaceExW(directory.c_str(),&available,nullptr,nullptr)&&available.QuadPart<buffer->coverage().bytes+(uint64_t(256)<<20)){
        error=L"录制目录所在磁盘剩余空间不足，无法保存回放";return false;
    }
    auto job=std::make_shared<ReplaySave>();
    job->path=uniqueFile(directory,std::wstring(veyra::brand::kProductNameW)+L"-Replay-"+fileStamp());
    const int64_t request=sink::recordingClockUs();
    veyra::log::info("live-recorder",std::format("replay save requested path={} buffered={:.2f}s bytes={}",utf8(job->path),buffer->coverage().seconds,buffer->coverage().bytes));
    try{
        job->thread=std::thread([job,buffer,request]{
            // The sound reaches the buffer a little after the picture (the
            // endpoint latency): wait briefly so both end at the request.
            const auto deadline=std::chrono::steady_clock::now()+std::chrono::milliseconds(kReplaySaveWaitMs);
            while(!buffer->covers(request)&&std::chrono::steady_clock::now()<deadline)std::this_thread::sleep_for(std::chrono::milliseconds(10));
            const auto clip=buffer->clip(request);
            std::wstring reason;
            const auto began=std::chrono::steady_clock::now();
            const bool ok=writeReplayFile(job->path,clip,reason);
            const double seconds=double(clip.endUs-clip.startUs)/1e6;
            size_t sounds=0;for(const auto& track:clip.audioPackets)sounds+=track.size();
            veyra::log::info("live-recorder",std::format("replay saved ok={} path={} seconds={:.3f} originUs={} clipStartUs={} videoPackets={} audioPackets={} tracks={} bytes={} writeMs={} error={}",
                ok,utf8(job->path),seconds,clip.originUs,clip.startUs,clip.videoPackets.size(),sounds,clip.audio.size(),clip.bytes,
                std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now()-began).count(),utf8(reason)));
            std::lock_guard lock(job->m);
            job->ok=ok;job->error=reason;job->seconds=seconds;job->done=true;
        });
    }catch(...){error=L"无法开始保存回放";return false;}
    s.save=std::move(job);
    return true;
}

void LiveRecorder::setPresentRateHint(double fps){if(std::isfinite(fps)&&fps>0)s_->presentFps=fps;}

void LiveRecorder::sessionEnded(const std::wstring& note){
    auto& s=*s_;
    if(!s.replayMode&&!s.streamMode){stop(note);return;}
    // The buffer belongs to the device that ends; the next session starts a
    // new stretch, and this one stays saveable until then.
    std::lock_guard lock(s.m);
    s.reap();
    if(s.current){s.retire(s.current,{});s.current.reset();}
}

void LiveRecorder::waitIdle(uint32_t timeoutMs){
    stop();
    const auto deadline=std::chrono::steady_clock::now()+std::chrono::milliseconds(timeoutMs);
    for(;;){
        {
            std::lock_guard lock(s_->m);
            s_->reap();
            if(s_->finishing.empty()&&!s_->save)return;
            if(std::chrono::steady_clock::now()>=deadline){
                // A stuck encoder must not hang the exit: leave its threads running.
                for(auto& segment:s_->finishing){
                    veyra::log::error("live-recorder",std::format("segment not finished at exit path={}",segment->name()));
                    if(segment->encoderThread.joinable())segment->encoderThread.detach();
                    if(segment->writerThread.joinable())segment->writerThread.detach();
                    // Intentionally process-lifetime storage: a static vector
                    // destructor could free a Segment under its detached worker.
                    static auto* abandoned=new std::vector<std::shared_ptr<Segment>>;
                    abandoned->push_back(segment);
                }
                s_->finishing.clear();
                if(s_->save){
                    // The save keeps its own state and buffer alive.
                    veyra::log::error("live-recorder","replay save not finished at exit");
                    if(s_->save->thread.joinable())s_->save->thread.detach();
                    s_->save.reset();
                }
                return;
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
}

bool LiveRecorder::record(ID3D12GraphicsCommandList* list,const Frame& frame){
    auto& s=*s_;
    std::lock_guard lock(s.m);
    if(!s.armed||!list||!frame.texture)return false;
    if(s.pendingCapture){s.pendingCapture->discardPending();s.pendingCapture.reset();}
    if(s.current&&s.current->failed.load())s.reap();
    if(!s.armed)return false;
    const int64_t now=sink::recordingClockUs();
    if((s.replayMode||s.streamMode)&&!s.current&&now<s.retryAfterUs)return false;
    if(s.settings.fpsLimit){
        const int64_t interval=1000000/int64_t(s.settings.fpsLimit);
        if(s.nextDueUs&&now+interval/4<s.nextDueUs)return false;
        s.nextDueUs=(s.nextDueUs&&now-s.nextDueUs<interval)?s.nextDueUs+interval:now+interval;
    }
    if(s.current&&!s.current->matches(frame)){
        s.reap();
        if(!s.replayMode&&!s.streamMode&&s.finishing.size()>=2){
            s.armed=false;s.failed=true;s.error=L"录制文件仍在保存，已停止录制以避免积压";
            s.retire(s.current,{});s.current.reset();return false;
        }
        veyra::log::info("live-recorder",std::format("picture changed {}x{} hdr={} -> {}x{} hdr={}; continuing in a new {}",
            s.current->frameWidth,s.current->frameHeight,s.current->hdrMode,frame.width,frame.height,frame.hdrMode,s.replayMode?"replay buffer":"file"));
        s.retire(s.current,(s.replayMode||s.streamMode)?std::wstring{}:L"画面尺寸或 HDR 模式变化，已接着写入新文件");s.current.reset();
    }
    // PushFPS: long recordings in parts (new file every N minutes, -2, -3, ...).
    if(s.current&&!s.replayMode&&!s.streamMode&&s.settings.splitMinutes&&!s.current->compareClip){
        const int64_t started=s.current->t0Us.load();
        if(started&&now-started>=int64_t(s.settings.splitMinutes)*60000000){
            veyra::log::info("live-recorder",std::format("split after {} minutes; continuing in a new file",s.settings.splitMinutes));
            s.reap();
            s.retire(s.current,{});s.current.reset();
        }
    }
    if(!s.current){
        // Rapid source/settings changes must not create an unbounded number
        // of encoder workers while the previous segments are draining.
        s.reap();
        if((s.replayMode||s.streamMode)&&s.finishing.size()>=2)return false;
        auto fail=[&](const std::wstring& reason){
            s.failed=true;s.error=reason;
            if(s.replayMode||s.streamMode)s.retryAfterUs=now+kReplayRetryUs;else s.armed=false;
            veyra::log::error("live-recorder",std::format("cannot {}: {}",s.replayMode?"buffer replay":"record",utf8(reason)));
            return false;
        };
        auto segment=std::make_shared<Segment>();
        if(FAILED(frame.texture->GetDevice(IID_PPV_ARGS(&segment->device))))return fail(L"无法获取当前显卡");
        segment->vendor=adapterVendor(segment->device.Get());
        segment->nvenc=segment->vendor==0x10DE&&GetEnvironmentVariableW(L"VEYRA_TEST_FORCE_MF_ENCODER",nullptr,0)==0;
        if(frame.hdrMode&&s.streamMode)return fail(L"直播需要 SDR 画面：请关闭 HDR 输出后再开播");
        if(frame.hdrMode&&!segment->nvenc)return fail(L"HDR 画面录制需要 NVIDIA 显卡（HEVC 10bit）；请关闭 HDR 输出后再录制");
        if(frame.width<16||frame.height<16||(frame.width&1)||(frame.height&1))
            return fail(std::format(L"当前画面尺寸 {}×{} 无法录制",frame.width,frame.height));
        segment->width=frame.width;segment->height=frame.height;segment->hdrMode=frame.hdrMode;
        segment->frameWidth=frame.width;segment->frameHeight=frame.height;
        // PushFPS outputs and canvas: this output's own size and the game picture's place on it.
        // Comparison clips keep their own layout. Streams stay within 4096 per side (H.264).
        if(!(s.settings.compare&&!s.replayMode)){
            uint32_t ow=0,oh=0;
            pushOutputSize(s.settings.outputWidth,s.settings.outputHeight,frame.width,frame.height,s.streamMode?4096u:8192u,ow,oh);
            segment->canvas=s.settings.canvas.sanitized();
            segment->canvasPass=ow!=frame.width||oh!=frame.height;
            segment->liveLayout=true;
            segment->width=ow;segment->height=oh;
            segment->planeWidth=ow;segment->planeHeight=oh;
        }
        // PushFPS comparison clips (split, side by side) record every presented frame: at 4K and 180 FPS
        // that is more than a hardware encoder takes, real frames were dropped and the console side
        // froze or showed a picture seconds old (field report 2026-11-17). They are recorded at most
        // 1440 pixels high, scaled once by PushCanvasConvert; the vertical layout has its own 1080 x 1920.
        // Split and Reveal clips take the height the user chose instead (compareClipHeightCap: 1080, 1440
        // or 2160; a picture is never enlarged): the encode is then tuned to keep up (compareClipPreset).
        else if(const uint32_t cap=compareClipHeightCap(s.settings.compareLayout,s.settings.compareMaxHeight);
                !s.replayMode&&s.settings.compareLayout!=CompareLayout::Vertical&&frame.height>cap){
            const uint32_t oh=cap;
            const uint32_t ow=std::max<uint32_t>(16,uint32_t(std::lround(double(frame.width)*oh/frame.height))&~1u);
            segment->canvas=PushCanvas{};segment->canvasPass=true;segment->liveLayout=false;
            segment->width=ow;segment->height=oh;segment->planeWidth=ow;segment->planeHeight=oh;
        }
        if(s.streamMode&&(segment->width>4096||segment->height>4096))return fail(std::format(L"当前画面尺寸 {}×{} 太大，无法直播",segment->width,segment->height));
        // H.264 hardware encoders stop at 4096 pixels per side.
        segment->hevc=s.settings.hevc||frame.hdrMode!=0||segment->width>4096||segment->height>4096;
        const double hint=s.presentFps.load();
        segment->fps=s.settings.fpsLimit?s.settings.fpsLimit:uint32_t(std::clamp<long>(std::lround(hint>1?hint:60.0),10,480));
        segment->bitrateMbps=s.settings.bitrateMbps?s.settings.bitrateMbps:s.streamMode?6u:autoBitrateMbps(segment->width,segment->height,segment->fps,segment->hevc);
        if(s.settings.bitrateKbps){
            segment->bitrateKbps=std::clamp(s.settings.bitrateKbps,300u,400000u);
            segment->bitrateMbps=std::max(1u,(segment->bitrateKbps+500)/1000);
        }
        segment->sound=s.settings.audio;
        segment->microphone=s.settings.microphone;
        segment->microphoneTrack=s.settings.microphoneTrack;
        // PushFPS streaming mode mixer: desktop audio and the extra input go wherever sound does.
        segment->aux=s.settings.audio&&pushAuxEnabled();
        segment->rateControl=std::min(s.settings.rateControl,2u);segment->quality=std::clamp(s.settings.quality,1u,51u);
        segment->preset=std::clamp(s.settings.preset,1u,7u);segment->keyframeSeconds=std::clamp(s.settings.keyframeSeconds,1u,10u);
        segment->audioKbps=std::clamp(s.settings.audioKbps,64u,320u);
        segment->multipass=std::min(s.settings.multipass,2u);segment->psychoAq=s.settings.psychoAq;
        segment->matroska=s.settings.matroska&&!s.replayMode;
        segment->compareClip=s.settings.compare&&!s.replayMode;
        segment->compareLabels=s.settings.compareLabels;
        segment->compareLayout=s.settings.compareLayout;
        segment->compareRevealSeconds=s.settings.compareRevealSeconds;
        // A chosen clip quality (Split and Reveal): a third more bits than a recording of that size
        // gets, and an encoder preset the card keeps up with at that many pixels a second. Automatic
        // (0), side by side and vertical clips are encoded exactly as before.
        segment->compareQuality=segment->compareClip&&compareClipHeightChosen(s.settings.compareLayout,s.settings.compareMaxHeight)?s.settings.compareMaxHeight:0;
        if(segment->compareQuality){
            if(!s.settings.bitrateMbps&&!s.settings.bitrateKbps)segment->bitrateMbps=compareClipBitrateMbps(segment->bitrateMbps);
            segment->preset=compareClipPreset(segment->preset,segment->width,segment->height,segment->fps);
        }
        segment->audio=segment->sound||segment->microphone;
        if(s.streamMode){
            // Platforms take H.264 + AAC at a constant bitrate with a keyframe every 2 s (OBS's
            // rtmp-services recommendations); the microphone is mixed into the one sound track.
            segment->hevc=false;segment->rateControl=0;segment->keyframeSeconds=std::clamp(s.settings.keyframeSeconds,1u,4u);
            segment->microphoneTrack=false;segment->matroska=false;segment->compareClip=false;
            segment->audioKbps=std::clamp(s.settings.audioKbps,64u,320u);
            segment->streamTargets=s.settings.streams;
        }else if(s.replayMode){
            const uint64_t cap=s.settings.replayMemoryBytes?s.settings.replayMemoryBytes:defaultReplayMemory();
            segment->replay=std::make_shared<ReplayBuffer>(int64_t(s.settings.replaySeconds)*1000000,size_t(cap));
        }else{
            const auto base=s.baseName+(s.nextNumber>1?std::format(L"-{}",s.nextNumber):L"");
            segment->path=uniqueFile(s.settings.directory,base,segment->matroska?L".mkv":L".mp4");
        }
        if(segment->compareClip&&segment->compareLayout==CompareLayout::Vertical){
            // Vertical comparison clips are a 1080 x 1920 file of their own size: H.264 (what the
            // short-video apps take best) unless HDR or the settings ask for HEVC.
            compareOutputSize(segment->compareLayout,frame.width,frame.height,segment->width,segment->height);
            segment->hevc=s.settings.hevc||frame.hdrMode!=0;
            if(!s.settings.bitrateMbps)segment->bitrateMbps=autoBitrateMbps(segment->width,segment->height,segment->fps,segment->hevc);
        }
        std::wstring reason;
        if(!segment->create(reason))return fail(reason);
        s.current=segment;++s.files;++s.nextNumber;s.audioTrack=segment->audio;
        if(segment->sound)sink::addRecordingAudioSink(segment);
        if(segment->microphone)attachMicrophone(segment,s.settings.microphoneId);
        if(segment->aux)attachPushAux(segment);
        veyra::log::info("live-recorder",std::format("{} {} path={} extent={}x{} hdr={} codec={} fps={} bitrateMbps={} encoder={} audio={} microphone={}",
            s.replayMode?"replay buffer":s.streamMode?"live stream":"file",s.files,segment->name(),segment->width,segment->height,segment->hdrMode,segment->hevc?"HEVC":"H264",segment->fps,segment->bitrateMbps,
            segment->nvenc?"NVENC":"MediaFoundation",segment->sound,segment->microphone?(segment->microphoneTrack?"track":"mixed"):"off"));
    }
    if(!s.current->capture(list,frame,now))return false;
    if(!s.firstFrameUs)s.firstFrameUs=now;
    s.pendingCapture=s.current;
    return true;
}

void LiveRecorder::submitted(ID3D12Fence* fence,uint64_t value){
    std::lock_guard lock(s_->m);
    if(s_->pendingCapture){s_->pendingCapture->submittedPending(fence,value);s_->pendingCapture.reset();}
}

void LiveRecorder::discarded(){
    std::lock_guard lock(s_->m);
    if(s_->pendingCapture){s_->pendingCapture->discardPending();s_->pendingCapture.reset();}
}

} // namespace veyra::engine
