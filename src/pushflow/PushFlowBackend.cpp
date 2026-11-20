// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 DosukaSOL and the PushFPS contributors. Part of PushFPS, a modified version of Veyra.
// PushFlow: RIFE through ONNX Runtime + DirectML on the graph's D3D12 queue (see PushFlowBackend.h).
#include "veyra/pushflow/PushFlowBackend.h"
#include "veyra/pipeline/GpuPassUtils.h"
#include "veyra/Log.h"
#include "veyra/RuntimePaths.h"

#include <windows.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <format>
#include <fstream>
#include <functional>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#ifdef VEYRA_HAVE_ONNXRUNTIME
#include <DirectML.h>
#include <onnxruntime_c_api.h>
#include <dml_provider_factory.h>
#endif

namespace veyra::pushflow {
namespace fs = std::filesystem;
#ifdef VEYRA_HAVE_ONNXRUNTIME
namespace {
// ONNX Runtime faults are caught here instead of ending PushFPS (field report 2026-11-17: an access
// violation inside onnxruntime.dll's ReleaseSession on an RTX 5090, on every connect). Structured
// exception handling needs functions without C++ objects to unwind, hence these small wrappers.
// After a fault the object is leaked and PushFlow reports itself failed; the crash marker stays.
bool sehReleaseSession(const OrtApi* api, OrtSession* s) { __try { api->ReleaseSession(s); return true; } __except (EXCEPTION_EXECUTE_HANDLER) { return false; } }
bool sehReleaseIoBinding(const OrtApi* api, OrtIoBinding* b) { __try { api->ReleaseIoBinding(b); return true; } __except (EXCEPTION_EXECUTE_HANDLER) { return false; } }
bool sehReleaseValue(const OrtApi* api, OrtValue* v) { __try { api->ReleaseValue(v); return true; } __except (EXCEPTION_EXECUTE_HANDLER) { return false; } }
bool sehReleaseMemoryInfo(const OrtApi* api, OrtMemoryInfo* m) { __try { api->ReleaseMemoryInfo(m); return true; } __except (EXCEPTION_EXECUTE_HANDLER) { return false; } }
bool sehReleaseEnv(const OrtApi* api, OrtEnv* e) { __try { api->ReleaseEnv(e); return true; } __except (EXCEPTION_EXECUTE_HANDLER) { return false; } }
bool sehFreeAllocation(const OrtDmlApi* dml, void* a) { __try { dml->FreeGPUAllocation(a); return true; } __except (EXCEPTION_EXECUTE_HANDLER) { return false; } }
// `faulted` is set when the call itself faulted (the returned status is then null).
OrtStatus* sehCreateSession(const OrtApi* api, const OrtEnv* env, const wchar_t* model, const OrtSessionOptions* options, OrtSession** out, bool* faulted) {
    __try { return api->CreateSession(env, model, options, out); } __except (EXCEPTION_EXECUTE_HANDLER) { *faulted = true; *out = nullptr; return nullptr; }
}
OrtStatus* sehRun(const OrtApi* api, OrtSession* s, const OrtRunOptions* o, OrtIoBinding* b, bool* faulted) {
    __try { return api->RunWithBinding(s, o, b); } __except (EXCEPTION_EXECUTE_HANDLER) { *faulted = true; return nullptr; }
}
OrtStatus* sehSync(const OrtApi* api, OrtIoBinding* b, bool* faulted) {
    __try { return api->SynchronizeBoundOutputs(b); } __except (EXCEPTION_EXECUTE_HANDLER) { *faulted = true; return nullptr; }
}
OrtStatus* sehCreateAllocator(const OrtApi* api, const OrtSession* s, const OrtMemoryInfo* m, OrtAllocator** out, bool* faulted) {
    __try { return api->CreateAllocator(s, m, out); } __except (EXCEPTION_EXECUTE_HANDLER) { *faulted = true; *out = nullptr; return nullptr; }
}
bool sehReleaseAllocator(const OrtApi* api, OrtAllocator* a) { __try { api->ReleaseAllocator(a); return true; } __except (EXCEPTION_EXECUTE_HANDLER) { return false; } }

// VEYRA_PUSHFLOW_TEST_UNGUARDED=1 (PushFlowBench --teardown-check): the session teardown of 3.2.0,
// to measure what the guard changes. Never set in the product.
bool unguardedForTest() { static const bool v = GetEnvironmentVariableW(L"VEYRA_PUSHFLOW_TEST_UNGUARDED", nullptr, 0) > 0; return v; }
}
#endif
namespace { std::atomic<uint64_t> g_faults{0}; }
uint64_t faultCount() { return g_faults.load(std::memory_order_relaxed); }
using Microsoft::WRL::ComPtr;

namespace {
ComPtr<ID3D12Resource> readbackBuffer(ID3D12Device* device, uint64_t size) {
    D3D12_HEAP_PROPERTIES hp{}; hp.Type = D3D12_HEAP_TYPE_READBACK;
    D3D12_RESOURCE_DESC bd{}; bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER; bd.Width = size; bd.Height = 1; bd.DepthOrArraySize = 1;
    bd.MipLevels = 1; bd.SampleDesc.Count = 1; bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ComPtr<ID3D12Resource> r;
    if (FAILED(device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&r)))) return nullptr;
    return r;
}
constexpr uint32_t kBlock = 64;   // RIFE v4 works on 1/32-size features; 64 keeps every level whole
constexpr size_t kTelemetry = 240;
const wchar_t* modelFile(uint32_t tier, bool half) {
    if (tier >= 2) return half ? L"pushflow-quality-fp16.onnx" : L"pushflow-quality.onnx";
    return half ? L"pushflow-fast-fp16.onnx" : L"pushflow-fast.onnx";
}
// PushFlow 2's split models of a tier: part 0 the first-frame encoder, 1 the second-frame encoder, 2 the rest.
const wchar_t* splitFile(uint32_t tier, int part) {
    static const wchar_t* const fast[3] = {L"pushflow-fast-enc0.onnx", L"pushflow-fast-enc1.onnx", L"pushflow-fast-body.onnx"};
    static const wchar_t* const quality[3] = {L"pushflow-quality-enc0.onnx", L"pushflow-quality-enc1.onnx", L"pushflow-quality-body.onnx"};
    return (tier >= 2 ? quality : fast)[std::clamp(part, 0, 2)];
}
std::string narrow(const std::wstring& w) { std::string s; for (wchar_t c : w) s.push_back(c < 128 ? char(c) : '?'); return s; }
DXGI_FORMAT viewFormat(DXGI_FORMAT f) {
    switch (f) {
    case DXGI_FORMAT_R16G16B16A16_TYPELESS: return DXGI_FORMAT_R16G16B16A16_FLOAT;
    case DXGI_FORMAT_R10G10B10A2_TYPELESS: return DXGI_FORMAT_R10G10B10A2_UNORM;
    case DXGI_FORMAT_R8G8B8A8_TYPELESS: case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB: return DXGI_FORMAT_R8G8B8A8_UNORM;
    case DXGI_FORMAT_B8G8R8A8_TYPELESS: case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB: return DXGI_FORMAT_B8G8R8A8_UNORM;
    default: return f;
    }
}
// Descriptor slots: each pass keeps its UAVs first and a ring of SRV pairs after them, so a pass recorded
// while the GPU still reads the previous pair's descriptors never rewrites them.
constexpr UINT kSrvRing = 8;
D3D12_CPU_DESCRIPTOR_HANDLE cpuSlot(const pipeline::ComputePass& pass, UINT slot) {
    return {pass.heap->GetCPUDescriptorHandleForHeapStart().ptr + SIZE_T(slot) * pass.increment};
}
uint64_t gpuSlot(const pipeline::ComputePass& pass, UINT slot) {
    return pass.heap->GetGPUDescriptorHandleForHeapStart().ptr + uint64_t(slot) * pass.increment;
}
float bits(uint32_t v) { return std::bit_cast<float>(v); }
// Per-model facts written by prepare_runtime.py next to each model (<model>.txt: key=value lines).
struct ModelFacts { bool known = false; bool alignCorners = true; bool maskForFirst = true; uint32_t pad = kBlock; uint32_t splitFeatures = 0; };
ModelFacts readFacts(const fs::path& model) {
    ModelFacts f;
    std::ifstream in(fs::path(model).concat(L".txt"));
    std::string line;
    while (std::getline(in, line)) {
        const auto eq = line.find('=');
        if (eq == std::string::npos) continue;
        const std::string key = line.substr(0, eq), value = line.substr(eq + 1);
        if (key == "align_corners") { f.alignCorners = value.rfind('1', 0) == 0; f.known = true; }
        if (key == "mask_for_first") f.maskForFirst = value.rfind('1', 0) == 0;
        // The block size the model pads to inside; PushFlow pads to it itself so the grids match its size.
        if (key == "pad") { const int v = std::atoi(value.c_str()); if (v >= 8 && v <= 512 && (v & (v - 1)) == 0) f.pad = uint32_t(v); }
        // The split models' feature channels per frame (prepare_runtime.py writes it only when they reproduce the model).
        if (key == "split_features") { const int v = std::atoi(value.c_str()); if (v >= 1 && v <= 64) f.splitFeatures = uint32_t(v); }
    }
    return f;
}
// Deterministic start-up pattern for the precision self-check: a textured square moving 16 pixels over
// a gradient, both frames planar RGB 0..1 at the network's size, t = 0.5.
std::vector<float> selfCheckInput(uint32_t w, uint32_t h, uint32_t channels) {
    const size_t plane = size_t(w) * h;
    std::vector<float> t(plane * channels);
    for (uint32_t y = 0; y < h; ++y)
        for (uint32_t x = 0; x < w; ++x) {
            const size_t i = size_t(y) * w + x;
            for (int f = 0; f < 2; ++f) {
                float r = 0.24f + 0.16f * x / w + 0.12f * y / h, g = r + 0.04f, b = r + 0.08f;
                const float dx = float(x) - (float(w) * 0.35f + 16.0f * f), dy = float(y) - float(h) * 0.35f;
                if (dx >= 0 && dx < 40 && dy >= 0 && dy < 40) {
                    const bool light = ((int(dx) / 8) + (int(dy) / 8)) % 2 == 0;
                    r = light ? 0.92f : 0.16f; g = light ? 0.78f : 0.12f; b = light ? 0.24f : 0.08f;
                }
                t[(f * 3 + 0) * plane + i] = r; t[(f * 3 + 1) * plane + i] = g; t[(f * 3 + 2) * plane + i] = b;
            }
            t[6 * plane + i] = 0.5f;
            if (channels == 11) {
                t[7 * plane + i] = 2.0f / float(w - 1); t[8 * plane + i] = 2.0f / float(h - 1);
                t[9 * plane + i] = 2.0f * x / float(w - 1) - 1.0f; t[10 * plane + i] = 2.0f * y / float(h - 1) - 1.0f;
            }
        }
    return t;
}
} // namespace

std::wstring pushFlowDirectory(const std::wstring& runtimeDir) { return (fs::path(runtimeDir) / L"pushflow").wstring(); }

bool runtimeInstalled(const std::wstring& runtimeDir, std::wstring* reason) {
#ifndef VEYRA_HAVE_ONNXRUNTIME
    (void)runtimeDir;
    if (reason) *reason = L"PushFlow 未编译进这个版本";
    return false;
#else
    const fs::path dir = pushFlowDirectory(runtimeDir);
    std::error_code ec;
    for (const wchar_t* f : {L"onnxruntime.dll", L"DirectML.dll", L"models\\pushflow-fast.onnx", L"models\\pushflow-quality.onnx"})
        if (!fs::is_regular_file(dir / f, ec)) {
            if (reason) *reason = L"缺少 PushFlow 运行组件：runtime\\pushflow\\" + std::wstring(f);
            return false;
        }
    return true;
#endif
}

struct PushFlowBackend::Impl {
    Status status;
    double inferMs = 0;
    mutable std::mutex telemetryMutex;
    std::array<float, kTelemetry> inferRing{};
    size_t inferCount = 0, inferSamples = 0;
    bool markerArmed = false;   // crash guard file written, not yet cleared
    uint64_t fullPacks = 0, timePacks = 0;
    // The pair the last full pack wrote (PushFlow 2's encoder cache reads them in the next inference).
    uint64_t packedPrevious = 0, packedCurrent = 0;
    bool packedFull = false;
    std::atomic<uint64_t> encoderRuns{0}, featureHits{0}, featureMisses{0};
    // PushFlow 2's Guard (shaders/PushGuard.hlsl): one pass in four stages after the picture is made, its
    // two small buffers, the guarded picture, and the frame's totals read back a few frames later.
    pipeline::ComputePass guardPass;
    ComPtr<ID3D12Resource> guardTiles, guardCounts, guarded, guardReadback;
    uint32_t guardTilesW = 0, guardTilesH = 0;
    UINT guardRing = 0;
    bool guardOn = false;
    float lastTimestep = 0.5f;
    static constexpr UINT kGuardRing = 8;
    uint32_t* guardMapped = nullptr;
    uint64_t guardWritten = 0;
    std::atomic<uint64_t> guardFrames{0}, guardMended{0}, guardHeld{0}, guardTilesReplaced{0};
    // PushFlow 2's Predict (opt-in): one compute pass in two stages (PushPredict.hlsl), the forecast
    // picture, and its totals read back a few frames later.
    pipeline::ComputePass predictPass;
    ComPtr<ID3D12Resource> predictTotals, predicted, predictReadback;
    UINT predictRing = 0;
    bool predictOn = false;
    static constexpr UINT kPredictRing = 8;
    uint32_t* predictMapped = nullptr;
    uint64_t predictWritten = 0;
    std::atomic<uint64_t> predictFrames{0}, predictCounted{0}, predictMoving{0}, predictHeld{0};
    std::chrono::steady_clock::time_point lastSummary = std::chrono::steady_clock::now();
    ComPtr<ID3D12Device> device;
    ComPtr<ID3D12CommandQueue> queue;
    uint32_t width = 0, height = 0;
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
    Encoding encoding = Encoding::Encoded;
    uint32_t channels = 7;
    ModelFacts facts;
    pipeline::ComputePass packPass, unpackPass, rebuildPass;
    pipeline::DescriptorStager stager;
    // Tensors: 0 input, 1 rgb, 2 grid0, 3 grid1, 4 mask (grids/mask only with the rebuild outputs).
    std::array<ComPtr<ID3D12Resource>, 5> buffers;
    ComPtr<ID3D12Resource> picture;
    pipeline::StateTracker states;
    UINT packRing = 0, rebuildRing = 0;
    bool haveGrids = false;
    // The network's span on the queue: a timestamp after the pack, one before the resolve. Read a few
    // inferences later from a persistently mapped readback ring (queued inference has no CPU time
    // that means anything). Telemetry falls back to CPU time when the timestamps are unavailable.
    static constexpr UINT kTimeRing = 8;
    ComPtr<ID3D12QueryHeap> timeQueries;
    ComPtr<ID3D12Resource> timeReadback;
    uint64_t* timeMapped = nullptr;
    double tickMs = 0;
    uint64_t timeWritten = 0;   // timestamp pairs recorded (pack + resolve)
    bool timePending = false;   // pack recorded its stamp; resolve has not yet
    double lastNetworkMs = 0;
#ifdef VEYRA_HAVE_ONNXRUNTIME
    HMODULE ortModule = nullptr, dmlModule = nullptr;
    const OrtApi* api = nullptr;
    const OrtDmlApi* dml = nullptr;
    OrtEnv* env = nullptr;
    OrtSession* session = nullptr;
    OrtMemoryInfo* memory = nullptr;
    OrtIoBinding* binding = nullptr;
    std::array<OrtValue*, 5> values{};
    std::array<void*, 5> allocations{};
    ComPtr<IDMLDevice> dmlDevice;
    std::string inputName;                                    // the model's one input (the split models share it)
    std::vector<std::pair<std::string, int64_t>> fixedDims;   // the free dimensions the main session was fixed to
    // PushFlow 2, Fusion's first step (docs/PUSHFLOW2.md; scripts/pushflow/rife_graph.py split_encoder).
    // The whole model encodes both frames every time it makes a frame. Cut in two, the second frame is
    // encoded once when its pair arrives and its features are kept for the pair after it, where it is the
    // first frame; every further time between the same two frames runs the rest alone. Two feature
    // buffers change places from pair to pair. The whole model's session stays open: it is what the
    // pictures are checked against at start, and what takes over at once if a split run fails.
    struct Split {
        OrtSession* encoder[2]{};        // [0] the input's first frame, [1] its second
        OrtSession* body = nullptr;
        OrtIoBinding* encode[2][2]{};    // [encoder][the feature buffer it writes]
        OrtIoBinding* run[2]{};          // [the buffer that holds the FIRST frame's features]
        ComPtr<ID3D12Resource> buffers[2];
        void* allocations[2]{};
        OrtValue* values[2]{};
        uint64_t holds[2]{};             // the real frame whose features each buffer holds (0: nothing)
        int first = 0;                   // the buffer bound as the first frame's for the pair in work
        bool active = false;
    } split;
    // Queued inference: ONNX Runtime records the network onto the graph's own queue and returns without
    // waiting for the GPU. The resolve list that reads the outputs is submitted after it on the same
    // queue, so the queue's order is the only synchronisation needed. Until 3.1.7 every inference
    // waited on the CPU (Run synchronises the providers, then SynchronizeBoundOutputs): the engine
    // thread stood still for the whole network once per generated frame, and the queue ran dry between
    // the pack, network and resolve lists, gaps that counted as frame generation cost and made the
    // budget skip pairs. VEYRA_PUSHFLOW_SYNC=1 brings the waits back for comparison.
    OrtRunOptions* runOptions = nullptr;
    bool queued = false;
    uint64_t sinceSync = 0;
    // A full wait every N inferences (0: never) in case the provider only releases finished work
    // there. VEYRA_PUSHFLOW_SYNC_EVERY overrides it; PushFlowBench --leak-check measures whether memory
    // grows without any.
    static constexpr uint64_t kSyncEvery = 120;
    uint64_t syncEvery = kSyncEvery;

    bool ok(OrtStatus* s, const char* what) {
        if (!s) return true;
        veyra::log::error("pushflow", std::format("{} failed: {}", what, api->GetErrorMessage(s)));
        api->ReleaseStatus(s);
        return false;
    }
    bool faulted = false;   // ONNX Runtime faulted (caught); PushFlow stays failed and the crash marker stays
    void fault(const char* what) {
        if (!faulted) veyra::log::error("pushflow", std::format("ONNX Runtime faulted in {}; PushFlow is turned off for this session", what));
        faulted = true; status.ready = false;
        g_faults.fetch_add(1, std::memory_order_relaxed);
    }
    // The provider's GPU buffer pool of each open session, held through an OrtAllocator.
    //
    // When a session closes, ONNX Runtime 1.20.1's DirectML provider first lets go of its buffer pool
    // and only then releases the objects still waiting in its queue of finished GPU work
    // (ExecutionProviderImpl::Close: allocator reset, then ExecutionContext::Close). A pooled buffer
    // among them returns itself to a pool that was destroyed a moment earlier. That is the access
    // violation of the 2026-11-17 field dump (Xbox streaming, PushFlow shut down for a settings
    // rebuild): BucketizedBufferAllocator::FreeResource on an allocator whose pool and context were
    // already gone. Whether such a buffer is still queued depends on when the session ends (that it
    // struck a source that rebuilds soon after connecting fits, but is not proven). An allocator held
    // here keeps the pool alive until the session is gone (the provider marks a pool that outlives
    // it as closed, a case it handles), so the late release is an ordinary one.
    // Not a cause, checked 2026-11-18: the provider's execution context is its own per session here.
    // The one it stores on the D3D12 device and shares between sessions is only for ONNX Runtime's
    // Python API (DMLProviderFactory::CreateProvider, python_api_).
    std::vector<std::pair<OrtSession*, OrtAllocator*>> pools;
    bool holdPool(OrtSession* s) {
        if (!s || !memory || unguardedForTest()) return false;
        for (const auto& [session_, pool] : pools) if (session_ == s) return true;
        OrtAllocator* pool = nullptr;
        bool crashed = false;
        OrtStatus* st = sehCreateAllocator(api, s, memory, &pool, &crashed);
        if (st) { veyra::log::warn("pushflow", std::format("provider buffer pool not held: {}", api->GetErrorMessage(st))); api->ReleaseStatus(st); pool = nullptr; }
        if (crashed) { fault("CreateAllocator"); pool = nullptr; }
        if (!pool) return false;
        pools.emplace_back(s, pool);
        return true;
    }
    void releaseSession(OrtSession* s) {
        if (!s) return;
        holdPool(s);
        if (!sehReleaseSession(api, s)) fault("ReleaseSession");
        for (auto it = pools.begin(); it != pools.end();) {
            if (it->first != s) { ++it; continue; }
            if (!sehReleaseAllocator(api, it->second)) fault("ReleaseAllocator");
            it = pools.erase(it);
        }
    }
    void releaseSplit() {
        split.active = false;
        if (!api) return;
        for (auto& row : split.encode) for (auto*& b : row) { if (b && !sehReleaseIoBinding(api, b)) fault("ReleaseIoBinding"); b = nullptr; }
        for (auto*& b : split.run) { if (b && !sehReleaseIoBinding(api, b)) fault("ReleaseIoBinding"); b = nullptr; }
        for (auto*& v : split.values) { if (v && !sehReleaseValue(api, v)) fault("ReleaseValue"); v = nullptr; }
        for (auto*& a : split.allocations) { if (dml && a && !sehFreeAllocation(dml, a)) fault("FreeGPUAllocation"); a = nullptr; }
        for (auto*& s : split.encoder) { releaseSession(s); s = nullptr; }
        releaseSession(split.body); split.body = nullptr;
        for (auto& b : split.buffers) b.Reset();
        split.holds[0] = split.holds[1] = 0;
    }
    void releaseBinding() {
        if (!api) return;
        if (binding && !sehReleaseIoBinding(api, binding)) fault("ReleaseIoBinding");
        binding = nullptr;
        for (auto*& v : values) { if (v && !sehReleaseValue(api, v)) fault("ReleaseValue"); v = nullptr; }
    }
    // Waits until the queue has finished everything submitted so far (teardown: queued inferences
    // may still be reading the buffers about to be freed). Bounded: a removed device completes at once.
    void drainQueue() {
        if (!device || !queue) return;
        ComPtr<ID3D12Fence> fence;
        if (FAILED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence))) || FAILED(queue->Signal(fence.Get(), 1))) return;
        if (fence->GetCompletedValue() >= 1) return;
        HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (!event) return;
        if (SUCCEEDED(fence->SetEventOnCompletion(1, event))) WaitForSingleObject(event, 5000);
        CloseHandle(event);
    }
    void release() {
        if (queued) drainQueue();
        if (api) {
            // The provider waits for its work and releases what it queued while everything it refers to
            // is still there (with queued inference nothing else does that after the last run).
            if (session && binding && !faulted && !unguardedForTest()) {
                bool crashed = false;
                if (OrtStatus* st = sehSync(api, binding, &crashed)) api->ReleaseStatus(st);
                if (crashed) fault("SynchronizeBoundOutputs");
            }
            // The split models' sessions the same way, each before anything of it is released.
            for (OrtIoBinding* b : {split.encode[0][0], split.encode[1][0], split.run[0]}) {
                if (!b || faulted || unguardedForTest()) continue;
                bool crashed = false;
                if (OrtStatus* st = sehSync(api, b, &crashed)) api->ReleaseStatus(st);
                if (crashed) fault("SynchronizeBoundOutputs");
            }
            releaseSplit();
            if (runOptions) { api->ReleaseRunOptions(runOptions); runOptions = nullptr; }
            // Values and binding, then the allocations they wrapped, then the session (Microsoft's
            // samples free GPU allocations before the session that used them goes).
            releaseBinding();
            for (auto*& a : allocations) { if (dml && a && !sehFreeAllocation(dml, a)) fault("FreeGPUAllocation"); a = nullptr; }
            releaseSession(session);
            if (memory && !sehReleaseMemoryInfo(api, memory)) fault("ReleaseMemoryInfo");
            if (env && !sehReleaseEnv(api, env)) fault("ReleaseEnv");
        }
        session = nullptr; memory = nullptr; env = nullptr; dml = nullptr; api = nullptr;
        dmlDevice.Reset();
        // The DLLs stay loaded for the process: ONNX Runtime keeps threads and state that do not unload cleanly.
    }
    // Opens a session for `model` and checks its contract: one [1, 7|11, H, W] float input; output 0 RGB,
    // optionally pushflow_grid0 / pushflow_grid1 / pushflow_mask.
    // One session with the DirectML provider; `overrides` fixes named free dimensions of the input.
    bool createSession(const fs::path& model, ID3D12CommandQueue* q, OrtSession** out, const std::vector<std::pair<std::string, int64_t>>& overrides) {
        OrtSessionOptions* options = nullptr;
        if (!ok(api->CreateSessionOptions(&options), "CreateSessionOptions")) return false;
        bool good = ok(api->SetSessionGraphOptimizationLevel(options, ORT_ENABLE_ALL), "optimization level") &&
                    ok(api->DisableMemPattern(options), "DisableMemPattern") &&
                    ok(api->SetSessionExecutionMode(options, ORT_SEQUENTIAL), "execution mode") &&
                    // The network runs on the GPU; the few CPU nodes are tiny. One thread that sleeps when
                    // idle keeps ONNX Runtime's spinning thread pool off the cores the capture, engine and
                    // presentation threads need.
                    ok(api->SetIntraOpNumThreads(options, 1), "intra-op threads") &&
                    ok(api->AddSessionConfigEntry(options, "session.intra_op.allow_spinning", "0"), "no spinning");
        for (const auto& [name, value] : overrides)
            if (good) good = ok(api->AddFreeDimensionOverrideByName(options, name.c_str(), value), "free dimension override");
        if (good) good = ok(dml->SessionOptionsAppendExecutionProvider_DML1(options, dmlDevice.Get(), q), "DirectML provider");
        if (good) {
            bool crashed = false;
            good = ok(sehCreateSession(api, env, model.c_str(), options, out, &crashed), "CreateSession") && !crashed;
            if (crashed) fault("CreateSession");
            const bool held = good && holdPool(*out);
            if (good) {
                // False with a runtime other than the pinned one would mean it no longer hands out its
                // pool this way: PushFlow still runs, as it did before 3.2.2.
                status.poolHeld = held;
                if (!held && !unguardedForTest())
                    veyra::log::warn("pushflow", "the provider's buffer pool is not held (the ONNX Runtime in runtime\\pushflow is not the pinned 1.20.1?)");
            }
        }
        api->ReleaseSessionOptions(options);
        return good;
    }
    // Opens a session for `model` and checks its contract: one [1, 7|11, H, W] float input; output 0 RGB,
    // optionally pushflow_grid0 / pushflow_grid1 / pushflow_mask.
    //
    // Static shapes (2026-11-18): the vs-mlrt RIFE models have a free height and width. Left free, the
    // DirectML provider cannot fold the shape arithmetic (the model pads itself to its block size),
    // splits the graph into DirectML and CPU pieces and syncs between them on every run, and does not
    // pre-process the weights: Microsoft's DirectML provider documentation says it "works most
    // efficiently when tensor shapes are known at the time a session is created". PushFlow always runs
    // one size per session, so the session is reopened with the free dimensions fixed
    // (AddFreeDimensionOverrideByName) to the padded network size. VEYRA_PUSHFLOW_DYNAMIC=1 keeps the
    // old dynamic session, for comparisons (PushFlowBench --dynamic).
    bool open(const fs::path& model, ID3D12CommandQueue* q, OrtSession** out, uint32_t& inChannels, bool& grids) {
        if (!createSession(model, q, out, {})) return false;
        for (int pass = 0; pass < 2; ++pass) {
            OrtAllocator* allocator = nullptr;
            if (!ok(api->GetAllocatorWithDefaultOptions(&allocator), "allocator")) return false;
            size_t inputs = 0, outputs = 0;
            api->SessionGetInputCount(*out, &inputs); api->SessionGetOutputCount(*out, &outputs);
            if (inputs != 1 || outputs < 1) { veyra::log::error("pushflow", std::format("model has {} inputs and {} outputs", inputs, outputs)); return false; }
            OrtTypeInfo* typeInfo = nullptr;
            if (!ok(api->SessionGetInputTypeInfo(*out, 0, &typeInfo), "input type")) return false;
            const OrtTensorTypeAndShapeInfo* tensorInfo = nullptr;
            api->CastTypeInfoToTensorInfo(typeInfo, &tensorInfo);
            ONNXTensorElementDataType element = ONNX_TENSOR_ELEMENT_DATA_TYPE_UNDEFINED;
            size_t dims = 0;
            int64_t shape[4]{};
            const char* names[4]{};
            if (tensorInfo) {
                api->GetTensorElementType(tensorInfo, &element); api->GetDimensionsCount(tensorInfo, &dims);
                if (dims == 4) { api->GetDimensions(tensorInfo, shape, 4); ok(api->GetSymbolicDimensions(tensorInfo, names, 4), "symbolic dimensions"); }
            }
            std::string symbolic[4];
            for (int i = 0; i < 4; ++i) symbolic[i] = names[i] ? names[i] : "";
            api->ReleaseTypeInfo(typeInfo);
            if (dims != 4 || (shape[1] != 7 && shape[1] != 11) || element != ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT) {
                veyra::log::error("pushflow", std::format("model input not [N,7|11,H,W] float (dims={} channels={} type={})", dims, dims == 4 ? shape[1] : 0, int(element)));
                return false;
            }
            if ((shape[2] > 0 && shape[2] != int64_t(status.padH)) || (shape[3] > 0 && shape[3] != int64_t(status.padW))) {
                veyra::log::error("pushflow", std::format("model has a fixed size {}x{}; needed {}x{}", shape[3], shape[2], status.padW, status.padH));
                return false;
            }
            inChannels = uint32_t(shape[1]);
            int found = 0;
            for (size_t i = 1; i < outputs; ++i) {
                char* name = nullptr;
                if (!ok(api->SessionGetOutputName(*out, i, allocator, &name), "output name")) return false;
                const std::string n = name; api->AllocatorFree(allocator, name);
                if (n == "pushflow_grid0" || n == "pushflow_grid1" || n == "pushflow_mask") ++found;
            }
            grids = found == 3;
            if (pass == 1) {
                status.staticShapes = shape[0] > 0 && shape[2] > 0 && shape[3] > 0;
                veyra::log::info("pushflow", std::format("session shapes {}: input {}x{}x{}x{}", status.staticShapes ? "static" : "still dynamic", shape[0], shape[1], shape[2], shape[3]));
                return true;
            }
            // First pass: fix the free dimensions and reopen.
            std::vector<std::pair<std::string, int64_t>> overrides;
            const int64_t wanted[4] = {1, shape[1], int64_t(status.padH), int64_t(status.padW)};
            bool clash = false;
            for (int i = 0; i < 4; ++i) {
                if (shape[i] > 0) continue;
                if (symbolic[i].empty()) { veyra::log::warn("pushflow", std::format("input dimension {} is free and unnamed; it stays dynamic", i)); continue; }
                bool seen = false;
                for (auto& [n, v] : overrides) if (n == symbolic[i]) { seen = true; clash = clash || v != wanted[i]; }
                if (!seen) overrides.emplace_back(symbolic[i], wanted[i]);
            }
            const bool dynamic = GetEnvironmentVariableW(L"VEYRA_PUSHFLOW_DYNAMIC", nullptr, 0) > 0;
            if (dynamic || clash || overrides.empty()) {
                status.staticShapes = !dynamic && !clash && shape[0] > 0 && shape[2] > 0 && shape[3] > 0;
                veyra::log::info("pushflow", std::format("session shapes {} ({})", status.staticShapes ? "static" : "dynamic",
                    dynamic ? "VEYRA_PUSHFLOW_DYNAMIC" : clash ? "height and width share one name" : overrides.empty() ? "nothing to fix" : ""));
                return true;
            }
            std::string list;
            for (auto& [n, v] : overrides) list += std::format("{}{}={}", list.empty() ? "" : ", ", n, v);
            veyra::log::info("pushflow", "fixing free dimensions: " + list);
            fixedDims = overrides;
            releaseSession(*out); *out = nullptr;
            if (!createSession(model, q, out, overrides)) return false;
        }
        return false;
    }
    // Binds the preallocated D3D12 buffers to `s`: every output, so ONNX Runtime never allocates one itself.
    bool bind(OrtSession* s) {
        releaseBinding();
        const int64_t h = status.padH, w = status.padW;
        const int64_t inShape[4]{1, int64_t(channels), h, w}, rgbShape[4]{1, 3, h, w}, gridShape[4]{1, h, w, 2}, maskShape[4]{1, 1, h, w};
        const int64_t* shapes[5] = {inShape, rgbShape, gridShape, gridShape, maskShape};
        const size_t counts[5] = {size_t(channels * h * w), size_t(3 * h * w), size_t(2 * h * w), size_t(2 * h * w), size_t(h * w)};
        const int tensors = haveGrids ? 5 : 2;
        for (int i = 0; i < tensors; ++i)
            if (!ok(api->CreateTensorWithDataAsOrtValue(memory, allocations[i], counts[i] * 4, shapes[i], 4, ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT, &values[i]), "tensor"))
                return false;
        if (!ok(api->CreateIoBinding(s, &binding), "binding")) return false;
        OrtAllocator* allocator = nullptr;
        if (!ok(api->GetAllocatorWithDefaultOptions(&allocator), "allocator")) return false;
        char* name = nullptr;
        if (!ok(api->SessionGetInputName(s, 0, allocator, &name), "input name")) return false;
        inputName = name;
        const bool inOk = ok(api->BindInput(binding, name, values[0]), "bind input");
        api->AllocatorFree(allocator, name);
        if (!inOk) return false;
        size_t outputs = 0;
        api->SessionGetOutputCount(s, &outputs);
        for (size_t i = 0; i < outputs; ++i) {
            if (!ok(api->SessionGetOutputName(s, i, allocator, &name), "output name")) return false;
            const std::string n = name;
            int slot = i == 0 ? 1 : n == "pushflow_grid0" ? 2 : n == "pushflow_grid1" ? 3 : n == "pushflow_mask" ? 4 : -1;
            if (slot >= tensors) slot = -1;
            const bool good = slot < 0 || ok(api->BindOutput(binding, name, values[slot]), "bind output");
            api->AllocatorFree(allocator, name);
            if (!good) return false;
            if (slot < 0) { veyra::log::error("pushflow", "model output PushFlow does not bind: " + n); return false; }
        }
        return true;
    }
    // One command list on the queue, waited for (start-up only: the self-check's upload and readback).
    bool oneShot(const std::function<void(ID3D12GraphicsCommandList*)>& record) {
        ComPtr<ID3D12CommandAllocator> allocator;
        ComPtr<ID3D12GraphicsCommandList> list;
        ComPtr<ID3D12Fence> fence;
        if (FAILED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator))) ||
            FAILED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr, IID_PPV_ARGS(&list))) ||
            FAILED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)))) return false;
        record(list.Get());
        if (FAILED(list->Close())) return false;
        ID3D12CommandList* lists[] = {list.Get()};
        queue->ExecuteCommandLists(1, lists);
        if (FAILED(queue->Signal(fence.Get(), 1))) return false;
        HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (!event) return false;
        fence->SetEventOnCompletion(1, event);
        WaitForSingleObject(event, 30000);
        CloseHandle(event);
        return fence->GetCompletedValue() >= 1;
    }
    bool uploadInput(const std::vector<float>& data) {
        auto upload = pipeline::makeUploadBuffer(device.Get(), data.size() * 4);
        if (!upload) return false;
        void* mapped = nullptr;
        if (FAILED(upload->Map(0, nullptr, &mapped))) return false;
        std::memcpy(mapped, data.data(), data.size() * 4);
        upload->Unmap(0, nullptr);
        return oneShot([&](ID3D12GraphicsCommandList* list) {
            D3D12_RESOURCE_BARRIER b{}; b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION; b.Transition.pResource = buffers[0].Get();
            b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            b.Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS; b.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
            list->ResourceBarrier(1, &b);
            list->CopyBufferRegion(buffers[0].Get(), 0, upload.Get(), 0, data.size() * 4);
            std::swap(b.Transition.StateBefore, b.Transition.StateAfter);
            list->ResourceBarrier(1, &b);
        });
    }
    std::vector<float> readRgb() {
        const uint64_t bytes = uint64_t(status.padW) * status.padH * 3 * 4;
        D3D12_HEAP_PROPERTIES hp{}; hp.Type = D3D12_HEAP_TYPE_READBACK;
        D3D12_RESOURCE_DESC bd{}; bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER; bd.Width = bytes; bd.Height = 1;
        bd.DepthOrArraySize = 1; bd.MipLevels = 1; bd.SampleDesc.Count = 1; bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        ComPtr<ID3D12Resource> rb;
        if (FAILED(device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&rb)))) return {};
        const bool copied = oneShot([&](ID3D12GraphicsCommandList* list) {
            D3D12_RESOURCE_BARRIER b{}; b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION; b.Transition.pResource = buffers[1].Get();
            b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            b.Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS; b.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
            list->ResourceBarrier(1, &b);
            list->CopyBufferRegion(rb.Get(), 0, buffers[1].Get(), 0, bytes);
            std::swap(b.Transition.StateBefore, b.Transition.StateAfter);
            list->ResourceBarrier(1, &b);
        });
        if (!copied) return {};
        float* mapped = nullptr;
        if (FAILED(rb->Map(0, nullptr, reinterpret_cast<void**>(&mapped)))) return {};
        std::vector<float> out(mapped, mapped + bytes / 4);
        const D3D12_RANGE none{0, 0}; rb->Unmap(0, &none);
        return out;
    }
    // --- PushFlow 2: the encoder cache (see Split above) ---------------------------------------------
    bool makeTensorBuffer(uint64_t bytes, ComPtr<ID3D12Resource>& out) {
        D3D12_HEAP_PROPERTIES hp{}; hp.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC bd{}; bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER; bd.Width = (bytes + 255) / 256 * 256; bd.Height = 1;
        bd.DepthOrArraySize = 1; bd.MipLevels = 1; bd.SampleDesc.Count = 1; bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        bd.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        return SUCCEEDED(device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr, IID_PPV_ARGS(&out)));
    }
    // One binding of a split session: the model's own input; for the rest (feat0 given) the two frames'
    // features in and the model's own outputs into the same buffers the whole model writes; for an
    // encoder (featOut given) its features out.
    OrtIoBinding* makeSplitBinding(OrtSession* s, OrtValue* feat0, OrtValue* feat1, OrtValue* featOut) {
        OrtIoBinding* b = nullptr;
        if (!ok(api->CreateIoBinding(s, &b), "split binding")) return nullptr;
        bool good = ok(api->BindInput(b, inputName.c_str(), values[0]), "bind input");
        if (good && feat0 && feat1) good = ok(api->BindInput(b, "pushflow_feat0", feat0), "bind first features") && ok(api->BindInput(b, "pushflow_feat1", feat1), "bind second features");
        if (good && featOut) good = ok(api->BindOutput(b, "pushflow_feat", featOut), "bind features out");
        if (good && !featOut) {
            OrtAllocator* allocator = nullptr;
            size_t outputs = 0;
            good = ok(api->GetAllocatorWithDefaultOptions(&allocator), "allocator");
            if (good) api->SessionGetOutputCount(s, &outputs);
            const int tensors = haveGrids ? 5 : 2;
            int bound = 0;
            for (size_t i = 0; good && i < outputs; ++i) {
                char* name = nullptr;
                if (!ok(api->SessionGetOutputName(s, i, allocator, &name), "output name")) { good = false; break; }
                const std::string n = name;
                api->AllocatorFree(allocator, name);
                const int slot = i == 0 ? 1 : n == "pushflow_grid0" ? 2 : n == "pushflow_grid1" ? 3 : n == "pushflow_mask" ? 4 : -1;
                good = slot > 0 && slot < tensors && ok(api->BindOutput(b, n.c_str(), values[slot]), "bind output");
                bound += good ? 1 : 0;
            }
            good = good && bound == tensors - 1;   // every buffer the whole model fills, and nothing else
        }
        if (!good) { if (!sehReleaseIoBinding(api, b)) fault("ReleaseIoBinding"); return nullptr; }
        return b;
    }
    bool openSplit(const fs::path& dir, uint32_t tier, ID3D12CommandQueue* q) {
        auto overrides = fixedDims;
        overrides.emplace_back("pushflow_fh", int64_t(status.padH));
        overrides.emplace_back("pushflow_fw", int64_t(status.padW));
        const fs::path models = dir / L"models";
        const bool held = status.poolHeld;   // createSession reports the last session's; keep the main session's word
        const bool opened = createSession(models / splitFile(tier, 0), q, &split.encoder[0], overrides) &&
                            createSession(models / splitFile(tier, 1), q, &split.encoder[1], overrides) &&
                            createSession(models / splitFile(tier, 2), q, &split.body, overrides);
        status.poolHeld = held && status.poolHeld;
        if (!opened) return false;
        size_t inputs[3]{}, outputs[3]{};
        OrtSession* const sessions[3] = {split.encoder[0], split.encoder[1], split.body};
        for (int i = 0; i < 3; ++i) { api->SessionGetInputCount(sessions[i], &inputs[i]); api->SessionGetOutputCount(sessions[i], &outputs[i]); }
        if (inputs[0] != 1 || inputs[1] != 1 || outputs[0] != 1 || outputs[1] != 1 || inputs[2] != 3) {
            veyra::log::error("pushflow", std::format("split models are not what PushFlow expects (inputs {} {} {}, outputs {} {} {})", inputs[0], inputs[1], inputs[2], outputs[0], outputs[1], outputs[2]));
            return false;
        }
        const uint64_t count = uint64_t(facts.splitFeatures) * status.padW * status.padH;
        const int64_t shape[4]{1, int64_t(facts.splitFeatures), int64_t(status.padH), int64_t(status.padW)};
        for (int k = 0; k < 2; ++k) {
            if (!makeTensorBuffer(count * 4, split.buffers[k])) { veyra::log::error("pushflow", "feature buffers"); return false; }
            if (!ok(dml->CreateGPUAllocationFromD3DResource(split.buffers[k].Get(), &split.allocations[k]), "feature allocation")) return false;
            if (!ok(api->CreateTensorWithDataAsOrtValue(memory, split.allocations[k], size_t(count * 4), shape, 4, ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT, &split.values[k]), "feature tensor")) return false;
        }
        for (int e = 0; e < 2; ++e)
            for (int k = 0; k < 2; ++k) {
                split.encode[e][k] = makeSplitBinding(split.encoder[e], nullptr, nullptr, split.values[k]);
                if (!split.encode[e][k]) return false;
            }
        for (int k = 0; k < 2; ++k) {
            split.run[k] = makeSplitBinding(split.body, split.values[k], split.values[1 - k], nullptr);
            if (!split.run[k]) return false;
        }
        return true;
    }
    // One generated frame through the split models. A new pair first gets its features: the first
    // frame's are the ones the pair before left behind (the same real frame), else the first-frame
    // encoder makes them. A buffer is marked empty while it is being written, so a failed run can
    // never leave features under the wrong frame's number.
    bool runSplit(bool async, bool& crashed) {
        const OrtRunOptions* options = async ? runOptions : nullptr;
        if (packedFull) {
            int k = packedPrevious && split.holds[0] == packedPrevious ? 0 : packedPrevious && split.holds[1] == packedPrevious ? 1 : -1;
            if (k < 0) {
                k = 0;
                split.holds[0] = split.holds[1] = 0;
                if (!ok(sehRun(api, split.encoder[0], options, split.encode[0][0], &crashed), "encoder (first frame)") || crashed) return false;
                split.holds[0] = packedPrevious;
                featureMisses.fetch_add(1, std::memory_order_relaxed); encoderRuns.fetch_add(1, std::memory_order_relaxed);
            } else {
                featureHits.fetch_add(1, std::memory_order_relaxed);
            }
            split.holds[1 - k] = 0;
            if (!ok(sehRun(api, split.encoder[1], options, split.encode[1][1 - k], &crashed), "encoder") || crashed) return false;
            split.holds[1 - k] = packedCurrent;
            encoderRuns.fetch_add(1, std::memory_order_relaxed);
            split.first = k;
            packedFull = false;
        }
        if (!ok(sehRun(api, split.body, options, split.run[split.first], &crashed), "the rest of the model") || crashed) return false;
        if (!async)
            for (OrtIoBinding* b : {split.encode[0][0], split.encode[1][0], split.run[split.first]})
                if (!ok(sehSync(api, b, &crashed), "SynchronizeBoundOutputs") || crashed) return false;
        return true;
    }
    // DirectML records the network into its own command lists and may hold them back; synchronizing the
    // bound outputs submits them to the queue and waits, so the next list reads the finished output.
    // wait=true: the call returns only after the GPU finished the network (start-up checks).
    bool run(bool wait = false) {
        if (faulted) return false;
        bool crashed = false;
        const bool async = queued && !wait && (syncEvery == 0 || ++sinceSync < syncEvery);
        if (!async) sinceSync = 0;
        if (split.active) {
            if (runSplit(async, crashed)) return true;
            if (crashed) { fault("the split models"); return false; }
            // A split run failed without a fault. The packed input is untouched, so the whole model makes
            // this frame and every later one.
            split.active = false; status.split = false; status.splitState = "failed";
            veyra::log::error("pushflow", "the encoder cache failed; PushFlow goes on with the whole model");
        }
        const bool done = ok(sehRun(api, session, async ? runOptions : nullptr, binding, &crashed), "RunWithBinding") && !crashed &&
                          (async || (ok(sehSync(api, binding, &crashed), "SynchronizeBoundOutputs") && !crashed));
        if (crashed) fault("RunWithBinding");
        return done;
    }
#endif
    ~Impl() {
#ifdef VEYRA_HAVE_ONNXRUNTIME
        release();
#endif
    }
};

std::wstring crashMarkerPath() { return (veyra::runtime::localDataDirectory() / L"pushflow-starting.txt").wstring(); }
bool crashedLastTime() { std::error_code ec; return fs::exists(crashMarkerPath(), ec); }
void clearCrashMarker() { std::error_code ec; fs::remove(crashMarkerPath(), ec); }
namespace {
// Written and flushed before ONNX Runtime runs, so it survives a crash inside it.
void armCrashMarker() {
    std::error_code ec;
    fs::create_directories(fs::path(crashMarkerPath()).parent_path(), ec);
    std::ofstream out(fs::path(crashMarkerPath()), std::ios::trunc);
    out << "PushFlow was starting; if this file is still here at the next start, PushFPS stopped inside PushFlow.\n";
    out.flush();
}
constexpr size_t kSteadyInferences = 120;   // about 2 s at 2X from 30 FPS
}
PushFlowBackend::PushFlowBackend() : p_(std::make_unique<Impl>()) {}
PushFlowBackend::~PushFlowBackend() {
    const bool armed = p_ && p_->markerArmed;
    bool keepMarker = false;
#ifdef VEYRA_HAVE_ONNXRUNTIME
    // Release ONNX Runtime first (guarded): a fault there keeps the marker, so the next start
    // switches away from PushFlow.
    if (p_) { p_->release(); keepMarker = p_->faulted; }
#endif
    p_.reset();
    if (armed && !keepMarker) clearCrashMarker();
}
double PushFlowBackend::lastInferCpuMs() const { return p_->inferMs; }
ID3D12Resource* PushFlowBackend::tensorBuffer(bool input) const { return p_->buffers[input ? 0 : 1].Get(); }
ID3D12Resource* PushFlowBackend::motionBuffer(uint32_t index) const { return p_->haveGrids && index < 3 ? p_->buffers[2 + index].Get() : nullptr; }
uint32_t PushFlowBackend::inputChannels() const { return p_->channels; }
const Status& PushFlowBackend::status() const { return p_->status; }
void PushFlowBackend::shutdown() { p_ = std::make_unique<Impl>(); }

Telemetry PushFlowBackend::telemetry() const {
    const auto& p = *p_;
    std::lock_guard lock(p.telemetryMutex);
    Telemetry t;
    t.inferences = p.inferCount; t.fullPacks = p.fullPacks; t.timePacks = p.timePacks;
    t.encoderRuns = p.encoderRuns.load(std::memory_order_relaxed); t.featureHits = p.featureHits.load(std::memory_order_relaxed);
    t.featureMisses = p.featureMisses.load(std::memory_order_relaxed);
    t.guardFrames = p.guardFrames.load(std::memory_order_relaxed); t.guardMended = p.guardMended.load(std::memory_order_relaxed);
    t.guardHeld = p.guardHeld.load(std::memory_order_relaxed); t.guardTiles = p.guardTilesReplaced.load(std::memory_order_relaxed);
    t.predictFrames = p.predictFrames.load(std::memory_order_relaxed); t.predictCounted = p.predictCounted.load(std::memory_order_relaxed);
    t.predictMoving = p.predictMoving.load(std::memory_order_relaxed); t.predictHeld = p.predictHeld.load(std::memory_order_relaxed);
    const size_t n = std::min(p.inferSamples, kTelemetry);
    if (!n) return t;
    std::vector<float> v(p.inferRing.begin(), p.inferRing.begin() + ptrdiff_t(n));
    std::sort(v.begin(), v.end());
    t.inferP50Ms = v[n / 2];
    t.inferP95Ms = v[std::min(n - 1, n * 95 / 100)];
    t.inferMaxMs = v.back();
    return t;
}

bool PushFlowBackend::initialize(ID3D12Device* device, ID3D12CommandQueue* queue, const std::wstring& runtimeDir, uint32_t width, uint32_t height,
                                 DXGI_FORMAT format, Encoding encoding, uint32_t tier, int motionHeight, bool encoderCache, bool guard, bool predict) {
    p_ = std::make_unique<Impl>();
    auto& p = *p_;
    std::wstring why;
    if (!device || !queue || width < 16 || height < 16 || !runtimeInstalled(runtimeDir, &why)) {
        veyra::log::error("pushflow", "runtime missing or bad arguments: " + narrow(why));
        return false;
    }
#ifndef VEYRA_HAVE_ONNXRUNTIME
    (void)format; (void)encoding; (void)tier; (void)motionHeight; (void)encoderCache; (void)guard; (void)predict;
    return false;
#else
    p.device = device; p.queue = queue; p.width = width; p.height = height; p.format = viewFormat(format); p.encoding = encoding;
    armCrashMarker();
    p.markerArmed = true;
    tier = std::min(tier, kTierCount - 1);
    const double scale = networkScale(tier, width, height, motionHeight);
    p.status.modelW = std::max<uint32_t>(32, uint32_t(std::lround(width * scale)));
    p.status.modelH = std::max<uint32_t>(32, uint32_t(std::lround(height * scale)));
    const fs::path dir = pushFlowDirectory(runtimeDir);
    const fs::path fp32Model = dir / L"models" / modelFile(tier, false), fp16Model = dir / L"models" / modelFile(tier, true);
    p.facts = readFacts(fp32Model);
    const uint32_t block = std::max(p.facts.pad, kBlock);
    p.status.padW = (p.status.modelW + block - 1) / block * block;
    p.status.padH = (p.status.modelH + block - 1) / block * block;
    // DirectML first, from our folder, so ONNX Runtime and we use the same (redistributed) copy.
    p.dmlModule = LoadLibraryExW((dir / L"DirectML.dll").c_str(), nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
    p.ortModule = LoadLibraryExW((dir / L"onnxruntime.dll").c_str(), nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
    if (!p.dmlModule || !p.ortModule) { veyra::log::error("pushflow", std::format("LoadLibrary failed error={}", GetLastError())); return false; }
    using CreateDml = HRESULT(WINAPI*)(ID3D12Device*, DML_CREATE_DEVICE_FLAGS, REFIID, void**);
    using GetBase = const OrtApiBase*(ORT_API_CALL*)();
    auto createDml = reinterpret_cast<CreateDml>(reinterpret_cast<void*>(GetProcAddress(p.dmlModule, "DMLCreateDevice")));
    auto getBase = reinterpret_cast<GetBase>(reinterpret_cast<void*>(GetProcAddress(p.ortModule, "OrtGetApiBase")));
    if (!createDml || !getBase) { veyra::log::error("pushflow", "runtime entry points missing"); return false; }
    const OrtApiBase* base = getBase();
    p.api = base ? base->GetApi(ORT_API_VERSION) : nullptr;
    if (!p.api) { veyra::log::error("pushflow", std::format("ONNX Runtime {} does not offer API {}", base ? base->GetVersionString() : "?", ORT_API_VERSION)); return false; }
    if (FAILED(createDml(device, DML_CREATE_DEVICE_FLAG_NONE, IID_PPV_ARGS(&p.dmlDevice)))) { veyra::log::error("pushflow", "DMLCreateDevice failed"); return false; }
    auto* api = p.api;
    if (!p.ok(api->GetExecutionProviderApi("DML", ORT_API_VERSION, reinterpret_cast<const void**>(&p.dml)), "GetExecutionProviderApi") || !p.dml) return false;
    if (!p.ok(api->CreateEnv(ORT_LOGGING_LEVEL_WARNING, "pushflow", &p.env), "CreateEnv")) return false;
    if (!p.ok(api->CreateMemoryInfo("DML", OrtDeviceAllocator, 0, OrtMemTypeDefault, &p.memory), "memory info")) return false;

    // The full-precision model is the reference and the fallback.
    const auto loadStart = std::chrono::steady_clock::now();
    bool grids = false;
    if (!p.open(fp32Model, queue, &p.session, p.channels, grids)) return false;
    p.haveGrids = grids && p.facts.known;
    p.status.model = narrow(fp32Model.filename().wstring());
    p.status.precision = "fp32";

    // D3D12 buffers for the tensors, always in UNORDERED_ACCESS (as DirectML expects).
    const uint64_t plane = uint64_t(p.status.padW) * p.status.padH;
    const uint64_t floats[5] = {plane * p.channels, plane * 3, plane * 2, plane * 2, plane};
    const auto makeBuffer = [&](uint64_t bytes, ComPtr<ID3D12Resource>& out) {
        D3D12_HEAP_PROPERTIES hp{}; hp.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC bd{}; bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER; bd.Width = (bytes + 255) / 256 * 256; bd.Height = 1;
        bd.DepthOrArraySize = 1; bd.MipLevels = 1; bd.SampleDesc.Count = 1; bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        bd.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        return SUCCEEDED(device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr, IID_PPV_ARGS(&out)));
    };
    const int tensors = p.haveGrids ? 5 : 2;
    for (int i = 0; i < tensors; ++i) {
        if (!makeBuffer(floats[i] * 4, p.buffers[i])) { veyra::log::error("pushflow", "tensor buffers"); return false; }
        if (!p.ok(p.dml->CreateGPUAllocationFromD3DResource(p.buffers[i].Get(), &p.allocations[i]), "allocation")) return false;
    }
    if (!p.bind(p.session)) return false;

    // Half precision: opt-in, then only after it matches the full-precision model on this graphics card.
    char forced[16]{};
    const DWORD forcedLen = GetEnvironmentVariableA("VEYRA_PUSHFLOW_PRECISION", forced, sizeof(forced));
    const std::string force = forcedLen > 0 && forcedLen < sizeof(forced) ? std::string(forced) : std::string();
    std::error_code ec;
    // Half precision only when asked for, since 3.1.7. The automatic self-check ran an fp16 model on
    // every start; on Windows' software renderer that removed the device (PushFlowBench, 2026-11-17:
    // the next tier failed with DMLCreateDevice), and a crash report from an RTX 5090 ended inside ONNX
    // Runtime right after it. fp32 with static shapes is the default. "fp16-checked" is what PushFPS
    // sets once PushFlowBench --fp16-probe passed on this graphics card in its own process
    // (preference pushFlowHalf): the comparison still runs and fp32 stays if it does not match.
    // "fp16" (developers) takes the half model whatever the comparison says.
    const bool wantHalf = force == "fp16" || force == "fp16-checked";
    if (wantHalf && fs::is_regular_file(fp16Model, ec)) {
        const auto input = selfCheckInput(p.status.padW, p.status.padH, p.channels);
        std::vector<float> reference;
        if (p.uploadInput(input) && p.run()) reference = p.readRgb();
        OrtSession* half = nullptr;
        uint32_t halfChannels = 0;
        bool halfGrids = false, accepted = false;
        if (!reference.empty() && p.open(fp16Model, queue, &half, halfChannels, halfGrids) && halfChannels == p.channels && halfGrids == grids) {
            OrtSession* full = p.session;
            p.session = half;
            std::vector<float> got;
            if (p.bind(half) && p.uploadInput(input) && p.run()) got = p.readRgb();
            double se = 0;
            bool finite = got.size() == reference.size();
            const uint32_t w = p.status.modelW, h = p.status.modelH, pw = p.status.padW, ph = p.status.padH;
            for (uint32_t c = 0; finite && c < 3; ++c)
                for (uint32_t y = 0; finite && y < h; ++y)
                    for (uint32_t x = 0; x < w; ++x) {
                        const size_t i = size_t(c) * pw * ph + size_t(y) * pw + x;
                        if (!std::isfinite(got[i])) { finite = false; break; }
                        const double d = std::clamp(double(got[i]), 0.0, 1.0) - std::clamp(double(reference[i]), 0.0, 1.0);
                        se += d * d;
                    }
            const double mse = finite ? se / (3.0 * w * h) : 1.0;
            p.status.selfCheckDb = mse <= 1e-12 ? 99.0 : 10.0 * std::log10(1.0 / mse);
            accepted = finite && (p.status.selfCheckDb >= 38.0 || force == "fp16");
            if (accepted) {
                p.releaseSession(full);
                p.status.model = narrow(fp16Model.filename().wstring());
                p.status.precision = "fp16";
            } else {
                p.releaseBinding();
                p.releaseSession(half);
                p.session = full;
                if (!p.bind(full)) return false;
            }
        } else if (half) {
            p.releaseSession(half);
        }
        veyra::log::info("pushflow", std::format("fp16 self-check {:.1f} dB -> {}", p.status.selfCheckDb,
            accepted ? "fp16" : "fp32 (half precision does not match on this GPU)"));
    }

    // Shaders, descriptors and the output picture.
    std::vector<uint8_t> cs;
    if (!p.packPass.loadShader("PushFlowPack.dxil", cs) || !p.packPass.create(device, cs, 1 + 2 * kSrvRing, 2, 1, 12)) return false;
    if (!p.unpackPass.loadShader("PushFlowUnpack.dxil", cs) || !p.unpackPass.create(device, cs, 3, 1, 2, 8)) return false;
    if (p.haveGrids && (!p.rebuildPass.loadShader("PushFlowReconstruct.dxil", cs) || !p.rebuildPass.create(device, cs, 4 + 2 * kSrvRing, 2, 4, 12))) return false;
    if (!p.stager.initialize(device, 4 * kSrvRing)) return false;
    p.picture = pipeline::makeTexture(device, width, height, p.format, true);
    if (!p.picture) { veyra::log::error("pushflow", "output picture"); return false; }
    const auto bufferUav = [&](ID3D12Resource* r, uint64_t count, D3D12_CPU_DESCRIPTOR_HANDLE at) {
        D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};
        uav.Format = DXGI_FORMAT_UNKNOWN; uav.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
        uav.Buffer.NumElements = UINT(count); uav.Buffer.StructureByteStride = 4;
        device->CreateUnorderedAccessView(r, nullptr, &uav, at);
    };
    bufferUav(p.buffers[0].Get(), floats[0], cpuSlot(p.packPass, 0));
    bufferUav(p.buffers[1].Get(), floats[1], cpuSlot(p.unpackPass, 1));
    pipeline::makeUav(device, p.picture.Get(), p.format, cpuSlot(p.unpackPass, 2));
    if (p.haveGrids) {
        bufferUav(p.buffers[2].Get(), floats[2], cpuSlot(p.rebuildPass, 0));
        bufferUav(p.buffers[3].Get(), floats[3], cpuSlot(p.rebuildPass, 1));
        bufferUav(p.buffers[4].Get(), floats[4], cpuSlot(p.rebuildPass, 2));
        pipeline::makeUav(device, p.picture.Get(), p.format, cpuSlot(p.rebuildPass, 3));
    }
    // PushFlow 2's Guard (opt-in; shaders/PushGuard.hlsl). It judges by the model's grids and mask, in
    // tiles of 16 x 16 motion pixels; whatever is missing here leaves PushFlow as it is without it.
    if (guard) {
        p.guardTilesW = p.status.modelW / 16; p.guardTilesH = p.status.modelH / 16;
        const uint64_t tileCount = uint64_t(p.guardTilesW) * p.guardTilesH;
        if (!p.haveGrids) p.status.guardState = "no-grids";
        else if (p.guardTilesW < 6 || p.guardTilesH < 4) p.status.guardState = "too-small";
        else if (!p.guardPass.loadShader("PushGuard.dxil", cs) || !p.guardPass.create(device, cs, 6 + 3 * kSrvRing, 3, 6, 16) ||
                 !makeBuffer(tileCount * 3 * 4, p.guardTiles) || !makeBuffer((tileCount * 3 + 4) * 4, p.guardCounts) ||
                 !(p.guarded = pipeline::makeTexture(device, width, height, p.format, true))) p.status.guardState = "shader";
        else {
            bufferUav(p.buffers[2].Get(), floats[2], cpuSlot(p.guardPass, 0));
            bufferUav(p.buffers[3].Get(), floats[3], cpuSlot(p.guardPass, 1));
            bufferUav(p.buffers[4].Get(), floats[4], cpuSlot(p.guardPass, 2));
            bufferUav(p.guardTiles.Get(), tileCount * 3, cpuSlot(p.guardPass, 3));
            bufferUav(p.guardCounts.Get(), tileCount * 3 + 4, cpuSlot(p.guardPass, 4));
            pipeline::makeUav(device, p.guarded.Get(), p.format, cpuSlot(p.guardPass, 5));
            // The frame's totals come back through a small ring; without it Guard still works, uncounted.
            p.guardReadback = readbackBuffer(device, Impl::kGuardRing * 4 * sizeof(uint32_t));
            if (p.guardReadback && SUCCEEDED(p.guardReadback->Map(0, nullptr, reinterpret_cast<void**>(&p.guardMapped))))
                std::memset(p.guardMapped, 0, Impl::kGuardRing * 4 * sizeof(uint32_t));
            else { p.guardReadback.Reset(); p.guardMapped = nullptr; }
            p.guardOn = true; p.status.guard = true; p.status.guardState = "on";
        }
        if (!p.status.guard) veyra::log::warn("pushflow", "Guard was asked for and is not used: " + p.status.guardState);
    }
    // PushFlow 2's Predict (opt-in; shaders/PushPredict.hlsl): frames after the newest real frame, from
    // the model's half-way grids. Whatever is missing here leaves PushFlow as it is without it.
    if (predict) {
        if (!p.haveGrids) p.status.predictState = "no-grids";
        else if (!p.predictPass.loadShader("PushPredict.dxil", cs) || !p.predictPass.create(device, cs, 4 + 2 * kSrvRing, 2, 4, 12) ||
                 !makeBuffer(4 * 4, p.predictTotals) || !(p.predicted = pipeline::makeTexture(device, width, height, p.format, true))) p.status.predictState = "shader";
        else {
            bufferUav(p.buffers[2].Get(), floats[2], cpuSlot(p.predictPass, 0));
            bufferUav(p.buffers[3].Get(), floats[3], cpuSlot(p.predictPass, 1));
            bufferUav(p.predictTotals.Get(), 4, cpuSlot(p.predictPass, 2));
            pipeline::makeUav(device, p.predicted.Get(), p.format, cpuSlot(p.predictPass, 3));
            p.predictReadback = readbackBuffer(device, Impl::kPredictRing * 4 * sizeof(uint32_t));
            if (p.predictReadback && SUCCEEDED(p.predictReadback->Map(0, nullptr, reinterpret_cast<void**>(&p.predictMapped))))
                std::memset(p.predictMapped, 0, Impl::kPredictRing * 4 * sizeof(uint32_t));
            else { p.predictReadback.Reset(); p.predictMapped = nullptr; }
            p.predictOn = true; p.status.predict = true; p.status.predictState = "on";
        }
        if (!p.status.predict) veyra::log::warn("pushflow", "Predict was asked for and is not used: " + p.status.predictState);
    }
    // The rebuild is what keeps the Fast tier sharp; at full size the network's own picture is used.
    p.status.rebuild = p.haveGrids && (p.status.modelW != width || p.status.modelH != height);
    p.status.grids = p.haveGrids; p.status.alignCorners = p.facts.alignCorners; p.status.maskForFirst = p.facts.maskForFirst;
    p.status.provider = "DirectML";
    {
        D3D12_QUERY_HEAP_DESC qd{}; qd.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP; qd.Count = 2 * Impl::kTimeRing;
        UINT64 frequency = 0;
        if (SUCCEEDED(queue->GetTimestampFrequency(&frequency)) && frequency && SUCCEEDED(device->CreateQueryHeap(&qd, IID_PPV_ARGS(&p.timeQueries))) &&
            (p.timeReadback = readbackBuffer(device, 2 * Impl::kTimeRing * sizeof(uint64_t))) &&
            SUCCEEDED(p.timeReadback->Map(0, nullptr, reinterpret_cast<void**>(&p.timeMapped)))) {
            p.tickMs = 1000.0 / double(frequency);
            std::memset(p.timeMapped, 0, 2 * Impl::kTimeRing * sizeof(uint64_t));
        } else {
            p.timeQueries.Reset(); p.timeReadback.Reset(); p.timeMapped = nullptr;
        }
    }
    // Warm-up: DirectML prepares the network on its first runs (shader compilation, weight upload).
    // Done here, while the graph is being built, those runs never reach the frame budget. Until 3.1.7
    // the first live inference carried them, and its time made the budget skip every pair until it
    // aged out, after which the next lone evaluation came back slow again (see FgCostSettling).
    {
        const auto input = selfCheckInput(p.status.padW, p.status.padH, p.channels);
        double first = 0, second = 0;
        for (int i = 0; i < 2; ++i) {
            const auto start = std::chrono::steady_clock::now();
            if (!p.uploadInput(input) || !p.run(true)) {
                veyra::log::error("pushflow", std::format("warm-up run failed; device state 0x{:08X}", uint32_t(device->GetDeviceRemovedReason())));
                return false;
            }
            (i == 0 ? first : second) = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
        }
        p.status.warmupMs = second;
        veyra::log::info("pushflow", std::format("warm-up runs {:.1f} ms, then {:.1f} ms", first, second));
    }
    // PushFlow 2, Fusion's first step (opt-in; Impl::Split, docs/PUSHFLOW2.md). Used only after the split
    // models gave the whole model's picture on this graphics card, for a pair that starts with nothing
    // kept and for the pair after it, whose first frame's features are the ones just kept. Whatever goes
    // wrong here leaves PushFlow exactly as it is without the cache, and says why.
    if (encoderCache) {
        std::error_code sec;
        const fs::path models = dir / L"models";
        if (p.status.precision != "fp32") p.status.splitState = "half-precision";
        else if (!p.facts.splitFeatures || !fs::is_regular_file(models / splitFile(tier, 0), sec) || !fs::is_regular_file(models / splitFile(tier, 1), sec) ||
                 !fs::is_regular_file(models / splitFile(tier, 2), sec)) p.status.splitState = "no-files";
        else if (!p.status.staticShapes || p.fixedDims.empty()) p.status.splitState = "dynamic-shapes";
        else if (!p.openSplit(dir, tier, queue)) { p.releaseSplit(); p.status.splitState = p.faulted ? "failed" : "session"; }
        else {
            const size_t planeFloats = size_t(p.status.padW) * p.status.padH;
            const auto forward = selfCheckInput(p.status.padW, p.status.padH, p.channels);
            auto backward = forward;   // the two frames change places: its first frame is the pair before's second
            std::swap_ranges(backward.begin(), backward.begin() + ptrdiff_t(3 * planeFloats), backward.begin() + ptrdiff_t(3 * planeFloats));
            const auto agreement = [&](const std::vector<float>& want, const std::vector<float>& got) {
                if (want.empty() || want.size() != got.size()) return 0.0;
                double se = 0;
                for (uint32_t c = 0; c < 3; ++c)
                    for (uint32_t y = 0; y < p.status.modelH; ++y)
                        for (uint32_t x = 0; x < p.status.modelW; ++x) {
                            const size_t i = size_t(c) * planeFloats + size_t(y) * p.status.padW + x;
                            if (!std::isfinite(got[i])) return 0.0;
                            const double d = std::clamp(double(got[i]), 0.0, 1.0) - std::clamp(double(want[i]), 0.0, 1.0);
                            se += d * d;
                        }
                const double mse = se / (3.0 * p.status.modelW * p.status.modelH);
                return mse <= 1e-12 ? 99.0 : 10.0 * std::log10(1.0 / mse);
            };
            double worst = 99.0;
            bool ran = true, crashed = false;
            for (int pair = 0; ran && pair < 2; ++pair) {
                const auto& input = pair == 0 ? forward : backward;
                std::vector<float> want, got;
                ran = p.uploadInput(input) && p.run(true);   // the whole model: the cache is not in use yet
                if (ran) want = p.readRgb();
                p.packedFull = true; p.packedPrevious = uint64_t(1 + pair); p.packedCurrent = uint64_t(2 + pair);
                ran = ran && p.runSplit(false, crashed) && !crashed;
                if (ran) got = p.readRgb();
                if (ran) worst = std::min(worst, agreement(want, got));
            }
            const uint64_t kept = p.featureHits.load(std::memory_order_relaxed);
            if (crashed) p.fault("the split models");
            p.status.splitCheckDb = ran ? worst : 0.0;
            // The second pair must have used what the first one kept, or the check did not test the cache.
            const bool accepted = ran && !p.faulted && kept == 1 && worst >= 50.0;
            if (accepted) {
                // One frame each way, waited for, the split one with its first frame's features kept: what a
                // steady 2X costs. Start-up numbers on an idle queue; a card test says more.
                const auto timed = [&](bool useSplit) {
                    const auto start = std::chrono::steady_clock::now();
                    bool fault2 = false;
                    const bool done = useSplit ? p.runSplit(false, fault2) && !fault2 : p.run(true);
                    if (fault2) p.fault("the split models");
                    return done ? std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count() : 0.0;
                };
                if (p.uploadInput(forward)) {
                    p.status.wholeMs = timed(false);
                    p.packedFull = true; p.packedPrevious = 3; p.packedCurrent = 4;   // the first frame is the one the check left kept
                    p.status.splitMs = timed(true);
                }
            }
            // Numbers used by the check must never meet a real frame's.
            p.split.holds[0] = p.split.holds[1] = 0;
            p.packedFull = false; p.packedPrevious = p.packedCurrent = 0;
            p.encoderRuns = 0; p.featureHits = 0; p.featureMisses = 0;
            if (accepted && !p.faulted && p.status.splitMs > 0) {
                p.split.active = true; p.status.split = true; p.status.splitState = "on";
            } else {
                p.releaseSplit();
                p.status.splitState = p.faulted ? "failed" : "mismatch";
            }
            veyra::log::info("pushflow", std::format("encoder cache self-check {:.1f} dB, kept features used {} -> {} (whole {:.1f} ms, split {:.1f} ms)",
                p.status.splitCheckDb, kept, p.status.splitState, p.status.wholeMs, p.status.splitMs));
        }
        if (p.faulted) return false;
        if (!p.status.split) veyra::log::warn("pushflow", "the encoder cache was asked for and is not used: " + p.status.splitState);
    }
    // Queued inference (see Impl::runOptions); a failure here only keeps the waits.
    if (GetEnvironmentVariableW(L"VEYRA_PUSHFLOW_SYNC", nullptr, 0) == 0) {
        if (p.ok(p.api->CreateRunOptions(&p.runOptions), "CreateRunOptions") &&
            p.ok(p.api->AddRunConfigEntry(p.runOptions, "disable_synchronize_execution_providers", "1"), "run option")) p.queued = true;
    }
    {
        wchar_t every[16]{};
        const DWORD n = GetEnvironmentVariableW(L"VEYRA_PUSHFLOW_SYNC_EVERY", every, 16);
        if (n > 0 && n < 16) p.syncEvery = uint64_t(std::clamp(_wtoi(every), 0, 100000));
    }
    p.status.queued = p.queued;
    veyra::log::info("pushflow", p.queued ? std::format("inference queued on the graph's queue (no CPU wait per frame; a full wait every {} inferences)", p.syncEvery)
                                          : std::string("inference waits for the GPU each frame"));
    p.status.ready = true;
    veyra::log::info("pushflow", std::format("ready runtime={} model={} precision={} tier={} size={}x{} model={}x{} padded={}x{} channels={} "
        "rebuild={} grids={} encoding={} encoderCache={} loadMs={:.0f}", base->GetVersionString(), p.status.model, p.status.precision, tier, width, height,
        p.status.modelW, p.status.modelH, p.status.padW, p.status.padH, p.channels, p.status.rebuild ? 1 : 0, p.haveGrids ? 1 : 0,
        unsigned(encoding), p.status.splitState.empty() ? std::string("not asked for") : p.status.splitState,
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - loadStart).count()));
    return true;
#endif
}

bool PushFlowBackend::pack(ID3D12GraphicsCommandList* list, ID3D12Resource* previous, ID3D12Resource* current, float timestep, bool timeOnly,
                           uint64_t previousId, uint64_t currentId) {
    auto& p = *p_;
    if (!p.status.ready || !list || !previous || !current) return false;
    UINT srvFirst;
    p.lastTimestep = std::clamp(timestep, 0.0f, 1.0f);   // Guard: which real frame is the nearer one
    if (!timeOnly) {
        // A new pair: the next inference takes its features anew (the encoder cache; nothing else reads these).
        p.packedFull = true; p.packedPrevious = previousId; p.packedCurrent = currentId;
        srvFirst = 1 + 2 * p.packRing;
        p.packRing = (p.packRing + 1) % kSrvRing;
        D3D12_SHADER_RESOURCE_VIEW_DESC view{};
        view.Format = p.format; view.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        view.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING; view.Texture2D.MipLevels = 1;
        p.stager.stageSrv(previous, &view, p.packPass.heap.Get(), srvFirst);
        p.stager.stageSrv(current, &view, p.packPass.heap.Get(), srvFirst + 1);
    } else {
        // Time-only packs bind the last full pack's descriptors (the shader does not read them).
        srvFirst = 1 + 2 * ((p.packRing + kSrvRing - 1) % kSrvRing);
    }
    const float constants[12] = {bits(p.width), bits(p.height), bits(p.status.padW), bits(p.status.padH),
                                 bits(p.status.modelW), bits(p.status.modelH), bits(uint32_t(p.encoding)), bits(p.channels),
                                 std::clamp(timestep, 0.0f, 1.0f), bits(timeOnly ? 1u : 0u), 0.0f, 0.0f};
    p.packPass.bind(list, constants, gpuSlot(p.packPass, srvFirst), gpuSlot(p.packPass, 0));
    list->Dispatch((p.status.padW + 15) / 16, (p.status.padH + 15) / 16, 1);
    D3D12_RESOURCE_BARRIER barrier{}; barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV; barrier.UAV.pResource = p.buffers[0].Get();
    list->ResourceBarrier(1, &barrier);
    if (p.timeQueries) { list->EndQuery(p.timeQueries.Get(), D3D12_QUERY_TYPE_TIMESTAMP, UINT(2 * (p.timeWritten % Impl::kTimeRing))); p.timePending = true; }
    std::lock_guard lock(p.telemetryMutex);
    ++(timeOnly ? p.timePacks : p.fullPacks);
    return true;
}

bool PushFlowBackend::infer() {
    auto& p = *p_;
    if (!p.status.ready) return false;
#ifdef VEYRA_HAVE_ONNXRUNTIME
    const auto start = std::chrono::steady_clock::now();
    const bool done = p.run();
    p.inferMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    // Earlier pairs the GPU has written back (a slot not yet written reads 0 and waits for a later
    // call); each is cleared once counted. The newest finished one is this call's sample.
    double network = -1;
    if (p.timeMapped) {
        for (uint64_t back = std::min<uint64_t>(p.timeWritten, Impl::kTimeRing - 1); back >= 1; --back) {
            uint64_t* pair = p.timeMapped + 2 * ((p.timeWritten - back) % Impl::kTimeRing);
            const uint64_t a = pair[0], b = pair[1];
            if (!a || b <= a) continue;
            const double ms = double(b - a) * p.tickMs;
            pair[0] = pair[1] = 0;
            if (ms < 2000) network = ms;
        }
    }
    if (network >= 0) p.lastNetworkMs = network;
    // Guard's totals of earlier frames (a slot the GPU has not written yet reads 0 and waits; each is
    // cleared once counted): risky tiles, whether the frame was held whole, tiles in all, a serial.
    if (p.guardMapped) {
        for (UINT slot = 0; slot < Impl::kGuardRing; ++slot) {
            uint32_t* totals = p.guardMapped + 4 * slot;
            if (!totals[3] || !totals[2]) continue;
            const uint32_t risky = totals[0], whole = totals[1];
            totals[0] = totals[1] = totals[2] = totals[3] = 0;
            p.guardFrames.fetch_add(1, std::memory_order_relaxed);
            if (whole) p.guardHeld.fetch_add(1, std::memory_order_relaxed);
            else if (risky) p.guardMended.fetch_add(1, std::memory_order_relaxed);
            p.guardTilesReplaced.fetch_add(risky, std::memory_order_relaxed);
        }
    }
    // Predict's totals of earlier forecast frames, the same way: counted, would move, held, a serial.
    if (p.predictMapped) {
        for (UINT slot = 0; slot < Impl::kPredictRing; ++slot) {
            uint32_t* totals = p.predictMapped + 4 * slot;
            if (!totals[3] || !totals[0]) continue;
            const uint32_t counted = totals[0], moving = totals[1], held = totals[2];
            totals[0] = totals[1] = totals[2] = totals[3] = 0;
            p.predictFrames.fetch_add(1, std::memory_order_relaxed);
            p.predictCounted.fetch_add(counted, std::memory_order_relaxed);
            p.predictMoving.fetch_add(moving, std::memory_order_relaxed);
            p.predictHeld.fetch_add(held, std::memory_order_relaxed);
        }
    }
    bool summarize = false;
    {
        std::lock_guard lock(p.telemetryMutex);
        // GPU span when measured; CPU time only without timestamps (queued runs return at once).
        const bool useGpu = p.timeMapped != nullptr;
        // (The very first call has no finished pair yet: its CPU time stands in until one arrives.)
        if (!useGpu || network >= 0 || p.inferSamples == 0) p.inferRing[p.inferSamples++ % kTelemetry] = float(network >= 0 ? network : p.inferMs);
        ++p.inferCount;
        if (p.markerArmed && done && p.inferCount >= kSteadyInferences) { p.markerArmed = false; clearCrashMarker(); }
        if (std::chrono::steady_clock::now() - p.lastSummary > std::chrono::seconds(30)) { p.lastSummary = std::chrono::steady_clock::now(); summarize = true; }
    }
    if (!done) {
        const HRESULT removed = p.device ? p.device->GetDeviceRemovedReason() : S_OK;
        veyra::log::error("pushflow", std::format("inference failed after {:.1f} ms; device state 0x{:08X}{}", p.inferMs, uint32_t(removed), removed != S_OK ? " (device removed)" : ""));
    } else if (p.inferCount == 1) {
        veyra::log::info("pushflow", std::format("first inference {:.1f} ms (includes DirectML's one-time preparation)", p.inferMs));
    }
    if (summarize) {
        const auto t = telemetry();
        veyra::log::info("pushflow", std::format("telemetry inferences={} p50={:.2f}ms p95={:.2f}ms max={:.2f}ms fullPacks={} timePacks={} precision={} rebuild={} "
            "encoderCache={} encoderRuns={} kept={} made={}",
            t.inferences, t.inferP50Ms, t.inferP95Ms, t.inferMaxMs, t.fullPacks, t.timePacks, p.status.precision, p.status.rebuild ? 1 : 0,
            p.status.splitState.empty() ? std::string("-") : p.status.splitState, t.encoderRuns, t.featureHits, t.featureMisses));
    }
    return done;
#else
    return false;
#endif
}

bool PushFlowBackend::resolve(ID3D12GraphicsCommandList* list, ID3D12Resource* previous, ID3D12Resource* current, ID3D12Resource* output) {
    auto& p = *p_;
    if (!p.status.ready || !list || !output) return false;
    if (p.timeQueries && p.timePending) {
        const UINT slot = UINT(p.timeWritten % Impl::kTimeRing);
        list->EndQuery(p.timeQueries.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 2 * slot + 1);
        list->ResolveQueryData(p.timeQueries.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 2 * slot, 2, p.timeReadback.Get(), 2 * slot * sizeof(uint64_t));
        ++p.timeWritten; p.timePending = false;
    }
    const UINT outputs = p.haveGrids ? 4 : 1;
    D3D12_RESOURCE_BARRIER barriers[4]{};
    for (UINT i = 0; i < outputs; ++i) { barriers[i].Type = D3D12_RESOURCE_BARRIER_TYPE_UAV; barriers[i].UAV.pResource = p.buffers[i + 1].Get(); }
    list->ResourceBarrier(outputs, barriers);
    p.states.transition(list, p.picture.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    if (p.status.rebuild && previous && current) {
        const UINT first = 4 + 2 * p.rebuildRing;
        p.rebuildRing = (p.rebuildRing + 1) % kSrvRing;
        D3D12_SHADER_RESOURCE_VIEW_DESC view{};
        view.Format = p.format; view.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        view.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING; view.Texture2D.MipLevels = 1;
        p.stager.stageSrv(previous, &view, p.rebuildPass.heap.Get(), first);
        p.stager.stageSrv(current, &view, p.rebuildPass.heap.Get(), first + 1);
        const float constants[12] = {bits(p.width), bits(p.height), bits(p.status.padW), bits(p.status.padH),
                                     bits(p.status.modelW), bits(p.status.modelH), bits(uint32_t(p.encoding)), bits(p.facts.alignCorners ? 1u : 0u),
                                     bits(p.facts.maskForFirst ? 1u : 0u), 0.0f, 0.0f, 0.0f};
        p.rebuildPass.bind(list, constants, gpuSlot(p.rebuildPass, first), gpuSlot(p.rebuildPass, 0));
    } else {
        const float constants[8] = {bits(p.width), bits(p.height), bits(p.status.padW), bits(p.status.padH),
                                    bits(p.status.modelW), bits(p.status.modelH), bits(uint32_t(p.encoding)), 0.0f};
        p.unpackPass.bind(list, constants, gpuSlot(p.unpackPass, 0), gpuSlot(p.unpackPass, 1));
    }
    list->Dispatch((p.width + 15) / 16, (p.height + 15) / 16, 1);
    ID3D12Resource* shown = p.picture.Get();
    if (p.guardOn && previous && current) {
        // PushFlow 2's Guard: the picture just made is read, the guarded one written (PushGuard.hlsl).
        const UINT first = 6 + 3 * p.guardRing;
        p.guardRing = (p.guardRing + 1) % kSrvRing;
        D3D12_RESOURCE_BARRIER made{}; made.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV; made.UAV.pResource = p.picture.Get();
        list->ResourceBarrier(1, &made);
        p.states.transition(list, p.picture.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        p.states.transition(list, p.guarded.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        D3D12_SHADER_RESOURCE_VIEW_DESC view{};
        view.Format = p.format; view.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        view.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING; view.Texture2D.MipLevels = 1;
        p.stager.stageSrv(previous, &view, p.guardPass.heap.Get(), first);
        p.stager.stageSrv(current, &view, p.guardPass.heap.Get(), first + 1);
        p.stager.stageSrv(p.picture.Get(), &view, p.guardPass.heap.Get(), first + 2);
        const uint32_t tw = p.guardTilesW, th = p.guardTilesH;
        const uint32_t serial = uint32_t(p.guardWritten % 0x7FFFFFFFull) + 1;
        const auto stage = [&](uint32_t which, uint32_t across, uint32_t down) {
            const float constants[16] = {bits(p.width), bits(p.height), bits(p.status.padW), bits(p.status.padH),
                                         bits(p.status.modelW), bits(p.status.modelH), bits(uint32_t(p.encoding)), bits(p.facts.alignCorners ? 1u : 0u),
                                         bits(p.facts.maskForFirst ? 1u : 0u), bits(which), bits(tw), bits(th),
                                         bits(p.lastTimestep > 0.5f ? 1u : 0u), bits(serial), 0.0f, 0.0f};
            p.guardPass.bind(list, constants, gpuSlot(p.guardPass, first), gpuSlot(p.guardPass, 0));
            list->Dispatch((across + 7) / 8, (down + 7) / 8, 1);
            D3D12_RESOURCE_BARRIER wait[2]{};
            wait[0].Type = wait[1].Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
            wait[0].UAV.pResource = p.guardTiles.Get(); wait[1].UAV.pResource = p.guardCounts.Get();
            list->ResourceBarrier(2, wait);
        };
        stage(0, tw, th);              // each tile: how much it changed
        stage(1, tw * 16, th * 16);    // each motion pixel: what it shows of each frame
        stage(2, 1, 1);                // the decision
        stage(3, p.width, p.height);   // the picture
        if (p.guardMapped) {
            D3D12_RESOURCE_BARRIER copy{}; copy.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION; copy.Transition.pResource = p.guardCounts.Get();
            copy.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            copy.Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS; copy.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
            list->ResourceBarrier(1, &copy);
            list->CopyBufferRegion(p.guardReadback.Get(), (p.guardWritten % Impl::kGuardRing) * 4 * sizeof(uint32_t), p.guardCounts.Get(),
                                   uint64_t(tw) * th * 3 * sizeof(uint32_t), 4 * sizeof(uint32_t));
            std::swap(copy.Transition.StateBefore, copy.Transition.StateAfter);
            list->ResourceBarrier(1, &copy);
        }
        ++p.guardWritten;
        shown = p.guarded.Get();
    }
    p.states.transition(list, shown, D3D12_RESOURCE_STATE_COPY_SOURCE);
    list->CopyResource(output, shown);
    return true;
}

bool PushFlowBackend::forecast(ID3D12GraphicsCommandList* list, ID3D12Resource* previous, ID3D12Resource* current, ID3D12Resource* output, float ahead) {
    auto& p = *p_;
    if (!p.status.ready || !p.predictOn || !list || !previous || !current || !output) return false;
    // The network's time, as resolve() takes it: this is the first reader of its output.
    if (p.timeQueries && p.timePending) {
        const UINT slot = UINT(p.timeWritten % Impl::kTimeRing);
        list->EndQuery(p.timeQueries.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 2 * slot + 1);
        list->ResolveQueryData(p.timeQueries.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 2 * slot, 2, p.timeReadback.Get(), 2 * slot * sizeof(uint64_t));
        ++p.timeWritten; p.timePending = false;
    }
    D3D12_RESOURCE_BARRIER grids[2]{};
    grids[0].Type = grids[1].Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    grids[0].UAV.pResource = p.buffers[2].Get(); grids[1].UAV.pResource = p.buffers[3].Get();
    list->ResourceBarrier(2, grids);
    p.states.transition(list, p.predicted.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    const UINT first = 4 + 2 * p.predictRing;
    p.predictRing = (p.predictRing + 1) % kSrvRing;
    D3D12_SHADER_RESOURCE_VIEW_DESC view{};
    view.Format = p.format; view.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    view.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING; view.Texture2D.MipLevels = 1;
    p.stager.stageSrv(previous, &view, p.predictPass.heap.Get(), first);
    p.stager.stageSrv(current, &view, p.predictPass.heap.Get(), first + 1);
    const uint32_t serial = uint32_t(p.predictWritten % 0x7FFFFFFFull) + 1;
    const auto stage = [&](uint32_t which, uint32_t across, uint32_t down) {
        const float constants[12] = {bits(p.width), bits(p.height), bits(p.status.padW), bits(p.status.padH),
                                     bits(p.status.modelW), bits(p.status.modelH), bits(uint32_t(p.encoding)), bits(p.facts.alignCorners ? 1u : 0u),
                                     bits(which), bits(serial), std::clamp(ahead, 0.0f, 1.0f), 0.0f};
        p.predictPass.bind(list, constants, gpuSlot(p.predictPass, first), gpuSlot(p.predictPass, 0));
        list->Dispatch((across + 7) / 8, (down + 7) / 8, 1);
        D3D12_RESOURCE_BARRIER wait{}; wait.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV; wait.UAV.pResource = p.predictTotals.Get();
        list->ResourceBarrier(1, &wait);
    };
    stage(0, 1, 1);                // the totals start at zero
    stage(1, p.width, p.height);   // the picture
    if (p.predictMapped) {
        D3D12_RESOURCE_BARRIER copy{}; copy.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION; copy.Transition.pResource = p.predictTotals.Get();
        copy.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        copy.Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS; copy.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
        list->ResourceBarrier(1, &copy);
        list->CopyBufferRegion(p.predictReadback.Get(), (p.predictWritten % Impl::kPredictRing) * 4 * sizeof(uint32_t), p.predictTotals.Get(), 0, 4 * sizeof(uint32_t));
        std::swap(copy.Transition.StateBefore, copy.Transition.StateAfter);
        list->ResourceBarrier(1, &copy);
    }
    ++p.predictWritten;
    p.states.transition(list, p.predicted.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE);
    list->CopyResource(output, p.predicted.Get());
    return true;
}

} // namespace veyra::pushflow
