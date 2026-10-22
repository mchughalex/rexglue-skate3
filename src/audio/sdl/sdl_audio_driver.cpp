/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2020 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 */

#include <algorithm>
#include <array>
#include <chrono>
#include <cstring>
#include <string>

#include <rex/assert.h>
#include <rex/audio/conversion.h>
#include <rex/audio/flags.h>
#include <rex/audio/sdl/sdl_audio_driver.h>
#include <rex/cvar.h>
#include <rex/dbg.h>
#include <rex/logging.h>
#include <rex/perf/counter.h>
#include <SDL3/SDL.h>
#include <rex/platform.h>

#include <algorithm>
#include <chrono>
#if REX_PLATFORM_LINUX
#include <pthread.h>
#endif

REXCVAR_DEFINE_BOOL(audio_mute, false, "Audio", "Mute audio output");

REXCVAR_DEFINE_BOOL(audio_realtime_credit_pacing, true, "Audio",
                    "Limit the rate at which consumed audio frames release guest submission "
                    "credits to slightly above real time. Prevents a misbehaving audio device "
                    "(e.g. a Bluetooth headset in a broken state) from running the guest audio "
                    "clock - and anything synchronized to it, like video playback - several "
                    "times too fast.")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_BOOL(audio_stats, false, "Audio",
                    "Log a 5-second audio pipeline stats line: device callback rate/gaps, "
                    "frames consumed vs real time, silence insertions (underruns), queue "
                    "depth and pacing state. Turn on when diagnosing robotic/slowed/"
                    "crackling audio reports.")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_BOOL(audio_steady_credit_release, true, "Audio",
                    "Hand frames back to the guest's audio engine at a steady one-per-5.33ms "
                    "cadence, like the console's audio hardware, instead of in one burst per "
                    "device callback. With a large device buffer (PipeWire quantum 1024+, "
                    "Bluetooth, HDMI) the burst made the game mix several frames back to back, "
                    "and its streamed music could not refill in time: the music skipped and "
                    "drifted off-tempo whenever sound effects played. Falls back to a burst "
                    "only when the device asks for more than is queued.")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_INT32(audio_device_sample_frames, 0, "Audio",
                     "Requested audio device buffer size in sample frames (0 = backend default). "
                     "Larger values (e.g. 1024 or 2048) add latency but tolerate scheduling "
                     "hiccups better - try this against crackling, especially on Linux.")
    .range(0, 8192);

namespace {

uint64_t AudioNowNs() {
  return uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
                      std::chrono::steady_clock::now().time_since_epoch())
                      .count());
}

}  // namespace

namespace rex::audio::sdl {

SDLAudioDriver::SDLAudioDriver(memory::Memory* memory, rex::thread::Semaphore* semaphore)
    : AudioDriver(memory), semaphore_(semaphore) {}

SDLAudioDriver::~SDLAudioDriver() {
  StopSteadyPacer();
  assert_true(frames_queued_.empty());
  assert_true(frames_unused_.empty());
}

bool SDLAudioDriver::Initialize() {
  // Prevent SDL from interfering with timer resolution (causes FPS drops)
  SDL_SetHintWithPriority(SDL_HINT_TIMER_RESOLUTION, "0", SDL_HINT_OVERRIDE);

  // Set audio category for proper OS audio handling
  SDL_SetHint(SDL_HINT_AUDIO_CATEGORY, "playback");

  // Set app name for audio device identification
  SDL_SetAppMetadataProperty(SDL_PROP_APP_METADATA_NAME_STRING, "rexglue");

  int32_t requested_sample_frames = REXCVAR_GET(audio_device_sample_frames);
  if (requested_sample_frames > 0) {
    SDL_SetHint(SDL_HINT_AUDIO_DEVICE_SAMPLE_FRAMES,
                std::to_string(requested_sample_frames).c_str());
  }

  if (!SDL_InitSubSystem(SDL_INIT_AUDIO)) {
    REXAPU_ERROR("SDL_InitSubSystem(SDL_INIT_AUDIO) failed: {}", SDL_GetError());
    return false;
  }
  sdl_initialized_ = true;

  SDL_AudioSpec desired_spec = {};
  SDL_AudioSpec obtained_spec = {};
  desired_spec.freq = frame_frequency_;
  desired_spec.format = SDL_AUDIO_F32LE;
  desired_spec.channels = frame_channels_;
  sdl_device_channels_ = frame_channels_;
  sdl_stream_ = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &desired_spec,
                                          SDLCallback, this);
  if (!sdl_stream_) {
    REXAPU_ERROR("SDL_OpenAudioDeviceStream() failed: {}", SDL_GetError());
    return false;
  }

  SDL_AudioDeviceID sdl_device = SDL_GetAudioStreamDevice(sdl_stream_);
  if (!sdl_device) {
    REXAPU_ERROR("SDL_GetAudioStreamDevice() failed: {}", SDL_GetError());
    return false;
  }

  if (!SDL_GetAudioDeviceFormat(sdl_device, &obtained_spec, NULL)) {
    REXAPU_WARN("SDL_GetAudioDeviceFormat() failed: {}", SDL_GetError());
    obtained_spec = desired_spec;
  }

  if (obtained_spec.channels == 2) {
    SDL_DestroyAudioStream(sdl_stream_);
    sdl_stream_ = nullptr;
    desired_spec.channels = 2;
    sdl_device_channels_ = 2;
    sdl_stream_ = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &desired_spec,
                                            SDLCallback, this);
    if (!sdl_stream_) {
      REXAPU_ERROR("SDL_OpenAudioDeviceStream() stereo fallback failed: {}", SDL_GetError());
      return false;
    }
    sdl_device = SDL_GetAudioStreamDevice(sdl_stream_);
    if (!sdl_device) {
      REXAPU_ERROR("SDL_GetAudioStreamDevice() failed after stereo fallback: {}", SDL_GetError());
      return false;
    }
  }

  StartSteadyPacer();

  if (!SDL_ResumeAudioDevice(sdl_device)) {
    REXAPU_ERROR("SDL_ResumeAudioDevice() failed: {}", SDL_GetError());
    return false;
  }

  // Publish the buffer length the backend chose so the settings UI can show
  // what "auto" resolves to, only meaningful when no explicit size was
  // requested (the device otherwise just reflects the request). Queried
  // after the stereo fallback so it reflects the device actually in use.
  if (requested_sample_frames <= 0) {
    SDL_AudioSpec active_spec = {};
    int device_sample_frames = 0;
    if (SDL_GetAudioDeviceFormat(sdl_device, &active_spec, &device_sample_frames) &&
        device_sample_frames > 0) {
      SetAutoDeviceSampleFrames(device_sample_frames);
    }
  }

  return true;
}

void SDLAudioDriver::SubmitFrame(uint32_t frame_ptr) {
  const auto input_frame = memory_->TranslateVirtual<float*>(frame_ptr);
  float* output_frame;
  {
    std::unique_lock<std::mutex> guard(frames_mutex_);
    if (frames_unused_.empty()) {
      output_frame = new float[frame_samples_];
    } else {
      output_frame = frames_unused_.top();
      frames_unused_.pop();
    }
  }

  std::memcpy(output_frame, input_frame, frame_samples_ * sizeof(float));

  static uint32_t sdl_submit_count = 0;
  if (sdl_submit_count < 10) {
    std::unique_lock<std::mutex> guard(frames_mutex_);
    REXAPU_DEBUG("SDLAudioDriver::SubmitFrame: frame_ptr={:08X} queued_count={}", frame_ptr,
                 frames_queued_.size() + 1);
    sdl_submit_count++;
  }

  {
    std::unique_lock<std::mutex> guard(frames_mutex_);
    frames_queued_.push(output_frame);
    PROFILE_BUFFER_QUEUE_DEPTH(static_cast<int64_t>(frames_queued_.size()));
  }
  frames_available_cv_.notify_one();
}

void SDLAudioDriver::Shutdown() {
  StopSteadyPacer();
  if (sdl_stream_) {
    SDL_DestroyAudioStream(sdl_stream_);
    sdl_stream_ = nullptr;
  }
  if (sdl_initialized_) {
    SDL_QuitSubSystem(SDL_INIT_AUDIO);
    sdl_initialized_ = false;
  }
  std::unique_lock<std::mutex> guard(frames_mutex_);
  while (!frames_unused_.empty()) {
    delete[] frames_unused_.top();
    frames_unused_.pop();
  }
  while (!frames_queued_.empty()) {
    delete[] frames_queued_.front();
    frames_queued_.pop();
  }
}

void SDLAudioDriver::SDLCallback(void* userdata, SDL_AudioStream* stream, int additional_amount,
                                 [[maybe_unused]] int total_amount) {
  SCOPE_profile_cpu_f("apu");
  if (!userdata || !stream) {
    REXAPU_ERROR("SDLAudioDriver::SDLCallback called with nullptr.");
    return;
  }
  const auto driver = static_cast<SDLAudioDriver*>(userdata);
  const int sample_count =
      static_cast<int>(channel_samples_ * std::max<uint8_t>(driver->sdl_device_channels_, 1));
  const int len = static_cast<int>(sizeof(float) * sample_count);
  float* data = SDL_stack_alloc(float, sample_count);
  if (!data) {
    REXAPU_ERROR("SDLAudioDriver::SDLCallback failed to allocate {} samples", sample_count);
    return;
  }
  const bool stats_enabled = REXCVAR_GET(audio_stats);
  if (stats_enabled) {
    const uint64_t now_ns = AudioNowNs();
    if (!driver->stats_window_start_ns_) {
      driver->stats_window_start_ns_ = now_ns;
    }
    if (driver->stats_last_callback_ns_) {
      driver->stats_max_callback_gap_ns_ =
          std::max(driver->stats_max_callback_gap_ns_, now_ns - driver->stats_last_callback_ns_);
    }
    driver->stats_last_callback_ns_ = now_ns;
    driver->stats_callbacks_++;
    {
      std::unique_lock<std::mutex> guard(driver->frames_mutex_);
      const size_t depth = driver->frames_queued_.size();
      driver->stats_queue_min_ = std::min(driver->stats_queue_min_, depth);
      driver->stats_queue_max_ = std::max(driver->stats_queue_max_, depth);
    }
  }
  // Bounded budget for waiting on frames the guest owes us: a large device
  // quantum (PipeWire force-quantum / Bluetooth / HDMI sinks) can request more
  // audio per callback than the credit pool depth, so the queue legitimately
  // runs dry mid-request while the guest is mixing the frames we just
  // credited. Waiting a few ms for those beats splicing silence. Scaled to
  // half the request duration so small-buffer backends never risk their own
  // deadline; only spent when credits were released this callback (an idle
  // guest - loading screens, shutdown - never engages it).
  const int64_t request_chunks = (int64_t(additional_amount) + len - 1) / len;
  int64_t wait_budget_ns =
      std::min<int64_t>(15000000, request_chunks * int64_t(channel_samples_) * 1000000000ll /
                                      int64_t(frame_frequency_) / 2);
  const bool steady = REXCVAR_GET(audio_steady_credit_release);
  // Grant credits deferred by the real-time pacer once enough time has passed.
  // In steady mode the pacer thread owns the release; nothing happens here
  // unless the queue runs dry (see below).
  uint64_t credits_released = steady ? 0 : driver->ReleasePacedCredits(0);
  while (additional_amount > 0) {
    static uint32_t sdl_callback_count = 0;
    float* buffer = nullptr;
    {
      std::unique_lock<std::mutex> guard(driver->frames_mutex_);
      if (driver->frames_queued_.empty() && steady) {
        // The device wants more than is queued: its buffer is larger than
        // the guest's credit pool, or the guest stalled. Hand over everything
        // still owed at once and wait for it, as the burst path always did.
        const uint64_t owed = driver->TakeSteadyCredits(UINT64_MAX);
        if (owed) {
          if (driver->semaphore_->Release(int(owed), nullptr)) {
            credits_released += owed;
            driver->stats_steady_emergency_.fetch_add(1, std::memory_order_relaxed);
          } else {
            driver->steady_pending_.fetch_add(owed, std::memory_order_relaxed);
          }
        }
      }
      if (driver->frames_queued_.empty() && credits_released && wait_budget_ns > 0) {
        const auto wait_start = std::chrono::steady_clock::now();
        driver->frames_available_cv_.wait_for(guard, std::chrono::nanoseconds(wait_budget_ns),
                                              [&] { return !driver->frames_queued_.empty(); });
        wait_budget_ns -= std::chrono::duration_cast<std::chrono::nanoseconds>(
                              std::chrono::steady_clock::now() - wait_start)
                              .count();
      }
      if (!driver->frames_queued_.empty()) {
        buffer = driver->frames_queued_.front();
        driver->frames_queued_.pop();
        PROFILE_BUFFER_QUEUE_DEPTH(static_cast<int64_t>(driver->frames_queued_.size()));
      }
    }

    if (!buffer) {
      if (sdl_callback_count < 10) {
        REXAPU_DEBUG("SDLCallback: no frames queued (silence)");
        sdl_callback_count++;
      }
      if (stats_enabled) {
        driver->stats_silence_chunks_++;
      }
      std::memset(data, 0, len);
      if (!SDL_PutAudioStreamData(stream, data, len)) {
        REXAPU_ERROR("SDL_PutAudioStreamData() failed while filling silence: {}", SDL_GetError());
        break;
      }
      additional_amount -= len;
    } else {
      if (REXCVAR_GET(audio_mute)) {
        std::memset(data, 0, len);
      } else {
        switch (driver->sdl_device_channels_) {
          case 2:
            conversion::sequential_6_BE_to_interleaved_2_LE(data, buffer, channel_samples_);
            break;
          case 6:
            conversion::sequential_6_BE_to_interleaved_6_LE(data, buffer, channel_samples_);
            break;
          default:
            assert_unhandled_case(driver->sdl_device_channels_);
            break;
        }
      }
      bool submitted = SDL_PutAudioStreamData(stream, data, len);
      {
        std::unique_lock<std::mutex> guard(driver->frames_mutex_);
        driver->frames_unused_.push(buffer);
      }
      if (!submitted) {
        REXAPU_ERROR("SDL_PutAudioStreamData() failed: {}", SDL_GetError());
        break;
      }

      if (steady) {
        driver->steady_pending_.fetch_add(1, std::memory_order_relaxed);
      } else {
        credits_released += driver->ReleasePacedCredits(1);
      }
      if (stats_enabled) {
        driver->stats_frames_++;
      }
      additional_amount -= len;
    }
  }

  if (stats_enabled) {
    const uint64_t now_ns = AudioNowNs();
    const uint64_t window_ns = now_ns - driver->stats_window_start_ns_;
    if (window_ns >= 5000000000ull) {
      const double window_s = double(window_ns) * 1e-9;
      REXAPU_INFO(
          "Audio stats ({:.1f}s): callbacks={} frames={} ({:.1f}/s, real-time={:.1f}/s) "
          "silence_chunks={} queue_depth={}..{} deferred_credits={} allowance={:.1f} "
          "max_callback_gap={}ms steady={} steady_pending={} steady_bursts={}",
          window_s, driver->stats_callbacks_, driver->stats_frames_,
          double(driver->stats_frames_) / window_s,
          double(frame_frequency_) / double(channel_samples_), driver->stats_silence_chunks_,
          driver->stats_queue_min_ == SIZE_MAX ? 0 : driver->stats_queue_min_,
          driver->stats_queue_max_, driver->pace_deferred_credits_, driver->pace_allowance_frames_,
          driver->stats_max_callback_gap_ns_ / 1000000ull, steady,
          driver->steady_pending_.load(std::memory_order_relaxed),
          driver->stats_steady_emergency_.exchange(0, std::memory_order_relaxed));
      driver->stats_window_start_ns_ = now_ns;
      driver->stats_callbacks_ = 0;
      driver->stats_frames_ = 0;
      driver->stats_silence_chunks_ = 0;
      driver->stats_queue_min_ = SIZE_MAX;
      driver->stats_queue_max_ = 0;
      driver->stats_max_callback_gap_ns_ = 0;
    }
  }

  SDL_stack_free(data);
}

uint64_t SDLAudioDriver::ReleasePacedCredits(uint32_t new_credits) {
  pace_deferred_credits_ += new_credits;
  if (!pace_deferred_credits_) {
    return 0;
  }
  uint64_t releasable = pace_deferred_credits_;
  if (REXCVAR_GET(audio_realtime_credit_pacing)) {
    const uint64_t now_ns = AudioNowNs();
    if (pace_last_ns_) {
      // 1% above real time so a device clock running slightly faster than the
      // host clock can't slowly drain the pipeline; the allowance cap keeps
      // catch-up bursts after pauses or stalls to a fraction of the guest's
      // credit pool instead of an unbounded replay.
      pace_allowance_frames_ += double(now_ns - pace_last_ns_) * 1e-9 *
                                (double(frame_frequency_) / double(channel_samples_)) * 1.01;
    }
    pace_last_ns_ = now_ns;
    // The bank must cover the largest burst a HEALTHY device consumes in one
    // callback cluster, or steady-state playback is throttled below real time
    // (the guest audio clock slows down and the queue underruns into
    // interleaved 5.3ms silence chunks - reported as "robotic"/"slowed down"
    // audio). PipeWire fires the callback once per graph quantum, which
    // force-quantum / Bluetooth / HDMI sinks can raise to 8192 device frames
    // (~170ms = 32 guest frames); loaded low-spec machines can delay the
    // audio thread similarly. 64 frames covers those with margin. Runaway
    // devices are still bounded by the 1.01x real-time accrual above - the
    // bank only sizes the transient catch-up burst after a stall (<=340ms of
    // guest audio clock, and the release is further clamped by the credit
    // pool depth).
    constexpr double kMaxBankedFrames = 64.0;
    pace_allowance_frames_ = std::min(pace_allowance_frames_, kMaxBankedFrames);
    releasable = std::min(releasable, uint64_t(pace_allowance_frames_));
  }
  if (releasable) {
    auto ret = semaphore_->Release(int(releasable), nullptr);
    assert_true(ret);
    if (!ret) {
      return 0;
    }
    pace_deferred_credits_ -= releasable;
    pace_allowance_frames_ -= double(releasable);
    if (pace_allowance_frames_ < 0.0) {
      pace_allowance_frames_ = 0.0;
    }
  }
  return releasable;
}

uint64_t SDLAudioDriver::TakeSteadyCredits(uint64_t max_credits) {
  uint64_t cur = steady_pending_.load(std::memory_order_relaxed);
  uint64_t take = 0;
  do {
    take = std::min(cur, max_credits);
    if (!take) {
      return 0;
    }
  } while (!steady_pending_.compare_exchange_weak(cur, cur - take, std::memory_order_acq_rel,
                                                  std::memory_order_relaxed));
  return take;
}

void SDLAudioDriver::StartSteadyPacer() {
  std::lock_guard<std::mutex> lock(steady_mutex_);
  if (steady_running_) {
    return;
  }
  steady_running_ = true;
  steady_thread_ = std::thread([this] { SteadyPacerThreadMain(); });
}

void SDLAudioDriver::StopSteadyPacer() {
  {
    std::lock_guard<std::mutex> lock(steady_mutex_);
    steady_running_ = false;
  }
  steady_cv_.notify_all();
  if (steady_thread_.joinable()) {
    steady_thread_.join();
  }
}

void SDLAudioDriver::SteadyPacerThreadMain() {
#if REX_PLATFORM_LINUX
  pthread_setname_np(pthread_self(), "Audio Pacer");
#endif
  using clock = std::chrono::steady_clock;
  // One guest frame period, 0.5% short: the pacer can never release more than
  // the device consumed (it only hands back owed credits), so running a hair
  // fast only guarantees it cannot fall behind a device clock that runs fast.
  const auto period = std::chrono::nanoseconds(
      int64_t(1e9 * double(channel_samples_) / double(frame_frequency_) * 0.995));
  // Bounds the catch-up after this thread was delayed, so a late wake-up
  // cannot turn back into the burst this pacer exists to remove.
  constexpr int64_t kMaxCreditsPerWake = 4;
  constexpr int64_t kResyncAfterPeriods = 20;

  auto next = clock::now() + period;
  std::unique_lock<std::mutex> lock(steady_mutex_);
  while (steady_running_) {
    steady_cv_.wait_until(lock, next, [this] { return !steady_running_; });
    if (!steady_running_) {
      break;
    }
    const auto now = clock::now();
    if (!REXCVAR_GET(audio_steady_credit_release)) {
      // Switched off at runtime: return anything still owed so the guest is
      // not left short, then idle.
      if (const uint64_t owed = TakeSteadyCredits(UINT64_MAX)) {
        if (!semaphore_->Release(int(owed), nullptr)) {
          steady_pending_.fetch_add(owed, std::memory_order_relaxed);
        }
      }
      next = now + period;
      continue;
    }
    int64_t due = 1;
    if (now > next) {
      due += (now - next) / period;
    }
    if (due > kResyncAfterPeriods) {
      next = now;
      due = 1;
    }
    if (const uint64_t take = TakeSteadyCredits(uint64_t(std::min(due, kMaxCreditsPerWake)))) {
      if (!semaphore_->Release(int(take), nullptr)) {
        steady_pending_.fetch_add(take, std::memory_order_relaxed);
      }
    }
    next += period * due;
  }
}

}  // namespace rex::audio::sdl
