/**
 * @file        perf/counter.h
 * @brief       Performance counter registry and profiler
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */
#pragma once

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#ifdef REXGLUE_ENABLE_PROFILING
#include <tracy/Tracy.hpp>
#endif

namespace rex::perf {

enum class DrawBucket : uint8_t {
  kMainColorDepth,
  kDepthOnly,
  kCopyResolve,
  kMemexport,
  kNoPixelShader,
};

struct DrawFingerprint {
  DrawBucket bucket = DrawBucket::kMainColorDepth;
  uint64_t vertex_shader_hash = 0;
  uint64_t pixel_shader_hash = 0;
  uint32_t primitive_type = 0;
  uint32_t vertex_count = 0;
  uint32_t primitive_count = 0;
  static constexpr size_t kVertexFetchCount = 4;
  uint32_t vertex_fetch_address[kVertexFetchCount] = {};
  uint32_t vertex_fetch_size[kVertexFetchCount] = {};
};

struct DrawFingerprintEntry {
  DrawFingerprint fingerprint;
  uint64_t draw_count = 0;
  uint64_t vertices = 0;
  uint64_t primitives = 0;
};

enum class CounterId : uint16_t {
  // Frame
  kFrameTimeUs,
  kFps,

  // GPU
  kGuestDrawPackets,
  kDrawCalls,
  kCommandBufferStalls,
  kVerticesProcessed,
  kPrimitivesProcessed,
  kDrawMainCalls,
  kDrawMainVertices,
  kDrawMainPrimitives,
  kDrawDepthCalls,
  kDrawDepthVertices,
  kDrawDepthPrimitives,
  kDrawCopyCalls,
  kDrawCopyVertices,
  kDrawCopyPrimitives,
  kDrawMemexportCalls,
  kDrawMemexportVertices,
  kDrawMemexportPrimitives,
  kDrawNoPixelShaderCalls,
  kDrawNoPixelShaderVertices,
  kDrawNoPixelShaderPrimitives,
  kDrawStageTotalUs,
  kDrawStagePrimitiveUs,
  kDrawStageRenderTargetUs,
  kDrawStagePipelineUs,
  kDrawStageTextureUs,
  kDrawStageFixedFunctionUs,
  kDrawStageBindingsUs,
  kDrawStageVertexBuffersUs,
  kDrawStageBarriersUs,
  kDrawStageSubmitUs,
  kGpuMainUs,
  kGpuDepthUs,
  kGpuCopyUs,
  kGpuMemexportUs,
  kGpuNoPixelShaderUs,
  kGpuTimestampedDraws,
  kGpuCommandProcessorFrameUs,
  kGpuTimestampedFrames,
  kGpuBarrierUs,
  kGpuClearUs,
  kGpuCopyBufferUs,
  kGpuCopyTextureUs,
  kGpuCopyResourceUs,
  kGpuDispatchUs,
  kGpuResolveQueryUs,
  kCpuPrimaryBufferUs,
  kCpuIndirectBufferUs,
  kCpuD3D12BeginSubmissionUs,
  kCpuD3D12BeginSubmissionFenceUs,
  kCpuD3D12BeginSubmissionFrameOpenUs,
  kCpuD3D12BeginSubmissionOpenUs,
  kCpuD3D12ProcessGpuTimestampsUs,
  kCpuD3D12EndSubmissionUs,
  kCpuD3D12CheckFenceUs,
  kCpuD3D12FenceWaitUs,
  kCpuD3D12FenceReclaimUs,
  kCpuD3D12EndFrameUs,
  kCpuD3D12PipelineEndUs,
  kCpuD3D12SubmitBarriersUs,
  kCpuD3D12DeferredExecuteUs,
  kCpuD3D12CommandListCloseUs,
  kCpuD3D12ExecuteCommandListsUs,
  kCpuD3D12SignalUs,
  kCpuD3D12PaintTotalUs,
  kCpuD3D12PaintConsumeUs,
  kCpuD3D12PaintRecordUs,
  kCpuD3D12PaintUiUs,
  kCpuD3D12PresentWaitUs,
  kCpuD3D12PresentUs,
  kTextureRequestUs,
  kTextureRequestCalls,
  kTextureFetchesRequested,
  kTextureBindingsChanged,
  kTexturePendingLoads,
  kTexturePendingRanges,
  kTexturePendingBytes,
  kTextureSharedMemoryRequestUs,
  kTextureCommitLoadUs,
  kTextureLoadsCommitted,
  kTextureLoadBytes,
  kTextureLoadBackendUs,
  kTextureScaledResolveCommitUs,
  kD3D12BarriersQueued,
  kD3D12BarriersSubmitted,
  kD3D12TransitionBarriers,
  kD3D12AliasingBarriers,
  kD3D12UavBarriers,
  kD3D12RtTransitions,
  kD3D12DepthTransitions,
  kD3D12CopyTransitions,
  kD3D12SrvTransitions,
  kD3D12UavTransitions,
  kD3D12PresentTransitions,
  kDeferredCommands,
  kDeferredClearCount,
  kDeferredClearUs,
  kDeferredBarrierCount,
  kDeferredBarrierUs,
  kDeferredCopyBufferCount,
  kDeferredCopyBufferBytes,
  kDeferredCopyBufferUs,
  kDeferredCopyTextureCount,
  kDeferredCopyTextureUs,
  kDeferredCopyResourceCount,
  kDeferredCopyResourceUs,
  kDeferredDispatchCount,
  kDeferredDispatchUs,
  kDeferredDrawCount,
  kDeferredDrawUs,
  kDeferredQueryCount,
  kDeferredQueryUs,
  kDeferredResolveQueryCount,
  kDeferredResolveQueryUs,
  kDeferredStateCount,
  kDeferredStateUs,
  kRtResolveCalls,
  kRtResolveUs,
  kRtResolveDirectCalls,
  kRtResolveDirectUs,
  kRtResolveTransferClearUs,
  kRtResolveReadbackCalls,
  kRtResolveReadbackUs,

  // Audio
  kXmaFramesDecoded,
  kAudioFrameLatencyUs,
  kBufferQueueDepth,

  // Dispatch
  kFunctionsDispatched,
  kInterruptDispatches,

  // Threading
  kActiveThreads,
  kApcQueueDepth,
  kCriticalRegionContentions,

  // Caches
  kTextureCacheHits,
  kTextureCacheMisses,
  kPipelineCacheHits,
  kPipelineCacheMisses,

  kCount  // sentinel -- must be last
};

#ifdef REXGLUE_ENABLE_PERF_COUNTERS

// Returns human-readable name for a counter (e.g. "frame_time_us")
const char* CounterName(CounterId id);

// Set a counter to an absolute value
void SetCounter(CounterId id, int64_t value);

// Atomically add to a counter
void IncrementCounter(CounterId id, int64_t delta = 1);

// Record one draw-like operation in a coarse render bucket for view comparisons.
void RecordDrawBucket(DrawBucket bucket, int64_t vertices, int64_t primitives);

// Aggregate one submitted draw for the last completed frame's fingerprint dump.
void RecordDrawFingerprint(const DrawFingerprint& fingerprint);

// Return a copy of the last completed frame's aggregated fingerprints.
std::vector<DrawFingerprintEntry> GetDrawFingerprintSnapshot();

// Save the last completed frame's aggregated fingerprints. Returns the written
// path, or std::nullopt if the file couldn't be opened.
std::optional<std::filesystem::path> SaveDrawFingerprintSnapshot(const std::filesystem::path& path);

// Save the last completed frame's counter snapshot as name,value CSV rows.
// Returns the written path, or std::nullopt if the file couldn't be opened.
std::optional<std::filesystem::path> SaveCounterSnapshot(const std::filesystem::path& path);

// Start a multi-frame capture. Counter totals and average-per-frame values are
// written after the requested frame count plus a short drain for delayed GPU
// timestamp query results. Returns false if another capture is already active.
bool StartCapture(const std::filesystem::path& counters_path,
                  const std::filesystem::path& fingerprints_path);

bool IsCaptureRecording();

// True when always-on heavyweight draw diagnostics are enabled, or while an
// explicit F8 capture is recording.
bool ShouldCollectDrawDiagnostics();

// True when callsites should build and record per-draw fingerprints.
bool ShouldCollectDrawFingerprints();

class ScopedCounterTimer {
 public:
  explicit ScopedCounterTimer(CounterId counter_id,
                              bool active = ShouldCollectDrawDiagnostics())
      : counter_id_(counter_id), active_(active) {
    if (active_) {
      start_ = Clock::now();
    }
  }

  ~ScopedCounterTimer() {
    Stop();
  }

  void Stop() {
    if (!active_) {
      return;
    }
    auto elapsed_us =
        std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - start_).count();
    IncrementCounter(counter_id_, elapsed_us);
    active_ = false;
  }

  ScopedCounterTimer(const ScopedCounterTimer&) = delete;
  ScopedCounterTimer& operator=(const ScopedCounterTimer&) = delete;

 private:
  using Clock = std::chrono::steady_clock;
  CounterId counter_id_;
  bool active_;
  Clock::time_point start_;
};

// Read a counter's current live value
int64_t GetCounter(CounterId id);

// Snapshot current values into the read buffer and zero the live counters.
// Called once per frame by Profiler::Flip().
void ResetFrameCounters();

// Read a counter from the last-frame snapshot (stable between frames).
int64_t GetSnapshotCounter(CounterId id);

// Initialize the counter system (zeroes everything). Safe to call multiple times.
void Init();

// CSV logging
void SetCsvLogPath(const std::string& path);
void WriteCsvFrame();
void FlushCsv();

#else

inline const char* CounterName(CounterId id) {
  (void)id;
  return "";
}

inline void SetCounter(CounterId id, int64_t value) {
  (void)id;
  (void)value;
}

inline void IncrementCounter(CounterId id, int64_t delta = 1) {
  (void)id;
  (void)delta;
}

inline void RecordDrawBucket(DrawBucket bucket, int64_t vertices, int64_t primitives) {
  (void)bucket;
  (void)vertices;
  (void)primitives;
}

inline void RecordDrawFingerprint(const DrawFingerprint& fingerprint) {
  (void)fingerprint;
}

inline std::vector<DrawFingerprintEntry> GetDrawFingerprintSnapshot() {
  return {};
}

inline std::optional<std::filesystem::path> SaveDrawFingerprintSnapshot(
    const std::filesystem::path& path) {
  (void)path;
  return std::nullopt;
}

inline std::optional<std::filesystem::path> SaveCounterSnapshot(
    const std::filesystem::path& path) {
  (void)path;
  return std::nullopt;
}

inline bool StartCapture(const std::filesystem::path& counters_path,
                         const std::filesystem::path& fingerprints_path) {
  (void)counters_path;
  (void)fingerprints_path;
  return false;
}

inline bool IsCaptureRecording() {
  return false;
}

inline bool ShouldCollectDrawDiagnostics() {
  return false;
}

inline bool ShouldCollectDrawFingerprints() {
  return false;
}

class ScopedCounterTimer {
 public:
  explicit ScopedCounterTimer(CounterId counter_id, bool active = false) {
    (void)counter_id;
    (void)active;
  }
  void Stop() {}

  ScopedCounterTimer(const ScopedCounterTimer&) = delete;
  ScopedCounterTimer& operator=(const ScopedCounterTimer&) = delete;
};

inline int64_t GetCounter(CounterId id) {
  (void)id;
  return 0;
}

inline void ResetFrameCounters() {}

inline int64_t GetSnapshotCounter(CounterId id) {
  (void)id;
  return 0;
}

inline void Init() {}

inline void SetCsvLogPath(const std::string& path) {
  (void)path;
}

inline void WriteCsvFrame() {}

inline void FlushCsv() {}

#endif

// Profiler -- coordinates Tracy frame marks and counter snapshots.
// Moved here from rex::debug to consolidate all perf code under rex::perf.
class Profiler {
 public:
  // Call once at runtime startup to enable Tracy's network threads.
  // CLI tools should skip this to avoid socket listeners entirely.
  static void Startup() {
#ifdef REXGLUE_ENABLE_PROFILING
    tracy::StartupProfiler();
#endif
  }
  static void OnThreadEnter(const char* name = nullptr) {
#ifdef REXGLUE_ENABLE_PROFILING
    if (name)
      tracy::SetThreadName(name);
#else
    (void)name;
#endif
  }
  static void OnThreadExit() {}
  static void ThreadEnter(const char* name = nullptr) { OnThreadEnter(name); }
  static void ThreadExit() {}
  static void Flip() {
#ifdef REXGLUE_ENABLE_PROFILING
    FrameMark;
#endif
#ifdef REXGLUE_ENABLE_PERF_COUNTERS
    ResetFrameCounters();
    WriteCsvFrame();
#endif
  }
  static void Flush() {}
  static void Shutdown() {
#ifdef REXGLUE_ENABLE_PROFILING
    tracy::ShutdownProfiler();
#endif
#ifdef REXGLUE_ENABLE_PERF_COUNTERS
    FlushCsv();
#endif
  }
  static bool is_enabled() {
#ifdef REXGLUE_ENABLE_PROFILING
    return tracy::IsProfilerStarted();
#else
    return false;
#endif
  }
};

}  // namespace rex::perf

// Perf counter macros -- compile to no-ops when counters are disabled.
#ifdef REXGLUE_ENABLE_PERF_COUNTERS

// Generic helpers for easily adding new counters
#define PERF_counter_set(id, value) rex::perf::SetCounter(rex::perf::CounterId::id, value)
#define PERF_counter_inc(id) rex::perf::IncrementCounter(rex::perf::CounterId::id)
#define PERF_counter_add(id, delta) rex::perf::IncrementCounter(rex::perf::CounterId::id, delta)

// Purpose-specific macros so callsites stay clean
#define PROFILE_FRAME_TIME_US(value) PERF_counter_set(kFrameTimeUs, value)
#define PROFILE_FPS(value) PERF_counter_set(kFps, value)
#define PROFILE_FUNCTION_DISPATCHED() PERF_counter_inc(kFunctionsDispatched)
#define PROFILE_INTERRUPT_DISPATCHED() PERF_counter_inc(kInterruptDispatches)
#define PROFILE_XMA_FRAME_DECODED() PERF_counter_inc(kXmaFramesDecoded)
#define PROFILE_GUEST_DRAW_PACKET() PERF_counter_inc(kGuestDrawPackets)
#define PROFILE_DRAW_CALL() PERF_counter_inc(kDrawCalls)
#define PROFILE_VERTICES(n) PERF_counter_add(kVerticesProcessed, n)
#define PROFILE_PRIMITIVES(n) PERF_counter_add(kPrimitivesProcessed, n)
#define PROFILE_DRAW_BUCKET(bucket, vertices, primitives) \
  rex::perf::RecordDrawBucket(bucket, vertices, primitives)
#define PROFILE_DRAW_FINGERPRINT(fingerprint) rex::perf::RecordDrawFingerprint(fingerprint)
#define PROFILE_DRAW_STAGE_US(id, delta) rex::perf::IncrementCounter((id), (delta))
#define PROFILE_GPU_TIMESTAMPED_DRAW() PERF_counter_inc(kGpuTimestampedDraws)
#define REX_PERF_CONCAT_INNER(a, b) a##b
#define REX_PERF_CONCAT(a, b) REX_PERF_CONCAT_INNER(a, b)
#define PROFILE_SCOPE_COUNTER(id) \
  rex::perf::ScopedCounterTimer REX_PERF_CONCAT(perf_scope_timer_, __LINE__)(rex::perf::CounterId::id)
#define PROFILE_SCOPE_COUNTER_IF(id, active) \
  rex::perf::ScopedCounterTimer REX_PERF_CONCAT(perf_scope_timer_, __LINE__)( \
      rex::perf::CounterId::id, active)
#define PROFILE_CMD_BUFFER_STALL() PERF_counter_inc(kCommandBufferStalls)
#define PROFILE_AUDIO_LATENCY_US(value) PERF_counter_set(kAudioFrameLatencyUs, value)
#define PROFILE_BUFFER_QUEUE_DEPTH(value) PERF_counter_set(kBufferQueueDepth, value)
#define PROFILE_THREAD_CREATED() PERF_counter_inc(kActiveThreads)
#define PROFILE_THREAD_EXITED() PERF_counter_add(kActiveThreads, -1)
#define PROFILE_APC_QUEUE_DEPTH(value) PERF_counter_set(kApcQueueDepth, value)
#define PROFILE_CRITICAL_REGION_CONTENTION() PERF_counter_inc(kCriticalRegionContentions)
#define PROFILE_TEXTURE_CACHE_HIT() PERF_counter_inc(kTextureCacheHits)
#define PROFILE_TEXTURE_CACHE_MISS() PERF_counter_inc(kTextureCacheMisses)
#define PROFILE_PIPELINE_CACHE_HIT() PERF_counter_inc(kPipelineCacheHits)
#define PROFILE_PIPELINE_CACHE_MISS() PERF_counter_inc(kPipelineCacheMisses)

#else

#define PERF_counter_set(id, value)
#define PERF_counter_inc(id)
#define PERF_counter_add(id, delta)

#define PROFILE_FRAME_TIME_US(value)
#define PROFILE_FPS(value)
#define PROFILE_FUNCTION_DISPATCHED()
#define PROFILE_INTERRUPT_DISPATCHED()
#define PROFILE_XMA_FRAME_DECODED()
#define PROFILE_GUEST_DRAW_PACKET()
#define PROFILE_DRAW_CALL()
#define PROFILE_VERTICES(n)
#define PROFILE_PRIMITIVES(n)
#define PROFILE_DRAW_BUCKET(bucket, vertices, primitives)
#define PROFILE_DRAW_FINGERPRINT(fingerprint)
#define PROFILE_DRAW_STAGE_US(id, delta)
#define PROFILE_GPU_TIMESTAMPED_DRAW()
#define PROFILE_SCOPE_COUNTER(id)
#define PROFILE_SCOPE_COUNTER_IF(id, active)
#define PROFILE_CMD_BUFFER_STALL()
#define PROFILE_AUDIO_LATENCY_US(value)
#define PROFILE_BUFFER_QUEUE_DEPTH(value)
#define PROFILE_THREAD_CREATED()
#define PROFILE_THREAD_EXITED()
#define PROFILE_APC_QUEUE_DEPTH(value)
#define PROFILE_CRITICAL_REGION_CONTENTION()
#define PROFILE_TEXTURE_CACHE_HIT()
#define PROFILE_TEXTURE_CACHE_MISS()
#define PROFILE_PIPELINE_CACHE_HIT()
#define PROFILE_PIPELINE_CACHE_MISS()

#endif
