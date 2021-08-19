// VapourSynth wrapper for BM3DCPU
// Copyright (c) 2021 WolframRhodium
//
// This program is free software; you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation; either version 2 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program; if not, write to the Free Software
// Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA

#include "bm3d_impl.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <string>
#include <thread>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <vector>

#include <VapourSynth.h>
#include <VSHelper.h>

#include <immintrin.h>

struct BM3DData {
    VSNodeRef * node;
    VSNodeRef * ref_node;
    const VSVideoInfo * vi;

    std::array<float, 3> sigma;
    int block_step[3];
    int bm_range[3];
    int radius;
    int ps_num[3];
    int ps_range[3];
    bool chroma;

    bool process[3]; // sigma != 0

    std::unordered_map<std::thread::id, float *> buffer; // not used by V-BM3D
};

static void VS_CC BM3DInit(
    VSMap *in, VSMap *out, void **instanceData, VSNode *node,
    VSCore *core, const VSAPI *vsapi
) {

    BM3DData * d = static_cast<BM3DData *>(*instanceData);

    if (d->radius) {
        VSVideoInfo vi = *d->vi;
        vi.height *= 2 * (2 * d->radius + 1);
        vsapi->setVideoInfo(&vi, 1, node);
    } else {
        vsapi->setVideoInfo(d->vi, 1, node);
    }
}

static const VSFrameRef *VS_CC BM3DGetFrame(
    int n, int activationReason, void **instanceData, void **frameData,
    VSFrameContext *frameCtx, VSCore *core, const VSAPI *vsapi
) {

    auto d = static_cast<BM3DData *>(*instanceData);

    if (activationReason == arInitial) {
        int start_frame = std::max(n - d->radius, 0);
        int end_frame = std::min(n + d->radius, d->vi->numFrames - 1);

        for (int i = start_frame; i <= end_frame; ++i) {
            vsapi->requestFrameFilter(i, d->node, frameCtx);
        }
        if (d->ref_node != nullptr) {
            for (int i = start_frame; i <= end_frame; ++i) {
                vsapi->requestFrameFilter(i, d->ref_node, frameCtx);
            }
        }
    } else if (activationReason == arAllFramesReady) {
        const int radius = d->radius;
        const int center = radius;
        const int temporal_width = 2 * radius + 1;
        const std::vector src_frames = [&](){
            std::vector<const VSFrameRef *> temp;
            temp.reserve(temporal_width);
            for (int i = -d->radius; i <= d->radius; ++i) {
                int clamped_n = std::clamp(n + i, 0, d->vi->numFrames - 1);
                temp.push_back(vsapi->getFrameFilter(clamped_n, d->node, frameCtx));
            }
            return temp;
        }();
        const std::vector ref_frames = [&](){
            std::vector<const VSFrameRef *> temp;
            if (d->ref_node) {
                temp.reserve(temporal_width);
                for (int i = -d->radius; i <= d->radius; ++i) {
                    int clamped_n = std::clamp(n + i, 0, d->vi->numFrames - 1);
                    temp.push_back(vsapi->getFrameFilter(clamped_n, d->ref_node, frameCtx));
                }
            }
            return temp;
        }();
        const VSFrameRef * const src_frame = src_frames[center];
        VSFrameRef * const dst_frame = [&](){
            if (radius == 0) {
                const VSFrameRef * fr[] {
                    d->process[0] ? nullptr : src_frame,
                    d->process[1] ? nullptr : src_frame,
                    d->process[2] ? nullptr : src_frame
                };
                const int pl[] { 0, 1, 2 };
                return vsapi->newVideoFrame2(
                    d->vi->format, d->vi->width, d->vi->height,
                    fr, pl, src_frame, core);
            } else {
                return vsapi->newVideoFrame(
                    d->vi->format, d->vi->width, d->vi->height * 2 * temporal_width,
                    src_frame, core);
            }
        }();

        const auto cast_fp = [](auto * p) {
            if constexpr (std::is_same_v<std::decay_t<decltype(p)>, decltype(p)>)
                return reinterpret_cast<const float *>(p);
            else
                return reinterpret_cast<float *>(p);
        };

        if (d->chroma) {
            constexpr bool chroma = true;

            std::vector srcps = [&](){
                std::vector<const float *> temp;
                temp.reserve(3 * temporal_width);
                for (int plane = 0; plane < 3; ++plane) {
                    for (auto & frame : src_frames) {
                        temp.push_back(cast_fp(vsapi->getReadPtr(frame, plane)));
                    }
                }
                return temp;
            }();

            std::array<float * VS_RESTRICT, 3> dstps {
                const_cast<float * VS_RESTRICT>(cast_fp(vsapi->getWritePtr(dst_frame, 0))),
                const_cast<float * VS_RESTRICT>(cast_fp(vsapi->getWritePtr(dst_frame, 1))),
                const_cast<float * VS_RESTRICT>(cast_fp(vsapi->getWritePtr(dst_frame, 2)))
            };

            const int width = vsapi->getFrameWidth(src_frame, 0);
            const int height = vsapi->getFrameHeight(src_frame, 0);
            const int stride = vsapi->getStride(src_frame, 0) / sizeof(float);
            const std::array sigma { d->sigma };
            const int block_step = d->block_step[0];
            const int bm_range = d->bm_range[0];
            const int ps_num = d->ps_num[0];
            const int ps_range = d->ps_range[0];

            float * const buffer = [&]() -> float * {
                if (radius == 0) {
                    const auto thread_id = std::this_thread::get_id();
                    if (d->buffer.count(thread_id) == 0) {
                        float * buffer = vs_aligned_malloc<float>(
                            sizeof(float) * stride * height * 2 * num_planes(chroma), 32);
                        d->buffer.emplace(thread_id, buffer);
                    }
                    return d->buffer[thread_id];
                } else {
                    return nullptr;
                }
            }();

            if (radius == 0) {
                memset(buffer, 0, sizeof(float) * stride * height * 2 * num_planes(chroma));
            } else {
                 for (const auto & dstp : dstps) {
                    memset(dstp, 0, sizeof(float) * stride * height * 2 * temporal_width);
                 }
            }

            if (d->ref_node == nullptr) {
                constexpr bool final_ = false;
                if (radius == 0) {
                    constexpr bool temporal = false;
                    bm3d<temporal, chroma, final_>(
                        dstps, stride, srcps.data(), nullptr,
                        width, height,
                        sigma, block_step, bm_range,
                        radius, ps_num, ps_range,
                        buffer);
                } else {
                    constexpr bool temporal = true;
                    bm3d<temporal, chroma, final_>(
                        dstps, stride, srcps.data(), nullptr,
                        width, height,
                        sigma, block_step, bm_range,
                        radius, ps_num, ps_range,
                        nullptr);
                }

            } else {
                constexpr bool final_ = true;
                std::vector refps = [&](){
                    std::vector<const float *> temp;
                    temp.reserve(3 * temporal_width);
                    for (int plane = 0; plane < 3; ++plane) {
                        for (auto & frame : ref_frames) {
                            temp.push_back(cast_fp(vsapi->getReadPtr(frame, plane)));
                        }
                    }
                    return temp;
                }();
                if (radius == 0) {
                    constexpr bool temporal = false;
                    bm3d<temporal, chroma, final_>(
                        dstps, stride, srcps.data(), refps.data(),
                        width, height,
                        sigma, block_step, bm_range,
                        radius, ps_num, ps_range,
                        buffer);
                } else {
                    constexpr bool temporal = true;
                    bm3d<temporal, chroma, final_>(
                        dstps, stride, srcps.data(), refps.data(),
                        width, height,
                        sigma, block_step, bm_range,
                        radius, ps_num, ps_range,
                        nullptr);
                }
            }
        } else {
            constexpr bool chroma = false;

            for (int plane = 0; plane < d->vi->format->numPlanes; plane++) {
                if (d->process[plane]) {
                    std::vector srcps = [&](){
                        std::vector<const float *> temp;
                        temp.reserve(temporal_width);
                        for (auto & frame : src_frames) {
                            temp.push_back(cast_fp(vsapi->getReadPtr(frame, plane)));
                        }
                        return temp;
                    }();
                    std::array<float * VS_RESTRICT, 1> dstps { const_cast<float * VS_RESTRICT>(cast_fp(vsapi->getWritePtr(dst_frame, plane))) };

                    const int width = vsapi->getFrameWidth(src_frame, plane);
                    const int height = vsapi->getFrameHeight(src_frame, plane);
                    const int stride = vsapi->getStride(src_frame, plane) / sizeof(float);
                    const std::array sigma { d->sigma[plane] };
                    const int block_step = d->block_step[plane];
                    const int bm_range = d->bm_range[plane];
                    const int ps_num = d->ps_num[plane];
                    const int ps_range = d->ps_range[plane];

                    float * const buffer = [&]() -> float * {
                        if (radius == 0) {
                            const auto thread_id = std::this_thread::get_id();
                            if (d->buffer.count(thread_id) == 0) {
                                float * buffer = vs_aligned_malloc<float>(
                                    sizeof(float) * stride * height * 2 * num_planes(chroma), 32);
                                d->buffer.emplace(thread_id, buffer);
                            }
                            return d->buffer[thread_id];
                        } else {
                            return nullptr;
                        }
                    }();

                    if (radius == 0) {
                        memset(buffer, 0, sizeof(float) * stride * height * 2 * num_planes(chroma));
                    } else {
                        for (const auto & dstp : dstps) {
                            memset(dstp, 0, sizeof(float) * stride * height * 2 * temporal_width);
                        }
                    }

                    if (d->ref_node == nullptr) {
                        constexpr bool final_ = false;
                        if (radius == 0) {
                            constexpr bool temporal = false;
                            bm3d<temporal, chroma, final_>(
                                dstps, stride, srcps.data(), nullptr,
                                width, height,
                                sigma, block_step, bm_range,
                                radius, ps_num, ps_range,
                                buffer);
                        } else {
                            constexpr bool temporal = true;
                            bm3d<temporal, chroma, final_>(
                                dstps, stride, srcps.data(), nullptr,
                                width, height,
                                sigma, block_step, bm_range,
                                radius, ps_num, ps_range,
                                nullptr);
                        }
                    } else {
                        constexpr bool final_ = true;
                        std::vector refps = [&](){
                            std::vector<const float *> temp;
                            temp.reserve(temporal_width);
                            for (auto & frame : ref_frames) {
                                temp.push_back(cast_fp(vsapi->getReadPtr(frame, plane)));
                            }
                            return temp;
                        }();
                        if (radius == 0) {
                            constexpr bool temporal = false;
                            bm3d<temporal, chroma, final_>(
                                dstps, stride, srcps.data(), refps.data(),
                                width, height,
                                sigma, block_step, bm_range,
                                radius, ps_num, ps_range,
                                buffer);
                        } else {
                            constexpr bool temporal = true;
                            bm3d<temporal, chroma, final_>(
                                dstps, stride, srcps.data(), refps.data(),
                                width, height,
                                sigma, block_step, bm_range,
                                radius, ps_num, ps_range,
                                nullptr);
                        }
                    }
                }
            }
        }

        for (const auto & frame : src_frames) {
            vsapi->freeFrame(frame);
        }

        for (const auto & frame : ref_frames) {
            vsapi->freeFrame(frame);
        }

        if (radius != 0) {
            VSMap * dst_prop { vsapi->getFramePropsRW(dst_frame) };

            vsapi->propSetInt(dst_prop, "BM3D_V_radius", radius, paReplace);

            int64_t process[3] { d->process[0], d->process[1], d->process[2] };
            vsapi->propSetIntArray(dst_prop, "BM3D_V_process", process, 3);
        }

        return dst_frame;
    }

    return nullptr;
}

static void VS_CC BM3DFree(
    void *instanceData, VSCore *core, const VSAPI *vsapi
) noexcept {

    BM3DData * d = static_cast<BM3DData *>(instanceData);

    for (auto & p : d->buffer) {
        vs_aligned_free(p.second);
    }

    vsapi->freeNode(d->node);
    vsapi->freeNode(d->ref_node);

    delete d;
}

static void VS_CC BM3DCreate(
    const VSMap *in, VSMap *out, void *userData,
    VSCore *core, const VSAPI *vsapi
) noexcept {

    auto d { std::make_unique<BM3DData>() };

    d->node = vsapi->propGetNode(in, "clip", 0, nullptr);
    d->vi = vsapi->getVideoInfo(d->node);
    int width = d->vi->width;
    int height = d->vi->height;

    auto set_error = [&](const std::string & error_message) {
        vsapi->setError(out, ("BM3D: " + error_message).c_str());
        vsapi->freeNode(d->node);
        vsapi->freeNode(d->ref_node);
    };

    if (!isConstantFormat(d->vi) || d->vi->format->sampleType == stInteger ||
        (d->vi->format->sampleType == stFloat && d->vi->format->bitsPerSample != 32)) {
        return set_error("only constant format 32 bit float input supported");
    }

    int error;

    d->ref_node = vsapi->propGetNode(in, "ref", 0, &error);
    if (error) {
        d->ref_node = nullptr;
    } else {
        auto ref_vi = vsapi->getVideoInfo(d->ref_node);
        if (ref_vi->format->id != d->vi->format->id) {
            return set_error("\"ref\" must be of the same format as \"clip\"");
        } else if (ref_vi->width != width || ref_vi->height != height ) {
            return set_error("\"ref\" must be of the same dimensions as \"clip\"");
        } else if (ref_vi->numFrames != d->vi->numFrames) {
            return set_error("\"ref\" must be of the same number of frames as \"clip\"");
        }
    }

    for (unsigned i = 0; i < std::size(d->sigma); ++i) {
        float sigma = static_cast<float>(
            vsapi->propGetFloat(in, "sigma", i, &error));

        if (error) {
            sigma = (i == 0) ? 3.f : d->sigma[i - 1];
        } else if (sigma < 0.f) {
            return set_error("\"sigma\" must be non-negative");
        }

        // assumes grayscale input, hard_thr = 2.7
        sigma *= (3.f / 4.f) / 255.f * 64.f * (d->ref_node == nullptr ? 2.7f : 1.0f);

        d->process[i] = !(sigma < std::numeric_limits<float>::epsilon());

        d->sigma[i] = sigma;
    }

    for (unsigned i = 0; i < std::size(d->block_step); ++i) {
        int block_step = int64ToIntS(
            vsapi->propGetInt(in, "block_step", i, &error));

        if (error) {
            block_step = (i == 0) ? 8 : d->block_step[i - 1];
        } else if (block_step <= 0 || block_step > 8) {
            return set_error("\"block_step\" must be in range [1, 8]");
        }

        d->block_step[i] = block_step;
    }

    for (unsigned i = 0; i < std::size(d->bm_range); ++i) {
        int bm_range = int64ToIntS(
            vsapi->propGetInt(in, "bm_range", i, &error));

        if (error) {
            bm_range = (i == 0) ? 9 : d->bm_range[i - 1];
        } else if (bm_range <= 0) {
            return set_error("\"bm_range\" must be positive");
        }

        d->bm_range[i] = bm_range;
    }

    int radius = int64ToIntS(vsapi->propGetInt(in, "radius", 0, &error));
    if (error) {
        radius = 0;
    } else if (radius < 0) {
        return set_error("\"radius\" must be positive");
    }
    d->radius = radius;

    for (unsigned i = 0; i < std::size(d->ps_num); ++i) {
        int ps_num = int64ToIntS(
            vsapi->propGetInt(in, "ps_num", i, &error));

        if (error) {
            ps_num = (i == 0) ? 2 : d->ps_num[i - 1];
        } else if (ps_num <= 0) {
            return set_error("\"ps_num\" must be positive");
        }

        d->ps_num[i] = ps_num;
    }

    for (unsigned i = 0; i < std::size(d->ps_range); ++i) {
        int ps_range = int64ToIntS(
            vsapi->propGetInt(in, "ps_range", i, &error));

        if (error) {
            ps_range = (i == 0) ? 4 : d->ps_range[i - 1];
        } else if (ps_range <= 0) {
            return set_error("\"ps_range\" must be positive");
        }

        d->ps_range[i] = ps_range;
    }

    bool chroma = !!vsapi->propGetInt(in, "chroma", 0, &error);
    if (error) {
        chroma = false;
    }
    if (chroma && d->vi->format->id != pfYUV444PS) {
        return set_error("clip format must be YUV444 when \"chroma\" is true");
    }
    d->chroma = chroma;

    if (radius == 0) {
        struct VSCoreInfo ci;
        vsapi->getCoreInfo2(core, &ci);
        auto num_threads = ci.numThreads;
        d->buffer.reserve(num_threads);
    }

    vsapi->createFilter(
        in, out, "BM3D",
        BM3DInit, BM3DGetFrame, BM3DFree,
        fmParallel, 0, d.release(), core);
}

VS_EXTERNAL_API(void) VapourSynthPluginInit(
    VSConfigPlugin configFunc, VSRegisterFunction registerFunc, VSPlugin *plugin
) {

    configFunc(
        "com.wolframrhodium.bm3dcpu", "bm3dcpu",
        "BM3D algorithm implemented in AVX and AVX2 intrinsics",
        VAPOURSYNTH_API_VERSION, 1, plugin);

    registerFunc("BM3D",
        "clip:clip;"
        "ref:clip:opt;"
        "sigma:float[]:opt;"
        "block_step:int[]:opt;"
        "bm_range:int[]:opt;"
        "radius:int:opt;"
        "ps_num:int:opt;"
        "ps_range:int:opt;"
        "chroma:int:opt;",
        BM3DCreate, nullptr, plugin);
}
