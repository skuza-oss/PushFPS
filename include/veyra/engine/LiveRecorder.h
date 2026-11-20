#pragma once

// LiveRecorder: records the processed picture the player presents, with the
// sound it plays.
//
// Picture: VideoPresenter calls record() inside its presentation command list
// (PresentCapture), so turning the frame into NV12/P010 costs one compute pass
// on the GPU and nothing on the CPU; the engine thread never waits. A frame
// that finds every slot still encoding, or the disk behind, is skipped and
// counted instead of stalling playback. NVENC (NVIDIA) waits for the
// conversion on the GPU; the Media Foundation path (AMD, Intel) reads it back
// on the recorder's own thread.
// Sound: the PCM every renderer plays (sink/RecordingAudioTap.h), before
// volume and mute, timed by when it reaches the endpoint; AAC 48 kHz stereo.
// Gaps become silence and clock drift is absorbed by resampling, so the track
// stays continuous and in sync with the picture.
// Microphone (#101, MicrophoneCapture.h): timed by when it was captured,
// mixed into the sound track or on a second track of its own.
// File: fragmented MP4, so a crash keeps everything written so far. Settings
// changes keep the file; a change of the output size or HDR mode continues in
// a new file (-2, -3, ...). Files are finished on the recorder's threads.
// Instant replay (#103): a second recorder, built with replay=true, encodes
// the same way into memory (ReplayBuffer.h) and keeps the last few seconds or
// minutes; saveReplay() writes them to a new MP4 on its own thread. It stays
// armed across sessions; a new session or picture size starts a new stretch,
// and the previous one stays saveable until the new one is as long.
#include <cstdint>
#include <memory>
#include <string>

#include "veyra/engine/LiveStream.h"
#include "veyra/engine/PushCanvas.h"
#include "veyra/engine/PresentCapture.h"
#include "veyra/engine/RecordingCompare.h"

namespace veyra::engine {

struct RecordingSettings {
    std::wstring directory;      // output folder (created when missing)
    uint32_t bitrateMbps=0;      // 0: automatic from the picture size and frame rate
    uint32_t bitrateKbps=0;      // PushFPS output presets: the same in kbps (wins when set)
    // PushFPS outputs and canvas (PushCanvas.h): the size this output is encoded at (0 x 0: the
    // presented picture's), where the game picture sits on it, and new files every N minutes.
    uint32_t outputWidth=0,outputHeight=0;
    PushCanvas canvas;
    uint32_t splitMinutes=0;     // recordings only; 0: one file
    bool hevc=false;             // H.264 unless chosen; HDR always records HEVC 10-bit
    uint32_t fpsLimit=0;         // 0: every presented frame
    bool audio=true;             // record the sound the player plays
    bool microphone=false;       // record a microphone
    std::wstring microphoneId;   // its endpoint; empty: the default recording device
    bool microphoneTrack=false;  // on a track of its own instead of mixed into the sound
    // PushFPS advanced recording (设置 → 录制 → 高级). The defaults are the
    // standard recording; the interface only changes them when Advanced is on.
    uint32_t rateControl=1;      // sink::ExportRateControl: 0 CBR, 1 VBR, 2 constant quality
    uint32_t quality=22;         // constant quality level 1..51, lower is better (rateControl 2)
    uint32_t preset=4;           // encoder speed/quality 1 (fastest) .. 7 (best)
    uint32_t keyframeSeconds=2;  // keyframe interval
    uint32_t audioKbps=192;      // AAC bitrate per track
    bool matroska=false;         // MKV instead of fragmented MP4 (recordings; replays stay MP4)
    // PushFPS encoder tuning (NVENC; other encoders ignore it): a two-pass encode (1 quarter, 2 full
    // resolution first pass) and psycho-visual tuning (spatial and temporal AQ). Recordings, replays
    // and going live.
    uint32_t multipass=0;
    bool psychoAq=false;
    // PushFPS before/after comparison clip (RecordingCompare.h): left half the
    // console's real frames, right half every presented frame. Recordings only.
    bool compare=false;
    CompareLayout compareLayout=CompareLayout::Split; // split, side by side, vertical or reveal
    // Split and Reveal clips: the highest the clip is recorded (1080, 1440 or 2160; a larger
    // picture is scaled down, a smaller one never enlarged). 0: automatic, at most 1440, with the
    // standard bitrate, as every such clip was before 3.2.5. A chosen height also asks for the
    // best-looking encode (compareClipBitrateMbps, compareClipPreset in RecordingComparePlan.h).
    // Side by side and Vertical ignore it.
    uint32_t compareMaxHeight=0;
    double compareRevealSeconds=3.0;  // Reveal: a sweep starts every so many seconds (compareRevealInterval)
    RecordingLabelRenderer compareLabels; // optional labels, called per file on the engine thread
    // Instant replay recorder only.
    uint32_t replaySeconds=60;   // how much the buffer keeps
    uint64_t replayMemoryBytes=0;// memory cap; 0: an eighth of the installed memory, 512 MB to 4 GB
    // PushFPS going live (Mode::Stream): where the stream goes, each with its own connection.
    std::vector<stream::Target> streams;
};

struct RecordingStatus {
    bool active=false;           // recording, or waiting for the first frame
    bool failed=false;           // the last recording stopped on an error
    bool finishing=false;        // a file is still being finished in the background
    std::wstring path;           // the file being written, or the last one finished
    std::wstring error;          // why the last recording failed
    std::wstring encoder;        // e.g. "NVIDIA NVENC H.264"
    std::wstring note;           // stop reason or a new-file note, for the interface
    uint64_t frames=0;           // frames encoded since start
    uint64_t dropped=0;          // presented frames skipped because the encoder or disk was busy
    double seconds=0;            // time since the first recorded frame
    uint32_t files=0;            // files this recording wrote (more than one after a size change)
    bool compare=false;          // a comparison clip
    uint64_t generatedFrames=0;  // comparison clip: generated frames recorded so far (0: both halves match)
    bool audio=false;            // the current file has a sound track
    bool microphone=false;       // it records the microphone
    std::wstring microphoneError;// the microphone cannot be opened (its track continues silent)
    std::wstring microphoneName; // the endpoint in use
    uint64_t started=0;          // +1 per start
    uint64_t saved=0;            // +1 per finished file (replay: per saved clip); path is that file while not active
    // Instant replay recorder only.
    double bufferedSeconds=0;    // what a save now would hold
    uint64_t bufferedBytes=0;    // memory the buffer holds
    bool memoryLimited=false;    // the memory cap keeps less than the chosen length
    bool saving=false;           // a clip is being written
    std::vector<stream::TargetStatus> streams; // going live: each output's connection
};

// What the interface needs to say how much memory instant replay takes before it runs: the automatic
// bitrate for a picture (the recorder's own rule) and the replay buffer's memory cap on this PC.
uint32_t recordingAutoBitrateMbps(uint32_t width,uint32_t height,uint32_t fps,bool hevc);
uint64_t replayMemoryCapBytes();

class LiveRecorder final : public PresentCapture {
public:
    // File: recordings. Replay: the instant replay buffer. Stream: going live (RTMP/RTMPS outputs,
    // H.264 + AAC, constant bitrate); like the replay buffer it stays on across sessions and picture
    // changes (a new picture size reconnects) and retries after an error.
    enum class Mode{File,Replay,Stream};
    explicit LiveRecorder(Mode mode);
    explicit LiveRecorder(bool replay=false);
    ~LiveRecorder() override;
    LiveRecorder(const LiveRecorder&)=delete;
    LiveRecorder& operator=(const LiveRecorder&)=delete;

    // Any thread. Checks the folder; the file opens with the next presented frame.
    // Replay recorder: arms the buffer, or applies new settings to it.
    bool start(const RecordingSettings& settings,std::wstring& error);
    // Any thread. The file is finished in the background; a replay buffer is let go.
    void stop(const std::wstring& note={});
    // Replay recorder, any thread: writes what the buffer holds now to a new
    // file in `directory`, in the background; false with a reason when
    // nothing can be saved. status().saved counts the finished clips.
    bool saveReplay(const std::wstring& directory,std::wstring& error);
    bool active() const;
    RecordingStatus status() const;
    // Engine thread: the presented frame rate, used for rate control when
    // every presented frame is recorded.
    void setPresentRateHint(double fps);
    // Engine thread: the session that owns the device ended; so does the
    // recording. The replay buffer stays armed for the next session.
    void sessionEnded(const std::wstring& note);
    // Stops and waits until every file is finished, at most timeoutMs.
    void waitIdle(uint32_t timeoutMs);

    // PresentCapture: engine thread, inside VideoPresenter::present().
    bool record(ID3D12GraphicsCommandList* list,const Frame& frame) override;
    void submitted(ID3D12Fence* fence,uint64_t value) override;
    void discarded() override;
    // PushFPS: a comparison clip is recording, so the presenter should pass the raw console picture.
    bool wantsRawFrames() const override;

    struct Segment;
private:
    struct State;
    std::unique_ptr<State> s_;
};

} // namespace veyra::engine
