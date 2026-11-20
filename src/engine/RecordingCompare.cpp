// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 DosukaSOL and the PushFPS contributors. Part of PushFPS, a modified version of Veyra.
#include "veyra/engine/RecordingCompare.h"
#include "veyra/pipeline/GpuPassUtils.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace veyra::engine {
namespace {
constexpr uint32_t kPitchAlign = D3D12_TEXTURE_DATA_PITCH_ALIGNMENT;          // 256
constexpr uint64_t kPlacementAlign = D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT;  // 512
constexpr UINT kRawSlots = 8;   // one raw-frame descriptor per recorder frame slot (LiveRecorder kSlots = 6)
constexpr UINT kMaxOverlays = 3;   // two badges and, in the Reveal layout, the sweeping line
uint32_t alignPitch(uint32_t bytes) { return (bytes + kPitchAlign - 1) / kPitchAlign * kPitchAlign; }
uint64_t alignOffset(uint64_t at) { return (at + kPlacementAlign - 1) / kPlacementAlign * kPlacementAlign; }
uint8_t limited(uint8_t full) { return uint8_t(16 + (uint32_t(full) * 219 + 127) / 255); }
// 10-bit PQ code: 64 (black) .. 573 (203 nits, BT.2408 reference white), as RgbToNv12.hlsl codes it.
uint32_t hdrLuma(uint8_t full) { return 64 + (uint32_t(full) * (573 - 64) + 127) / 255; }
void bindConstants(const pipeline::ComputePass& pass, ID3D12GraphicsCommandList* list, const uint32_t (&values)[8], UINT srvSlot, UINT uavSlot) {
    float bits[8];
    std::memcpy(bits, values, sizeof(bits));
    pass.bind(list, bits, pipeline::gpuHandleOf(pass, srvSlot).ptr, pipeline::gpuHandleOf(pass, uavSlot).ptr);
}
bool hasRgba(const RecordingLabel& l) { return l.width >= 2 && l.height >= 2 && l.rgba.size() >= size_t(l.width) * l.height * 4; }
bool hasLuma(const RecordingLabel& l) { return l.width >= 2 && l.height >= 2 && l.luma.size() >= size_t(l.width) * l.height; }
// A colour label as grey levels over black, for when blending is unavailable.
RecordingLabel lumaOf(const RecordingLabel& l) {
    RecordingLabel out{l.width, l.height, std::vector<uint8_t>(size_t(l.width) * l.height), {}};
    for (size_t i = 0; i < out.luma.size(); ++i) {
        const uint8_t* p = &l.rgba[i * 4];
        const uint32_t grey = (uint32_t(p[0]) * 54 + uint32_t(p[1]) * 183 + uint32_t(p[2]) * 19) >> 8;
        out.luma[i] = uint8_t((grey * p[3] + 127) / 255);
    }
    return out;
}
} // namespace

// Side by side: copies of the current frame, the compute pass and its descriptors
// (heap slots 0-3: current y, current uv, held y, held uv; 4-5: the recorder's y and uv).
struct RecordingCompare::SideBySide {
    Microsoft::WRL::ComPtr<ID3D12Resource> curY, curUv;
    pipeline::ComputePass pass;
    pipeline::DescriptorStager stager;
    ID3D12Device* device = nullptr;
    ID3D12Resource* boundY = nullptr;
    ID3D12Resource* boundUv = nullptr;
};
// Vertical: the recorder's planes and the held planes (heap slots 0-3) scaled into the
// 9:16 output planes (4-5).
struct RecordingCompare::Vertical {
    pipeline::ComputePass pass;
    pipeline::DescriptorStager stager;
    ID3D12Device* device = nullptr;
    ID3D12Resource* boundY = nullptr;
    ID3D12Resource* boundUv = nullptr;
};
// Raw console picture -> the held planes (heap slots 0..7: the raw frame per recorder slot; 8-9: held y and uv).
struct RecordingCompare::Raw {
    pipeline::ComputePass pass;
    pipeline::DescriptorStager stager;
    ID3D12Device* device = nullptr;
};
// Colour labels blended in (per label i: slot 3i overlay, 3i+1 background y, 3i+2 background uv; 6-7: the recorder's y and uv).
struct RecordingCompare::Blend {
    pipeline::ComputePass pass;
    pipeline::DescriptorStager stager;
    ID3D12Device* device = nullptr;
    Microsoft::WRL::ComPtr<ID3D12Resource> upload;
    Microsoft::WRL::ComPtr<ID3D12Resource> overlay[kMaxOverlays], bgY[kMaxOverlays], bgUv[kMaxOverlays];
    bool uploaded = false;
    ID3D12Resource* boundY = nullptr;
    ID3D12Resource* boundUv = nullptr;
};

RecordingCompare::RecordingCompare() = default;
RecordingCompare::~RecordingCompare() = default;

RecordingLabel scaleRecordingLabel(const RecordingLabel& label, uint32_t width, uint32_t height) {
    RecordingLabel out;
    const bool luma = hasLuma(label), rgba = hasRgba(label);
    if (!width || !height || (!luma && !rgba)) return out;
    out.width = width; out.height = height;
    if (luma) out.luma.resize(size_t(width) * height);
    if (rgba) out.rgba.resize(size_t(width) * height * 4);
    // Area average: each output pixel covers sx x sy source pixels (partial ones weighted).
    const double sx = double(label.width) / width, sy = double(label.height) / height;
    for (uint32_t y = 0; y < height; ++y) {
        const double y0 = y * sy, y1 = std::min(double(label.height), y0 + sy);
        for (uint32_t x = 0; x < width; ++x) {
            const double x0 = x * sx, x1 = std::min(double(label.width), x0 + sx);
            double grey = 0, area = 0, alpha = 0, r = 0, g = 0, b = 0;
            for (uint32_t yy = uint32_t(y0); yy < label.height && double(yy) < y1; ++yy) {
                const double hy = std::min(y1, yy + 1.0) - std::max(y0, double(yy));
                if (hy <= 0) continue;
                for (uint32_t xx = uint32_t(x0); xx < label.width && double(xx) < x1; ++xx) {
                    const double wx = std::min(x1, xx + 1.0) - std::max(x0, double(xx));
                    if (wx <= 0) continue;
                    const double w = wx * hy;
                    const size_t at = size_t(yy) * label.width + xx;
                    area += w;
                    if (luma) grey += w * label.luma[at];
                    if (rgba) {
                        const uint8_t* p = &label.rgba[at * 4];
                        const double a = w * p[3];
                        alpha += a; r += a * p[0]; g += a * p[1]; b += a * p[2];
                    }
                }
            }
            const size_t to = size_t(y) * width + x;
            const auto byte = [](double v) { return uint8_t(std::clamp(v + 0.5, 0.0, 255.0)); };
            if (luma) out.luma[to] = byte(area > 0 ? grey / area : 0.0);
            if (rgba) {
                uint8_t* p = &out.rgba[to * 4];
                if (alpha > 0) { p[0] = byte(r / alpha); p[1] = byte(g / alpha); p[2] = byte(b / alpha); }
                else p[0] = p[1] = p[2] = 0;
                p[3] = byte(area > 0 ? alpha / area : 0.0);
            }
        }
    }
    return out;
}

bool RecordingCompare::create(ID3D12Device* device, uint32_t width, uint32_t height, bool hdr, const RecordingLabel& left,
                              const RecordingLabel& right, std::wstring& reason, CompareLayout layout) {
    width_ = width; height_ = height; hdr_ = hdr; layout_ = layout;
    split_ = (width / 2) & ~1u;
    holdValid_ = rawHold_ = false; generated_ = real_ = rawFrames_ = 0;
    patches_.clear(); overlays_.clear(); upload_.Reset(); sideBySide_.reset(); vertical_.reset(); raw_.reset(); blend_.reset();
    outY_.Reset(); outUv_.Reset(); crop_ = {};
    revealPush_ = revealConsole_ = revealLine_ = -1; lineW_ = 0; revealX_ = 0; revealLineShown_ = false;
    const bool sbs = layout == CompareLayout::SideBySide;
    const bool vert = layout == CompareLayout::Vertical;
    const bool reveal = layout == CompareLayout::Reveal;
    compareOutputSize(layout, width, height, outW_, outH_);
    pictureW_ = sbs ? split_ : vert ? outW_ : 0;
    pictureH_ = sbs ? (height / 2) & ~1u : vert ? (outH_ / 2) & ~1u : 0;
    top_ = sbs ? ((height - pictureH_) / 2) & ~1u : 0;
    rightX_ = sbs ? width - pictureW_ : 0;
    if (vert) crop_ = compareVerticalCrop(width, height, pictureW_, pictureH_);
    const DXGI_FORMAT yFormat = hdr ? DXGI_FORMAT_R16_UNORM : DXGI_FORMAT_R8_UNORM;
    const DXGI_FORMAT uvFormat = hdr ? DXGI_FORMAT_R16G16_UNORM : DXGI_FORMAT_R8G8_UNORM;
    holdY_ = pipeline::makeTexture(device, width, height, yFormat, true);
    holdUv_ = pipeline::makeTexture(device, width / 2, height / 2, uvFormat, true); // same flags as y and uv (CopyResource)
    if (!holdY_ || !holdUv_) { reason = L"录制显存分配失败"; return false; }
    D3D12_SHADER_RESOURCE_VIEW_DESC view{};
    view.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    view.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    view.Texture2D.MipLevels = 1;
    if (sbs) {
        auto side = std::make_unique<SideBySide>();
        side->device = device;
        side->curY = pipeline::makeTexture(device, width, height, yFormat, true);
        side->curUv = pipeline::makeTexture(device, width / 2, height / 2, uvFormat, true);
        std::vector<uint8_t> shader;
        if (!side->curY || !side->curUv) { reason = L"录制显存分配失败"; return false; }
        if (!side->pass.loadShader("PushCompareSideBySide.dxil", shader) || !side->pass.create(device, shader, 6, 4, 2) ||
            !side->stager.initialize(device, 4)) { reason = L"对比片段着色器加载失败"; return false; }
        ID3D12Resource* const sources[4] = {side->curY.Get(), side->curUv.Get(), holdY_.Get(), holdUv_.Get()};
        for (UINT i = 0; i < 4; ++i) {
            view.Format = (i & 1) ? uvFormat : yFormat;
            side->stager.stageSrv(sources[i], &view, side->pass.heap.Get(), i);
        }
        sideBySide_ = std::move(side);
    }
    if (vert) {
        auto v = std::make_unique<Vertical>();
        v->device = device;
        outY_ = pipeline::makeTexture(device, outW_, outH_, yFormat, true);
        outUv_ = pipeline::makeTexture(device, outW_ / 2, outH_ / 2, uvFormat, true);
        if (!outY_ || !outUv_ || !crop_.width || !crop_.height) { reason = L"录制显存分配失败"; return false; }
        std::vector<uint8_t> shader;
        if (!v->pass.loadShader("PushCompareVertical.dxil", shader) || !v->pass.create(device, shader, 6, 4, 2, 12, 0, true) ||
            !v->stager.initialize(device, 4)) { reason = L"对比片段着色器加载失败"; return false; }
        view.Format = yFormat;
        v->stager.stageSrv(holdY_.Get(), &view, v->pass.heap.Get(), 2);
        view.Format = uvFormat;
        v->stager.stageSrv(holdUv_.Get(), &view, v->pass.heap.Get(), 3);
        pipeline::makeUav(device, outY_.Get(), yFormat, pipeline::cpuHandleOf(v->pass, 4));
        pipeline::makeUav(device, outUv_.Get(), uvFormat, pipeline::cpuHandleOf(v->pass, 5));
        vertical_ = std::move(v);
    }
    {
        // The raw console picture. Optional: without it the console side is the held real frame.
        auto raw = std::make_unique<Raw>();
        raw->device = device;
        std::vector<uint8_t> shader;
        if (raw->pass.loadShader("PushCompareRaw.dxil", shader) && raw->pass.create(device, shader, kRawSlots + 2, 1, 2, 8, 0, true) &&
            raw->stager.initialize(device, kRawSlots)) {
            pipeline::makeUav(device, holdY_.Get(), yFormat, pipeline::cpuHandleOf(raw->pass, kRawSlots));
            pipeline::makeUav(device, holdUv_.Get(), uvFormat, pipeline::cpuHandleOf(raw->pass, kRawSlots + 1));
            raw_ = std::move(raw);
        }
    }

    // Divider (4 pixels, white) and the two labels, each scaled down when it does not fit.
    // Split: labels at the top corners of the picture. Side by side: centred in the black bar
    // above each picture (on the picture's top edge when the bar is too thin). Vertical: centred,
    // just above and just below the divider. Positions are in the composed picture (W x H).
    const uint32_t W = vert ? outW_ : width, H = vert ? outH_ : height;
    const uint32_t divider = 4;
    const uint32_t margin = std::max<uint32_t>(8, H / 36) & ~1u;
    const bool inBar = sbs && top_ >= 24;
    const uint32_t barMargin = std::max<uint32_t>(2, top_ / 8) & ~1u;
    uint32_t maxW, maxH;
    if (vert) {
        maxW = W > 2 * margin ? W - 2 * margin : 0;
        maxH = pictureH_ / 4;
    } else if (sbs) {
        maxW = pictureW_ > 2 * margin ? pictureW_ - 2 * margin : 0;
        maxH = inBar ? (top_ > 2 * barMargin ? top_ - 2 * barMargin : 0) : (pictureH_ / 3 > 2 * margin ? pictureH_ / 3 - 2 * margin : 0);
    } else {
        maxW = split_ > 2 * margin + divider ? split_ - 2 * margin - divider : 0;
        maxH = height / 3 > 2 * margin ? height / 3 - 2 * margin : 0;
    }
    RecordingLabel fitted[2];
    const auto fit = [&](const RecordingLabel& l, RecordingLabel& scaled) -> const RecordingLabel* {
        if (!hasRgba(l) && !hasLuma(l)) return nullptr;
        if ((l.width & ~1u) <= maxW && (l.height & ~1u) <= maxH) return &l;
        const double f = std::min(double(maxW) / l.width, double(maxH) / l.height);
        const uint32_t w = uint32_t(l.width * f), h = uint32_t(l.height * f);
        if (w < 8 || h < 8) return nullptr; // unreadable: leave it out
        scaled = scaleRecordingLabel(l, w, h);
        return &scaled;
    };
    const RecordingLabel* labels[2] = {fit(left, fitted[0]), fit(right, fitted[1])};
    struct Place { uint32_t x, y, w, h; const RecordingLabel* label; int role; };   // role: 0 the console's label, 1 PushFPS's, 2 the Reveal line
    std::vector<Place> places;
    for (int side = 0; side < 2; ++side) {
        const auto* l = labels[side];
        if (!l) continue;
        const uint32_t w = l->width & ~1u, h = l->height & ~1u;
        uint32_t x, y;
        if (vert) {
            x = ((W - w) / 2) & ~1u;
            y = side ? (pictureH_ + divider / 2 + margin) & ~1u : (pictureH_ - divider / 2 - margin - h) & ~1u;
        } else if (sbs) {
            const uint32_t x0 = side ? rightX_ : 0;
            x = (x0 + (pictureW_ - w) / 2) & ~1u;
            y = inBar ? ((top_ - h) / 2) & ~1u : (top_ + margin) & ~1u;
        } else {
            // Split: the console (side 0) on the left. Reveal: PushFPS is left of the line, so the
            // badges change corners.
            x = (side != 0) != reveal ? (W - margin - w) & ~1u : margin;
            y = margin;
        }
        places.push_back({x, y, w, h, l, side});
    }
    // Reveal: the line, a strip as high as the picture. Its place changes with every frame.
    const RecordingLabel line = reveal ? compareRevealLine(height) : RecordingLabel{};

    // Colour labels: blended in by a compute pass. If it cannot load they are copied as grey levels.
    std::vector<RecordingLabel> greyed;
    greyed.reserve(2);
    std::vector<Place> blended;
    std::vector<int> roles;   // per blended overlay: 0 the console's badge, 1 PushFPS's, 2 the line
    for (auto& p : places)
        if (hasRgba(*p.label)) { blended.push_back(p); roles.push_back(p.role); }
    if (reveal && hasRgba(line) && line.height == (height & ~1u)) {
        blended.push_back({0, 0, line.width, line.height, &line, 2});
        roles.push_back(2);
    }
    if (!blended.empty()) {
        auto b = std::make_unique<Blend>();
        b->device = device;
        std::vector<uint8_t> shader;
        bool ok = b->pass.loadShader("PushCompareOverlay.dxil", shader) && b->pass.create(device, shader, kMaxOverlays * 3 + 2, 3, 2) &&
                  b->stager.initialize(device, kMaxOverlays * 3);
        uint64_t at = 0;
        std::vector<Overlay> overlays;
        for (size_t i = 0; ok && i < blended.size(); ++i) {
            const auto& p = blended[i];
            Overlay o{p.x, p.y, p.w, p.h};
            o.pitch = alignPitch(p.w * 4); o.offset = alignOffset(at); at = o.offset + uint64_t(o.pitch) * p.h;
            b->overlay[i] = pipeline::makeTexture(device, p.w, p.h, DXGI_FORMAT_R8G8B8A8_UNORM, false);
            b->bgY[i] = pipeline::makeTexture(device, p.w, p.h, yFormat, false);
            b->bgUv[i] = pipeline::makeTexture(device, p.w / 2, p.h / 2, uvFormat, false);
            ok = b->overlay[i] && b->bgY[i] && b->bgUv[i];
            overlays.push_back(o);
        }
        if (ok) {
            D3D12_HEAP_PROPERTIES hp{}; hp.Type = D3D12_HEAP_TYPE_UPLOAD;
            D3D12_RESOURCE_DESC bd{}; bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER; bd.Width = std::max<uint64_t>(at, 256); bd.Height = 1;
            bd.DepthOrArraySize = 1; bd.MipLevels = 1; bd.SampleDesc.Count = 1; bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
            uint8_t* mapped = nullptr;
            const D3D12_RANGE none{0, 0};
            ok = SUCCEEDED(device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&b->upload))) &&
                 SUCCEEDED(b->upload->Map(0, &none, reinterpret_cast<void**>(&mapped))) && mapped;
            for (size_t i = 0; ok && i < blended.size(); ++i) {
                const auto& p = blended[i];
                for (uint32_t row = 0; row < p.h; ++row)
                    std::memcpy(mapped + overlays[i].offset + uint64_t(overlays[i].pitch) * row, &p.label->rgba[size_t(row) * p.label->width * 4], size_t(p.w) * 4);
            }
            if (mapped) b->upload->Unmap(0, nullptr);
        }
        if (ok) {
            for (size_t i = 0; i < blended.size(); ++i) {
                view.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
                b->stager.stageSrv(b->overlay[i].Get(), &view, b->pass.heap.Get(), UINT(i * 3));
                view.Format = yFormat;
                b->stager.stageSrv(b->bgY[i].Get(), &view, b->pass.heap.Get(), UINT(i * 3 + 1));
                view.Format = uvFormat;
                b->stager.stageSrv(b->bgUv[i].Get(), &view, b->pass.heap.Get(), UINT(i * 3 + 2));
            }
            overlays_ = std::move(overlays);
            blend_ = std::move(b);
            if (reveal)
                for (size_t i = 0; i < roles.size(); ++i) {
                    if (roles[i] == 0) revealConsole_ = int(i);
                    else if (roles[i] == 1) revealPush_ = int(i);
                    else { revealLine_ = int(i); lineW_ = overlays_[i].width; }
                }
        } else {
            for (auto& p : places)
                if (hasRgba(*p.label)) { greyed.push_back(lumaOf(*p.label)); p.label = &greyed.back(); }
        }
    }

    struct Source { uint32_t x, y, w, h; const RecordingLabel* label; };
    std::vector<Source> sources;
    if (vert) sources.push_back({0, pictureH_ - divider / 2, W & ~1u, divider, nullptr});
    else if (sbs) sources.push_back({split_ - divider / 2, top_, divider, pictureH_, nullptr});
    else if (!reveal) sources.push_back({split_ - divider / 2, 0, divider, height & ~1u, nullptr});
    // Reveal has no fixed divider, and a label that could not be blended is left out: a copied one
    // could not be cut at the moving line.
    for (const auto& p : places)
        if (!reveal && !(blend_ && hasRgba(*p.label))) sources.push_back({p.x, p.y, p.w, p.h, p.label});

    // SDR: limited-range 8-bit (white 235, black 16, neutral chroma 128).
    // HDR: P010 (10-bit code in the top bits), PQ: white 573 = 203 nits, black 64, chroma 512.
    const uint32_t bytes = hdr ? 2 : 1;
    uint64_t at = 0;
    for (const auto& s : sources) {
        Patch p{s.x, s.y, s.w, s.h};
        p.yPitch = alignPitch(s.w * bytes); p.yOffset = alignOffset(at); at = p.yOffset + uint64_t(p.yPitch) * s.h;
        p.uvPitch = alignPitch(s.w * bytes); p.uvOffset = alignOffset(at); at = p.uvOffset + uint64_t(p.uvPitch) * (s.h / 2); // w/2 texels of 2 channels
        patches_.push_back(p);
    }
    D3D12_HEAP_PROPERTIES hp{}; hp.Type = D3D12_HEAP_TYPE_UPLOAD;
    D3D12_RESOURCE_DESC bd{}; bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER; bd.Width = std::max<uint64_t>(at, 256); bd.Height = 1;
    bd.DepthOrArraySize = 1; bd.MipLevels = 1; bd.SampleDesc.Count = 1; bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    if (FAILED(device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&upload_)))) {
        patches_.clear(); return true; // the layout still works without the divider
    }
    uint8_t* mapped = nullptr;
    const D3D12_RANGE none{0, 0};
    if (FAILED(upload_->Map(0, &none, reinterpret_cast<void**>(&mapped))) || !mapped) { patches_.clear(); upload_.Reset(); return true; }
    const auto lumaCode = [&](uint8_t full) -> uint16_t {
        return hdr ? uint16_t(hdrLuma(full) << 6) : uint16_t(limited(full));
    };
    for (size_t i = 0; i < sources.size(); ++i) {
        const auto& s = sources[i];
        const auto& p = patches_[i];
        for (uint32_t row = 0; row < s.h; ++row) {
            uint8_t* out = mapped + p.yOffset + uint64_t(p.yPitch) * row;
            for (uint32_t x = 0; x < s.w; ++x) {
                const uint16_t v = lumaCode(s.label ? s.label->luma[size_t(row) * s.label->width + x] : 255);
                if (hdr) std::memcpy(out + size_t(x) * 2, &v, 2); else out[x] = uint8_t(v);
            }
        }
        const uint16_t neutral = hdr ? uint16_t(512 << 6) : 128;
        for (uint32_t row = 0; row < s.h / 2; ++row) {
            uint8_t* out = mapped + p.uvOffset + uint64_t(p.uvPitch) * row;
            if (!hdr) { std::memset(out, 128, s.w); continue; }
            for (uint32_t x = 0; x < s.w; ++x) std::memcpy(out + size_t(x) * 2, &neutral, 2); // s.w/2 texels x 2 channels
        }
    }
    upload_->Unmap(0, nullptr);
    return true;
}

void RecordingCompare::rawPass(ID3D12GraphicsCommandList* list, pipeline::StateTracker& states, const RecordingRawFrame& raw, unsigned slot) {
    auto& r = *raw_;
    const auto desc = raw.texture->GetDesc();
    D3D12_SHADER_RESOURCE_VIEW_DESC view{};
    view.Format = desc.Format;
    view.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    view.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    view.Texture2D.MipLevels = 1;
    const UINT at = slot % kRawSlots;
    r.stager.stageSrv(raw.texture, &view, r.pass.heap.Get(), at);
    states.transition(list, raw.texture, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    states.transition(list, holdY_.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    states.transition(list, holdUv_.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    const uint32_t constants[8] = {width_, height_, hdr_ ? 1u : 0u, raw.sdrInHdr ? 1u : 0u, 0, 0, 0, 0};
    bindConstants(r.pass, list, constants, at, kRawSlots);
    list->Dispatch((width_ + 15) / 16, (height_ + 15) / 16, 1);
    states.uavBarrier(list, holdY_.Get());
    states.uavBarrier(list, holdUv_.Get());
    states.transition(list, raw.texture, D3D12_RESOURCE_STATE_COMMON);
}

void RecordingCompare::blend(ID3D12GraphicsCommandList* list, pipeline::StateTracker& states, ID3D12Resource* y, ID3D12Resource* uv,
                             const Draw* draws, size_t count) {
    auto& b = *blend_;
    if (!b.uploaded) {
        for (size_t i = 0; i < overlays_.size(); ++i) {
            const auto& o = overlays_[i];
            states.transition(list, b.overlay[i].Get(), D3D12_RESOURCE_STATE_COPY_DEST);
            D3D12_TEXTURE_COPY_LOCATION from{}, to{};
            from.pResource = b.upload.Get(); from.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
            from.PlacedFootprint.Offset = o.offset;
            from.PlacedFootprint.Footprint = {DXGI_FORMAT_R8G8B8A8_UNORM, o.width, o.height, 1, o.pitch};
            to.pResource = b.overlay[i].Get(); to.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            list->CopyTextureRegion(&to, 0, 0, 0, &from, nullptr);
            states.transition(list, b.overlay[i].Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        }
        b.uploaded = true;
    }
    if (b.boundY != y || b.boundUv != uv) {
        pipeline::makeUav(b.device, y, hdr_ ? DXGI_FORMAT_R16_UNORM : DXGI_FORMAT_R8_UNORM, pipeline::cpuHandleOf(b.pass, kMaxOverlays * 3));
        pipeline::makeUav(b.device, uv, hdr_ ? DXGI_FORMAT_R16G16_UNORM : DXGI_FORMAT_R8G8_UNORM, pipeline::cpuHandleOf(b.pass, kMaxOverlays * 3 + 1));
        b.boundY = y; b.boundUv = uv;
    }
    for (size_t n = 0; n < count; ++n) {
        const Draw& o = draws[n];
        const size_t i = o.index;
        if (i >= overlays_.size() || o.width < 2 || o.height < 2) continue;
        // The picture under the label, read by the blend (a UAV is never read here).
        states.transition(list, y, D3D12_RESOURCE_STATE_COPY_SOURCE);
        states.transition(list, uv, D3D12_RESOURCE_STATE_COPY_SOURCE);
        states.transition(list, b.bgY[i].Get(), D3D12_RESOURCE_STATE_COPY_DEST);
        states.transition(list, b.bgUv[i].Get(), D3D12_RESOURCE_STATE_COPY_DEST);
        D3D12_TEXTURE_COPY_LOCATION from{}, to{};
        from.Type = to.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        from.pResource = y; to.pResource = b.bgY[i].Get();
        const D3D12_BOX yBox{o.x, o.y, 0, o.x + o.width, o.y + o.height, 1};
        list->CopyTextureRegion(&to, 0, 0, 0, &from, &yBox);
        from.pResource = uv; to.pResource = b.bgUv[i].Get();
        const D3D12_BOX uvBox{o.x / 2, o.y / 2, 0, (o.x + o.width) / 2, (o.y + o.height) / 2, 1};
        list->CopyTextureRegion(&to, 0, 0, 0, &from, &uvBox);
        states.transition(list, b.bgY[i].Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        states.transition(list, b.bgUv[i].Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        states.transition(list, y, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        states.transition(list, uv, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        const uint32_t constants[8] = {o.x, o.y, o.width, o.height, hdr_ ? 1u : 0u, 0, o.sourceX, 0};
        bindConstants(b.pass, list, constants, UINT(i * 3), kMaxOverlays * 3);
        list->Dispatch((o.width + 15) / 16, (o.height + 15) / 16, 1);
        states.uavBarrier(list, y);
        states.uavBarrier(list, uv);
    }
}

void RecordingCompare::compose(ID3D12GraphicsCommandList* list, pipeline::StateTracker& states, ID3D12Resource* y, ID3D12Resource* uv, bool generated,
                               const RecordingRawFrame* raw, unsigned slot, double seconds) {
    if (!list || !y || !uv || !holdY_) return;
    if (generated) ++generated_; else ++real_;
    const auto copy = [&](ID3D12Resource* to, ID3D12Resource* from) {
        states.transition(list, from, D3D12_RESOURCE_STATE_COPY_SOURCE);
        states.transition(list, to, D3D12_RESOURCE_STATE_COPY_DEST);
        list->CopyResource(to, from);
    };
    // The console side: the raw picture of a real frame when there is one, else the real frame itself.
    if (!generated && raw && raw->texture && raw_) {
        rawPass(list, states, *raw, slot);
        holdValid_ = true; rawHold_ = true; ++rawFrames_;
    } else if (!generated || !holdValid_) {
        copy(holdY_.Get(), y); copy(holdUv_.Get(), uv);
        holdValid_ = true; rawHold_ = false;
    }
    // The planes the divider and labels go on: the vertical output, or the recorder's own.
    ID3D12Resource* const ty = vertical_ ? outY_.Get() : y;
    ID3D12Resource* const tuv = vertical_ ? outUv_.Get() : uv;
    int32_t revealCentre = 0;
    if (vertical_) {
        auto& v = *vertical_;
        if (v.boundY != y || v.boundUv != uv) {
            D3D12_SHADER_RESOURCE_VIEW_DESC view{};
            view.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
            view.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            view.Texture2D.MipLevels = 1;
            view.Format = hdr_ ? DXGI_FORMAT_R16_UNORM : DXGI_FORMAT_R8_UNORM;
            v.stager.stageSrv(y, &view, v.pass.heap.Get(), 0);
            view.Format = hdr_ ? DXGI_FORMAT_R16G16_UNORM : DXGI_FORMAT_R8G8_UNORM;
            v.stager.stageSrv(uv, &view, v.pass.heap.Get(), 1);
            v.boundY = y; v.boundUv = uv;
        }
        for (auto* r : {y, uv, holdY_.Get(), holdUv_.Get()})
            states.transition(list, r, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        states.transition(list, outY_.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        states.transition(list, outUv_.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        const uint32_t constants[12] = {outW_, outH_, width_, height_, crop_.x, crop_.y, crop_.width, crop_.height, pictureH_, hdr_ ? 1u : 0u, 0, 0};
        float bits[12];
        std::memcpy(bits, constants, sizeof(bits));
        v.pass.bind(list, bits, pipeline::gpuHandleOf(v.pass, 0).ptr, pipeline::gpuHandleOf(v.pass, 4).ptr);
        list->Dispatch((outW_ + 15) / 16, (outH_ + 15) / 16, 1);
        states.uavBarrier(list, outY_.Get());
        states.uavBarrier(list, outUv_.Get());
    } else if (sideBySide_) {
        auto& side = *sideBySide_;
        copy(side.curY.Get(), y); copy(side.curUv.Get(), uv);
        if (side.boundY != y || side.boundUv != uv) {
            // UAVs go straight into the visible heap (safe, GpuPassUtils.h driver note).
            pipeline::makeUav(side.device, y, hdr_ ? DXGI_FORMAT_R16_UNORM : DXGI_FORMAT_R8_UNORM, pipeline::cpuHandleOf(side.pass, 4));
            pipeline::makeUav(side.device, uv, hdr_ ? DXGI_FORMAT_R16G16_UNORM : DXGI_FORMAT_R8G8_UNORM, pipeline::cpuHandleOf(side.pass, 5));
            side.boundY = y; side.boundUv = uv;
        }
        for (auto* r : {side.curY.Get(), side.curUv.Get(), holdY_.Get(), holdUv_.Get()})
            states.transition(list, r, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        states.transition(list, y, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        states.transition(list, uv, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        const uint32_t constants[8] = {width_, height_, pictureW_, pictureH_, top_, rightX_, hdr_ ? 1u : 0u, 0};
        bindConstants(side.pass, list, constants, 0, 4);
        list->Dispatch((width_ + 15) / 16, (height_ + 15) / 16, 1);
        states.uavBarrier(list, y);
        states.uavBarrier(list, uv);
    } else if (layout_ == CompareLayout::Reveal) {
        // Reveal: left of the line every presented frame, from the line on the console side. Where
        // the line is follows from the frame's time in the clip alone, so it moves as evenly as the
        // frames themselves arrive.
        const uint32_t right = width_ & ~1u;
        const int32_t centre = compareRevealCentre(compareRevealPosition(seconds, revealInterval_), right, lineW_);
        revealCentre = centre;
        revealX_ = uint32_t(std::clamp<int32_t>(centre, 0, int32_t(right)));
        // A real frame without a raw picture is already the console picture, whole.
        if (revealX_ < right && (generated || rawHold_)) {
            states.transition(list, y, D3D12_RESOURCE_STATE_COPY_DEST);
            states.transition(list, uv, D3D12_RESOURCE_STATE_COPY_DEST);
            states.transition(list, holdY_.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE);
            states.transition(list, holdUv_.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE);
            D3D12_TEXTURE_COPY_LOCATION from{}, to{};
            from.Type = to.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            from.pResource = holdY_.Get(); to.pResource = y;
            const D3D12_BOX yBox{revealX_, 0, 0, right, height_ & ~1u, 1};
            list->CopyTextureRegion(&to, revealX_, 0, 0, &from, &yBox);
            from.pResource = holdUv_.Get(); to.pResource = uv;
            const D3D12_BOX uvBox{revealX_ / 2, 0, 0, right / 2, height_ / 2, 1};
            list->CopyTextureRegion(&to, revealX_ / 2, 0, 0, &from, &uvBox);
        }
    } else if (split_ && (generated || rawHold_)) {
        // Split: the left half shows the console side. A real frame without a raw picture is
        // already the console picture, whole.
        states.transition(list, y, D3D12_RESOURCE_STATE_COPY_DEST);
        states.transition(list, uv, D3D12_RESOURCE_STATE_COPY_DEST);
        states.transition(list, holdY_.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE);
        states.transition(list, holdUv_.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE);
        D3D12_TEXTURE_COPY_LOCATION from{}, to{};
        from.Type = to.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        from.pResource = holdY_.Get(); to.pResource = y;
        const D3D12_BOX yBox{0, 0, 0, split_, height_, 1};
        list->CopyTextureRegion(&to, 0, 0, 0, &from, &yBox);
        from.pResource = holdUv_.Get(); to.pResource = uv;
        const D3D12_BOX uvBox{0, 0, 0, split_ / 2, height_ / 2, 1};
        list->CopyTextureRegion(&to, 0, 0, 0, &from, &uvBox);
    }
    if (!patches_.empty() && upload_) {
        states.transition(list, ty, D3D12_RESOURCE_STATE_COPY_DEST);
        states.transition(list, tuv, D3D12_RESOURCE_STATE_COPY_DEST);
        for (const auto& p : patches_) {
            D3D12_TEXTURE_COPY_LOCATION from{}, to{};
            from.pResource = upload_.Get(); from.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
            to.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX; to.SubresourceIndex = 0;
            from.PlacedFootprint.Offset = p.yOffset;
            from.PlacedFootprint.Footprint = {hdr_ ? DXGI_FORMAT_R16_UNORM : DXGI_FORMAT_R8_UNORM, p.width, p.height, 1, p.yPitch};
            to.pResource = ty;
            list->CopyTextureRegion(&to, p.x, p.y, 0, &from, nullptr);
            from.PlacedFootprint.Offset = p.uvOffset;
            from.PlacedFootprint.Footprint = {hdr_ ? DXGI_FORMAT_R16G16_UNORM : DXGI_FORMAT_R8G8_UNORM, p.width / 2, p.height / 2, 1, p.uvPitch};
            to.pResource = tuv;
            list->CopyTextureRegion(&to, p.x / 2, p.y / 2, 0, &from, nullptr);
        }
    }
    if (!blend_ || overlays_.empty()) return;
    Draw draws[kMaxOverlays];
    size_t count = 0;
    if (layout_ != CompareLayout::Reveal) {
        for (size_t i = 0; i < overlays_.size() && count < kMaxOverlays; ++i)
            draws[count++] = {i, overlays_[i].x, overlays_[i].y, overlays_[i].width, overlays_[i].height, 0};
    } else {
        // Each badge belongs to its picture and is cut at the line with it; the line goes on top.
        revealLineShown_ = false;
        if (revealPush_ >= 0) {
            const auto& o = overlays_[size_t(revealPush_)];
            const uint32_t shown = revealX_ > o.x ? std::min(o.width, (revealX_ - o.x) & ~1u) : 0;
            if (shown >= 2) draws[count++] = {size_t(revealPush_), o.x, o.y, shown, o.height, 0};
        }
        if (revealConsole_ >= 0) {
            const auto& o = overlays_[size_t(revealConsole_)];
            const uint32_t start = std::max(o.x, revealX_);
            if (start + 2 <= o.x + o.width) draws[count++] = {size_t(revealConsole_), start, o.y, o.x + o.width - start, o.height, start - o.x};
        }
        if (revealLine_ >= 0 && lineW_) {
            const auto& o = overlays_[size_t(revealLine_)];
            const int32_t left = revealCentre - int32_t(lineW_ / 2), width = int32_t(width_ & ~1u);
            const int32_t x0 = std::max(left, 0), x1 = std::min(left + int32_t(lineW_), width);
            if (x1 - x0 >= 2) {
                draws[count++] = {size_t(revealLine_), uint32_t(x0), 0, uint32_t(x1 - x0), o.height, uint32_t(x0 - left)};
                revealLineShown_ = true;
            }
        }
    }
    if (count) blend(list, states, ty, tuv, draws, count);
}

} // namespace veyra::engine
