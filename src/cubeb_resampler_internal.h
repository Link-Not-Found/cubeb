/*
 * Copyright © 2016 Mozilla Foundation
 *
 * This program is made available under an ISC-style license.  See the
 * accompanying file LICENSE for details.
 */

#if !defined(CUBEB_RESAMPLER_INTERNAL)
#define CUBEB_RESAMPLER_INTERNAL

#include <algorithm>
#include <cassert>
#include <cmath>
#include <memory>
#ifdef CUBEB_GECKO_BUILD
#include "mozilla/UniquePtr.h"
// In libc++, symbols such as std::unique_ptr may be defined in std::__1.
// The _LIBCPP_BEGIN_NAMESPACE_STD and _LIBCPP_END_NAMESPACE_STD macros
// will expand to the correct namespace.
#ifdef _LIBCPP_BEGIN_NAMESPACE_STD
#define MOZ_BEGIN_STD_NAMESPACE _LIBCPP_BEGIN_NAMESPACE_STD
#define MOZ_END_STD_NAMESPACE _LIBCPP_END_NAMESPACE_STD
#else
#define MOZ_BEGIN_STD_NAMESPACE namespace std {
#define MOZ_END_STD_NAMESPACE }
#endif
MOZ_BEGIN_STD_NAMESPACE
using mozilla::DefaultDelete;
using mozilla::UniquePtr;
#define default_delete DefaultDelete
#define unique_ptr UniquePtr
MOZ_END_STD_NAMESPACE
#endif
#include "cubeb-speex-resampler.h"
#include "cubeb/cubeb.h"
#include "cubeb_log.h"
#include "cubeb_resampler.h"
#include "cubeb_utils.h"
#include <stdio.h>

/* This header file contains the internal C++ API of the resamplers, for
 * testing. */

// When dropping audio input frames to prevent building
// an input delay, this function returns the number of frames
// to keep in the buffer.
// @parameter sample_rate The sample rate of the stream.
// @return A number of frames to keep.
uint32_t
min_buffered_audio_frame(uint32_t sample_rate);

int
to_speex_quality(cubeb_resampler_quality q);

struct cubeb_resampler {
  virtual long fill(void * input_buffer, long * input_frames_count,
                    void * output_buffer, long frames_needed) = 0;
  virtual long latency() = 0;
  virtual ~cubeb_resampler() {}
};

/** Base class for processors. This is just used to share methods for now. */
class processor {
public:
  explicit processor(uint32_t channels) : channels(channels) {}

protected:
  size_t frames_to_samples(size_t frames) const { return frames * channels; }
  size_t samples_to_frames(size_t samples) const
  {
    assert(!(samples % channels));
    return samples / channels;
  }
  /** The number of channel of the audio buffers to be resampled. */
  const uint32_t channels;
};

template <typename T>
class passthrough_resampler : public cubeb_resampler, public processor {
public:
  passthrough_resampler(cubeb_stream * s, cubeb_data_callback cb, void * ptr,
                        uint32_t input_channels, uint32_t sample_rate);

  virtual long fill(void * input_buffer, long * input_frames_count,
                    void * output_buffer, long output_frames);

  virtual long latency() { return 0; }

  void drop_audio_if_needed()
  {
    uint32_t to_keep = min_buffered_audio_frame(sample_rate);
    uint32_t available = samples_to_frames(internal_input_buffer.length());
    if (available > to_keep) {
      internal_input_buffer.pop(nullptr,
                                frames_to_samples(available - to_keep));
    }
  }

private:
  cubeb_stream * const stream;
  const cubeb_data_callback data_callback;
  void * const user_ptr;
  /* This allows to buffer some input to account for the fact that we buffer
   * some inputs. */
  auto_array<T> internal_input_buffer;
  uint32_t sample_rate;
};

class clock_drift_estimator {
public:
  clock_drift_estimator(uint32_t input_rate, uint32_t output_rate);
  /*
   * input_frame_count: the number of (potentially resampled) audio input frame
   * count received this callback.
   * output_frame_count: the number of (unresampled) audio output frames to
   * return this callback.
   * buffered_input_frames: the number of input frames currently stored in the
   * input buffer, after potential input resampling. If this is > 0, then some
   * input frames are buffered.
   * input_frames_raw: number of audio input frames passed to this callback
   */
  double estimate(size_t input_frame_count, size_t output_frame_count,
                  size_t buffered_input_frames, size_t input_frames_raw);

private:
  // Number of input and input frames seen so far during this drift estimation
  // period.
  size_t total_output_frames = 0;
  size_t total_input_frames = 0;
  // The rate at which the frames come.
  uint32_t source_rate_hz;
  // The rate to which frames are resampled to
  uint32_t target_rate_hz;
  // The drift compensation command that was decided in the previous
  // estimation period.
  double compensation_command;
  // The number of frames that were buffered in the previous estimation call.
  uint32_t previous_buffered_input_frames = 0;
};

/** Bidirectional resampler, can resample an input and an output stream, or just
 * an input stream or output stream. When doing bidirectional resampling, it can
 * correct the drift between an input and an output, by reclocking the input
 * against the output. */
template <typename T, typename InputProcessing, typename OutputProcessing>
class cubeb_resampler_speex : public cubeb_resampler {
public:
  cubeb_resampler_speex(InputProcessing * input_processor,
                        OutputProcessing * output_processor, cubeb_stream * s,
                        cubeb_data_callback cb, void * ptr,
                        cubeb_resampler_reclock reclock);

  virtual ~cubeb_resampler_speex();

  virtual long fill(void * input_buffer, long * input_frames_count,
                    void * output_buffer, long output_frames_needed);

  virtual long latency()
  {
    if (input_processor && output_processor) {
      assert(input_processor->latency() == output_processor->latency());
      return input_processor->latency();
    } else if (input_processor) {
      return input_processor->latency();
    } else {
      return output_processor->latency();
    }
  }

private:
  typedef long (cubeb_resampler_speex::*processing_callback)(
      T * input_buffer, long * input_frames_count, T * output_buffer,
      long output_frames_needed);

  long fill_internal_duplex(T * input_buffer, long * input_frames_count,
                            T * output_buffer, long output_frames_needed);
  long fill_internal_input(T * input_buffer, long * input_frames_count,
                           T * output_buffer, long output_frames_needed);
  long fill_internal_output(T * input_buffer, long * input_frames_count,
                            T * output_buffer, long output_frames_needed);

  std::unique_ptr<InputProcessing> input_processor;
  std::unique_ptr<OutputProcessing> output_processor;
  processing_callback fill_internal;
  cubeb_stream * const stream;
  const cubeb_data_callback data_callback;
  void * const user_ptr;
  std::unique_ptr<clock_drift_estimator> drift_estimator;
  bool draining = false;
};

/** Handles one way of a (possibly) duplex resampler, working on interleaved
 * audio buffers of type T. This class is designed so that the number of frames
 * coming out of the resampler can be precisely controled. It manages its own
 * input buffer, and can use the caller's output buffer, or allocate its own. */
template <typename T> class cubeb_resampler_speex_one_way : public processor {
public:
  /** The sample type of this resampler, either 16-bit integers or 32-bit
   * floats. */
  typedef T sample_type;
  /** Construct a resampler resampling from #source_rate to #target_rate, that
   * can be arbitrary, strictly positive number.
   * @parameter channels The number of channels this resampler will resample.
   * @parameter source_rate The sample-rate of the audio input.
   * @parameter target_rate The sample-rate of the audio output.
   * @parameter quality A number between 0 (fast, low quality) and 10 (slow,
   * high quality). */
  cubeb_resampler_speex_one_way(uint32_t channels, uint32_t source_rate,
                                uint32_t target_rate, int quality)
      : processor(channels),
        resampling_ratio(static_cast<float>(source_rate) / target_rate),
        source_rate_hz(source_rate), target_rate_hz(target_rate),
        leftover_samples(0)
  {
    int r;
    speex_resampler =
        speex_resampler_init(channels, source_rate, target_rate, quality, &r);
    assert(r == RESAMPLER_ERR_SUCCESS && "resampler allocation failure");

    uint32_t input_latency = speex_resampler_get_input_latency(speex_resampler);
    const size_t LATENCY_SAMPLES = 8192;
    T input_buffer[LATENCY_SAMPLES] = {};
    T output_buffer[LATENCY_SAMPLES] = {};
    uint32_t input_frame_count = input_latency;
    uint32_t output_frame_count = LATENCY_SAMPLES;
    assert(input_latency * channels <= LATENCY_SAMPLES);
    speex_resample(input_buffer, &input_frame_count, output_buffer,
                   &output_frame_count);
  }

  /** Destructor, deallocate the resampler */
  virtual ~cubeb_resampler_speex_one_way()
  {
    speex_resampler_destroy(speex_resampler);
  }

  /* Fill the resampler with `input_frame_count` frames. */
  void input(T * input_buffer, size_t input_frame_count)
  {
    resampling_in_buffer.push(input_buffer,
                              frames_to_samples(input_frame_count));
  }

  /** Outputs exactly `output_frame_count` into `output_buffer`.
   * `output_buffer` has to be at least `output_frame_count` long. */
  size_t output(T * output_buffer, size_t output_frame_count)
  {
    uint32_t in_len = samples_to_frames(resampling_in_buffer.length());
    uint32_t out_len = output_frame_count;

    speex_resample(resampling_in_buffer.data(), &in_len, output_buffer,
                   &out_len);

    /* This shifts back any unresampled samples to the beginning of the input
       buffer. */
    resampling_in_buffer.pop(nullptr, frames_to_samples(in_len));

    return out_len;
  }

  size_t output_for_input(uint32_t input_frames)
  {
    return (size_t)floorf(
        (input_frames + samples_to_frames(resampling_in_buffer.length())) /
        resampling_ratio);
  }

  /** Returns a buffer containing exactly `output_frame_count` resampled frames.
   * The consumer should not hold onto the pointer. */
  T * output(size_t output_frame_count, size_t * input_frames_used)
  {
    if (resampling_out_buffer.capacity() <
        frames_to_samples(output_frame_count)) {
      resampling_out_buffer.reserve(frames_to_samples(output_frame_count));
    }

    uint32_t in_len = samples_to_frames(resampling_in_buffer.length());
    uint32_t out_len = output_frame_count;

    speex_resample(resampling_in_buffer.data(), &in_len,
                   resampling_out_buffer.data(), &out_len);

    if (out_len < output_frame_count) {
      LOGV("underrun during resampling: got %u frames, expected %zu",
           (unsigned)out_len, output_frame_count);
      // silence the rightmost part
      T * data = resampling_out_buffer.data();
      for (uint32_t i = frames_to_samples(out_len);
           i < frames_to_samples(output_frame_count); i++) {
        data[i] = 0;
      }
    }

    /* This shifts back any unresampled samples to the beginning of the input
       buffer. */
    resampling_in_buffer.pop(nullptr, frames_to_samples(in_len));
    *input_frames_used = in_len;

    return resampling_out_buffer.data();
  }

  /** Get the latency of the resampler, in output frames. */
  uint32_t latency() const
  {
    /* The documentation of the resampler talks about "samples" here, but it
     * only consider a single channel here so it's the same number of frames. */
    int latency = 0;

    latency = speex_resampler_get_output_latency(speex_resampler);

    assert(latency >= 0);

    return latency;
  }

  /** Returns the number of frames to pass in the input of the resampler to have
   * exactly `output_frame_count` resampled frames. This can return a number
   * slightly bigger than what is strictly necessary, but it guaranteed that the
   * number of output frames will be exactly equal. */
  uint32_t input_needed_for_output(int32_t output_frame_count) const
  {
    assert(output_frame_count >= 0); // Check overflow
    int32_t unresampled_frames_left =
        samples_to_frames(resampling_in_buffer.length());
    int32_t resampled_frames_left =
        samples_to_frames(resampling_out_buffer.length());
    float input_frames_needed =
        (output_frame_count - unresampled_frames_left) * resampling_ratio -
        resampled_frames_left;
    if (input_frames_needed < 0) {
      return 0;
    }
    return (uint32_t)ceilf(input_frames_needed);
  }

  /** Returns a pointer to the input buffer, that contains empty space for at
   * least `frame_count` elements. This is useful so that consumer can directly
   * write into the input buffer of the resampler. The pointer returned is
   * adjusted so that leftover data are not overwritten.
   */
  T * input_buffer(size_t frame_count)
  {
    leftover_samples = resampling_in_buffer.length();
    resampling_in_buffer.reserve(leftover_samples +
                                 frames_to_samples(frame_count));
    return resampling_in_buffer.data() + leftover_samples;
  }

  /** This method works with `input_buffer`, and allows to inform the processor
      how much frames have been written in the provided buffer. */
  void written(size_t written_frames)
  {
    resampling_in_buffer.set_length(leftover_samples +
                                    frames_to_samples(written_frames));
  }

  void drop_audio_if_needed()
  {
    // Keep at most 100ms buffered.
    uint32_t available = samples_to_frames(resampling_in_buffer.length());
    uint32_t to_keep = min_buffered_audio_frame(source_rate);
    if (available > to_keep) {
      resampling_in_buffer.pop(nullptr, frames_to_samples(available - to_keep));
    }
  }


  void set_resampling_ratio(double ratio) {
    int rv = speex_resampler_set_rate(speex_resampler, source_rate_hz, source_rate_hz / ratio);
    resampling_ratio = ratio;
    if (rv != RESAMPLER_ERR_SUCCESS) {
      LOGV("Error when setting the resampling rate: %d", rv);
    }
  }

  uint32_t source_rate() const {
    return source_rate_hz;
  }

  uint32_t target_rate() const { return target_rate_hz; }

  uint32_t input_buffer_frames() const
  {
    return samples_to_frames(resampling_in_buffer.length());
  }
  uint32_t output_buffer_frames() const
  {
    return samples_to_frames(resampling_out_buffer.length());
  }

private:
  /** Wrapper for the speex resampling functions to have a typed
   * interface. */
  void speex_resample(float * input_buffer, uint32_t * input_frame_count,
                      float * output_buffer, uint32_t * output_frame_count)
  {
#ifndef NDEBUG
    int rv;
    rv =
#endif
        speex_resampler_process_interleaved_float(
            speex_resampler, input_buffer, input_frame_count, output_buffer,
            output_frame_count);
    assert(rv == RESAMPLER_ERR_SUCCESS);
  }

  void speex_resample(short * input_buffer, uint32_t * input_frame_count,
                      short * output_buffer, uint32_t * output_frame_count)
  {
#ifndef NDEBUG
    int rv;
    rv =
#endif
        speex_resampler_process_interleaved_int(
            speex_resampler, input_buffer, input_frame_count, output_buffer,
            output_frame_count);
    assert(rv == RESAMPLER_ERR_SUCCESS);
  }
  /** The state for the speex resampler used internaly. */
  SpeexResamplerState * speex_resampler;
  /** Source rate / target rate. */
  float resampling_ratio;
  const uint32_t source_rate_hz;
  const uint32_t target_rate_hz;
  /** Storage for the input frames, to be resampled. Also contains
   * any unresampled frames after resampling. */
  auto_array<T> resampling_in_buffer;
  /* Storage for the resampled frames, to be passed back to the caller. */
  auto_array<T> resampling_out_buffer;
  /** When `input_buffer` is called, this allows tracking the number of samples
      that were in the buffer. */
  uint32_t leftover_samples;
};

/* A processor that passes the audio through without modification. */
template <typename T> class passthrough : public processor {
public:
  typedef T sample_type;
  passthrough(uint32_t channels) : processor(channels) {}
  virtual ~passthrough() = default;

  /* Fill the resampler with `input_frame_count` frames. */
  void input(T * input_buffer, size_t input_frame_count)
  {
    internal_input_buffer.push(input_buffer,
                               frames_to_samples(input_frame_count));
  }

  /** Outputs exactly `output_frame_count` into `output_buffer`.
   * `output_buffer` has to be at least `output_frame_count` long. */
  size_t output(T * output_buffer, size_t output_frame_count)
  {
    assert(samples_to_frames(internal_input_buffer.length()) >=
           output_frame_count);

    PodCopy(output_buffer, internal_input_buffer.data(),
            frames_to_samples(output_frame_count));

    /* This shifts back any unresampled samples to the beginning of the input
       buffer. */
    internal_input_buffer.pop(nullptr, frames_to_samples(output_frame_count));

    return output_frame_count;
  }

  T * output(size_t output_frame_count, size_t * input_frames_used)
  {
    size_t sample_count = frames_to_samples(output_frame_count);
    internal_output_buffer.reserve(sample_count);
    PodCopy(internal_input_buffer.data(), internal_input_buffer.data(),
            sample_count);

    internal_input_buffer.pop(nullptr, sample_count);
    *input_frames_used = output_frame_count;

    return internal_output_buffer.data();
  }

  size_t output_for_input(uint32_t input_frames) { return input_frames; }

  uint32_t latency() const { return 0; }

  uint32_t input_needed_for_output(int32_t output_frame_count) const
  {
    return output_frame_count;
  }

  T * input_buffer(size_t frame_count)
  {
    // return valid buffer space after the data left in the buffer
    size_t leftover_samples = internal_input_buffer.length();
    internal_input_buffer.reserve(leftover_samples +
                                  frames_to_samples(frame_count));
    return internal_input_buffer.data() + leftover_samples;
  }

  /** This method works with `input_buffer`, and allows to inform the processor
      how much frames have been written in the provided buffer. */
  void written(size_t written_frames)
  {
    size_t leftover_samples = internal_input_buffer.length();
    internal_input_buffer.set_length(leftover_samples +
                                     frames_to_samples(written_frames));
  }

  void set_resampling_ratio(double) { assert("need to enable reclocking"); }

  uint32_t source_rate() { return sample_rate_hz; }
  uint32_t target_rate() { return sample_rate_hz; }

  uint32_t input_buffer_frames() const
  {
    return samples_to_frames(internal_input_buffer.length());
  }
  uint32_t output_buffer_frames() const
  {
    return samples_to_frames(internal_output_buffer.length());
  }

private:
  auto_array<T> internal_input_buffer;
  // Only ever used when using `output` with two arguments.
  auto_array<T> internal_output_buffer;
  uint32_t sample_rate_hz = 0;
};

/** This sits behind the C API and is more typed. */
template <typename T>
cubeb_resampler *
cubeb_resampler_create_internal(cubeb_stream * stream,
                                cubeb_stream_params * input_params,
                                cubeb_stream_params * output_params,
                                unsigned int target_rate,
                                cubeb_data_callback callback, void * user_ptr,
                                cubeb_resampler_quality quality,
                                cubeb_resampler_reclock reclock)
{
  std::unique_ptr<cubeb_resampler_speex_one_way<T>> input_resampler = nullptr;
  std::unique_ptr<cubeb_resampler_speex_one_way<T>> output_resampler = nullptr;
  std::unique_ptr<passthrough<T>> input_passthrough = nullptr;
  std::unique_ptr<passthrough<T>> output_passthrough = nullptr;

  assert((input_params || output_params) &&
         "need at least one valid parameter pointer.");

  /* All the streams we have have a sample rate that matches the target
     sample rate, and no reclocking has been requested, use a no-op resampler,
     that simply forwards the buffers to the callback. */
  if (((input_params && input_params->rate == target_rate) &&
       (output_params && output_params->rate == target_rate) &&
       reclock == CUBEB_RESAMPLER_RECLOCK_NONE) ||
      (input_params && !output_params && (input_params->rate == target_rate)) ||
      (output_params && !input_params &&
       (output_params->rate == target_rate))) {
    LOG("Input and output sample-rate match, target rate of %dHz", target_rate);
    return new passthrough_resampler<T>(
        stream, callback, user_ptr, input_params ? input_params->channels : 0,
        target_rate);
  }

  /* Determine if we need to resampler one or both directions, and create the
     resamplers. */
  if (output_params && (output_params->rate != target_rate)) {
    output_resampler.reset(new cubeb_resampler_speex_one_way<T>(
        output_params->channels, target_rate, output_params->rate,
        to_speex_quality(quality)));
    if (!output_resampler) {
      return NULL;
    }
  }

  // If either input reclocking is necessary, or input resampling is necessary,
  // get an resampler for the audio input.
  if ((input_params && (input_params->rate != target_rate)) ||
      (input_params->rate == target_rate &&
       reclock == CUBEB_RESAMPLER_RECLOCK_INPUT)) {
    input_resampler.reset(new cubeb_resampler_speex_one_way<T>(
        input_params->channels, input_params->rate, target_rate,
        to_speex_quality(quality)));
    if (!input_resampler) {
      return NULL;
    }
  }

  if (input_resampler && !output_resampler && input_params && output_params) {
    output_passthrough.reset(new passthrough<T>(output_params->channels));
    if (!output_passthrough) {
      return NULL;
    }
  } else if (output_resampler && !input_resampler && input_params &&
             output_params) {
    input_passthrough.reset(new passthrough<T>(input_params->channels));
    if (!input_passthrough) {
      return NULL;
    }
  }

  if (input_resampler && output_resampler) {
    LOG("Resampling input (%d) and output (%d) to target rate of %dHz",
        input_params->rate, output_params->rate, target_rate);
    return new cubeb_resampler_speex<T, cubeb_resampler_speex_one_way<T>,
                                     cubeb_resampler_speex_one_way<T>>(
        input_resampler.release(), output_resampler.release(), stream, callback,
        user_ptr, reclock);
  } else if (input_resampler) {
    LOG("Resampling input (%d) to target and output rate of %dHz, with%s "
        "reclocking",
        input_params->rate, target_rate,
        reclock == CUBEB_RESAMPLER_RECLOCK_INPUT ? "" : "out");
    return new cubeb_resampler_speex<T, cubeb_resampler_speex_one_way<T>,
                                     passthrough<T>>(
        input_resampler.release(), output_passthrough.release(), stream,
        callback, user_ptr, reclock);
  } else {
    LOG("Resampling output (%dHz) to target and input rate of %dHz, with%s "
        "reclocking",
        output_params->rate, target_rate,
        reclock == CUBEB_RESAMPLER_RECLOCK_INPUT ? "" : "out");
    return new cubeb_resampler_speex<T, passthrough<T>,
                                     cubeb_resampler_speex_one_way<T>>(
        input_passthrough.release(), output_resampler.release(), stream,
        callback, user_ptr, reclock);
  }
}

#endif /* CUBEB_RESAMPLER_INTERNAL */
