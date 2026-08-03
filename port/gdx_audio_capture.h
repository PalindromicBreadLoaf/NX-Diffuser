/* G-Diffuser -- streaming PCM capture for the bit-identical audio gate.
 *
 * A dormant diagnostic (gdx_unlock_audio_capture_ai_buffer in gdx_audio_lle.c) already existed
 * upstream of ALL host post-processing (low-pass, volume, underrun-fade), but the tap call site
 * at decomp/src/audio/disk/lib/thread.c:87-96 (AudioThread_CreateTaskImpl, immediately before its
 * osAiSetNextBuffer) is what wires this module into the scheduler; both taps run, side by side.
 * This module is the streaming replacement used by the PCM-parity harness: instead of
 * a fixed 64000-frame RAM ring it appends raw interleaved s16 stereo samples straight to
 * <prefix>.pcm and, on finalize, writes a SHA-256 sidecar the harness compares run-to-run.
 *
 * FORMAT of <prefix>.pcm: headerless, interleaved signed 16-bit little-endian stereo
 * (L,R,L,R,...). One "frame" is one L+R sample pair = 4 bytes. Sample rate is 32000 Hz
 * on this title but is taken from the tap per call. The bytes are host-native little-
 * endian s16 exactly as the tap delivers them (the AI output buffer), so no conversion.
 *
 * ENV CONTROL:
 *   GDX_PCM_CAPTURE          output path PREFIX. Unset -> every call is a no-op (zero
 *                            behavior change for normal play). Set -> capture is armed at
 *                            gdx_pcm_capture_init() and streams to <prefix>.pcm.
 *   GDX_PCM_CAPTURE_FRAMES   optional frame cap. When >0 the window auto-finalizes after
 *                            that many frames (deterministic capture length in frames, not
 *                            wall-clock). Unset/0 -> unbounded until gdx_pcm_capture_shutdown().
 *
 * THREADING: gdx_pcm_capture_feed()/_active() run on the audio thread (the tap). init/arm/
 * shutdown/finished run on the host/main thread. The handful of cross-thread flags are plain
 * ints read/written atomically on this target (aligned int), matching the benign-race pattern
 * used elsewhere in the port (see port_log.h). No lock is taken on the audio-tick fast path.
 */
#ifndef GDX_AUDIO_CAPTURE_H
#define GDX_AUDIO_CAPTURE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Parse GDX_PCM_CAPTURE / GDX_PCM_CAPTURE_FRAMES once and, when GDX_PCM_CAPTURE is set,
   arm the capture window (opens <prefix>.pcm). A no-op if the env var is unset. Call once
   at boot, BEFORE the audio thread begins producing ticks, so gdx_pcm_capture_active() is
   already true on the first audio tick (the RNG determinism pin depends on this). */
void gdx_pcm_capture_init(void);

/* Arm the capture window explicitly. Idempotent; a no-op when unconfigured or already
   finalized. gdx_pcm_capture_init() calls this for you when capture is configured. */
void gdx_pcm_capture_arm(void);

/* Append interleaved s16 stereo frames to <prefix>.pcm (frameCount = L+R sample pairs).
   A no-op unless capture is configured AND already armed (fail-closed: this does NOT auto-arm --
   init/arm must run on the main thread before the audio thread starts feeding, per the threading
   contract above). When the GDX_PCM_CAPTURE_FRAMES cap is reached it writes the remaining frames
   and finalizes. */
void gdx_pcm_capture_feed(const int16_t* frames, unsigned int frameCount, unsigned int sampleRate);

/* 1 while a capture window is armed and not yet finalized. Gates the deterministic-RNG
   substitution at thread.c:205-226 (the osGetCount() pin inside AudioThread_CreateTaskImpl) -- 0
   in all normal (unconfigured) play, so that site keeps its original hardware-entropy
   expression. */
int gdx_pcm_capture_active(void);

/* 1 once the capture window has finalized (frame cap reached, or shutdown). Polled by the
   host loop to trigger auto-exit. Always 0 when capture is unconfigured. */
int gdx_pcm_capture_finished(void);

/* Finalize an in-progress capture: close <prefix>.pcm and write <prefix>.pcm.sha256. Called
   at host shutdown so an unbounded capture still emits its digest. A no-op when unconfigured
   or already finalized. */
void gdx_pcm_capture_shutdown(void);

#ifdef __cplusplus
}
#endif

#endif /* GDX_AUDIO_CAPTURE_H */
