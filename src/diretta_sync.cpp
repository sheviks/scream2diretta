// Implementation of ScreamDirettaSync. See diretta_sync.h for context.
//
//  the ring is externally owned; the Sync only reads from it. The
// prefill / rebuffer / underrun gates are unchanged in shape — they decide
// whether each getNewStream cycle outputs silence or pops from the ring.

#include "diretta_sync.h"

#include <Diretta/Sync>
#include <Diretta/Stream>

#include <algorithm>
#include <cstring>
#include <iostream>
#include <thread>

#if defined(__linux__)
#include <pthread.h>
#include <sched.h>
#endif

namespace scream_diretta {

namespace {
// RAII guard that pairs with ScreamDirettaSync::deactivate(). Mirrors
// DRUP's RingAccessGuard / beginReconfigure pattern: increments the
// ring-user counter only after observing m_active==true, then re-checks
// to close the race window where deactivate() flipped m_active=false
// between the first check and the increment. ~Guard releases the
// counter so deactivate()'s spin loop can make progress.
class RingUserGuard {
public:
    RingUserGuard(std::atomic<int>& users, const std::atomic<bool>& active)
        : users_(users), active_(false) {
        if (!active.load(std::memory_order_acquire)) return;
        // acq_rel ensures the increment is visible to deactivate()'s
        // load(acquire) and that we observe a subsequent store of
        // m_active==false.
        users_.fetch_add(1, std::memory_order_acq_rel);
        if (!active.load(std::memory_order_acquire)) {
            // Lost the race: deactivate() set m_active=false either
            // before or concurrently with our increment. Roll back; we
            // never entered the guarded section, so relaxed is fine.
            users_.fetch_sub(1, std::memory_order_relaxed);
            return;
        }
        active_ = true;
    }

    ~RingUserGuard() {
        if (active_) {
            // release ensures all ring ops complete before the
            // decrement is visible to deactivate().
            users_.fetch_sub(1, std::memory_order_release);
        }
    }

    bool active() const { return active_; }

    RingUserGuard(const RingUserGuard&) = delete;
    RingUserGuard& operator=(const RingUserGuard&) = delete;

private:
    std::atomic<int>& users_;
    bool active_;
};
} // anonymous namespace

ScreamDirettaSync::ScreamDirettaSync() = default;
ScreamDirettaSync::~ScreamDirettaSync() = default;

void ScreamDirettaSync::deactivate() {
    // Two-phase shutdown: flip the gate first so new getNewStream cycles
    // bail at Gate 0, then spin until any cycle that already passed the
    // first m_active check has exited its RingUserGuard. After this
    // returns the owner (DirettaState) is guaranteed no SDK pull is
    // inside m_ring access and may safely resize / free the
    // externally-owned ring buffer. Cycles are ~hundreds of microseconds
    // long so the yield loop is bounded in practice; we deliberately do
    // not impose a deadline because a forced detach here would re-
    // introduce the use-after-free we are trying to eliminate.
    m_active.store(false, std::memory_order_release);
    while (m_ringUsers.load(std::memory_order_acquire) > 0) {
        std::this_thread::yield();
    }
}

void ScreamDirettaSync::configureFormat(uint32_t sampleRate,
                                        uint32_t channels,
                                        uint32_t bytesPerSample,
                                        const SyncTuning& tuning) {
    const size_t bytesPerFrame  = static_cast<size_t>(channels) * bytesPerSample;
    const size_t bytesPerSecond = static_cast<size_t>(sampleRate) * bytesPerFrame;
    if (bytesPerSecond == 0) return;

    m_bytesPerFrame.store(static_cast<uint32_t>(bytesPerFrame), std::memory_order_release);
    m_bytesPerSecond.store(static_cast<uint32_t>(bytesPerSecond), std::memory_order_release);

    //  cache the active format scalars for the egress dumper. Stored
    // plain (not atomic) because configureFormat() runs on the control
    // thread between play()/stop() -- the SDK send thread is paused during
    // reconfigure so a non-atomic write is safe.
    m_egress_rate     = sampleRate;
    m_egress_channels = channels;
    m_egress_bits     = bytesPerSample * 8u;

    //  configure the egress startup analyser + fader for this open.
    // Safe to call regardless of enabled state (the helpers no-op when
    // disabled). The SDK send thread is paused during reconfigure so
    // resetting the running counters here is race-free.
    if (m_egress_analyzer) {
        pcm_startup_analyzer_configure(m_egress_analyzer,
                                       sampleRate, channels, m_egress_bits);
    }
    if (m_egress_fader) {
        pcm_startup_fader_configure(m_egress_fader,
                                    sampleRate, channels, m_egress_bits);
    }

    // Ring sizing happened externally; pick a silence byte for the gate.
    const uint8_t silence = (m_ring ? m_ring->silenceByte() : 0x00);
    m_silenceByte.store(silence, std::memory_order_release);

    // Prefill / startup gate target in bytes. Effective threshold is
    // max(prefill_ms, startup_queue_ms) so callers can override the open-
    // time gate independently of steady-state prefill, but the default
    // (startup_queue_ms == 0) preserves prior behaviour exactly.
    int prefillMs = tuning.prefill_ms;
    if (prefillMs < 0) prefillMs = 0;
    int startupMs = tuning.startup_queue_ms;
    if (startupMs < 0) startupMs = 0;
    int gateMs = (startupMs > prefillMs) ? startupMs : prefillMs;
    int ringMs = tuning.ring_buffer_ms;
    if (ringMs < 50)   ringMs = 50;
    if (ringMs > 5000) ringMs = 5000;
    if (gateMs > ringMs) gateMs = ringMs / 2;
    size_t prefillBytes = (bytesPerSecond * static_cast<size_t>(gateMs)) / 1000;
    if (bytesPerFrame > 0) {
        prefillBytes = (prefillBytes / bytesPerFrame) * bytesPerFrame;
    }
    m_prefillBytes.store(prefillBytes, std::memory_order_release);

    float pct = tuning.rebuffer_percent;
    if (pct < 0.0f) pct = 0.0f;
    if (pct > 0.95f) pct = 0.95f;
    m_rebufferPct.store(pct, std::memory_order_release);

    //  absolute underrun-rebuffer target in bytes (from
    // tuning.underrun_rebuffer_ms). Clamped to the ring capacity. 0 means
    // "fall back to rebuffer_percent". Used by Gate 2 in getNewStream().
    int urMs = tuning.underrun_rebuffer_ms;
    if (urMs < 0) urMs = 0;
    size_t urBytes = (bytesPerSecond * static_cast<size_t>(urMs)) / 1000;
    if (bytesPerFrame > 0 && urBytes > 0) {
        urBytes = (urBytes / bytesPerFrame) * bytesPerFrame;
    }
    // Clamp to a bit under the ring so we can actually reach it.
    if (m_ring) {
        size_t cap = m_ring->capacity();
        if (cap > 0 && urBytes > (cap - bytesPerFrame)) {
            urBytes = (cap > bytesPerFrame * 2) ? (cap - bytesPerFrame * 2) : 0;
        }
    }
    m_underrunRebufferBytes.store(urBytes, std::memory_order_release);

    // Forced silent warmup. Convert ms -> bytes and clamp to the ring
    // capacity-equivalent window (no need to clamp further; emitting silent
    // cycles is a no-op on the queue). bytesPerFrame alignment matters only
    // for the emit counter -- the silence pattern itself is a byte fill.
    int muteMs = tuning.startup_mute_ms;
    if (muteMs < 0)    muteMs = 0;
    if (muteMs > 2000) muteMs = 2000;
    size_t muteBytes = (bytesPerSecond * static_cast<size_t>(muteMs)) / 1000;
    if (bytesPerFrame > 0) {
        muteBytes = (muteBytes / bytesPerFrame) * bytesPerFrame;
    }
    m_muteBytes.store(muteBytes, std::memory_order_release);

    // startup_real_delay window: silence cycles AFTER the prefill gate
    // releases, without consuming the queue. Convert ms -> bytes aligned to
    // frame size.
    int realDelayMs = tuning.startup_real_delay_ms;
    if (realDelayMs < 0)    realDelayMs = 0;
    if (realDelayMs > 5000) realDelayMs = 5000;
    size_t realDelayBytes = (bytesPerSecond * static_cast<size_t>(realDelayMs)) / 1000;
    if (bytesPerFrame > 0) {
        realDelayBytes = (realDelayBytes / bytesPerFrame) * bytesPerFrame;
    }
    m_realDelayBytes.store(realDelayBytes, std::memory_order_release);

    // Cycle size from the SDK is the number of bytes the send thread will ask
    // for each call. getCycleSize() is valid as soon as setSinkConfigure()
    // and a transfer-mode (configTransferAuto/etc.) have been applied -- the
    // caller orders configureFormat() after those steps.
    size_t cycle = getCycleSize();
    if (cycle == 0) {
        // Fallback to ~1ms worth of audio rounded to a frame.
        cycle = bytesPerSecond / 1000;
        if (bytesPerFrame > 0) {
            cycle = (cycle / bytesPerFrame) * bytesPerFrame;
        }
        if (cycle == 0) cycle = bytesPerFrame;
    }
    m_streamBytes.store(cycle, std::memory_order_release);
    m_streamData.assign(cycle, silence);

    resetGate();
}

void ScreamDirettaSync::resetGate() {
    m_prefillDone.store(false, std::memory_order_release);
    m_rebuffering.store(false, std::memory_order_release);
    m_streamCount.store(0, std::memory_order_release);
    m_silentCycles.store(0, std::memory_order_release);
    m_realCycles.store(0, std::memory_order_release);
    m_muteBytesEmitted.store(0, std::memory_order_release);
    const size_t target = m_muteBytes.load(std::memory_order_acquire);
    m_muteDone.store(target == 0, std::memory_order_release);
    m_underrunEvents.store(0, std::memory_order_release);
    m_rebufferTargetBytes.store(0, std::memory_order_release);
    m_poppedBytes.store(0, std::memory_order_release);
    m_realDelayEmitted.store(0, std::memory_order_release);
    m_realDelayCycles.store(0, std::memory_order_release);
    const size_t realDelayTarget = m_realDelayBytes.load(std::memory_order_acquire);
    m_realDelayDone.store(realDelayTarget == 0, std::memory_order_release);
}

bool ScreamDirettaSync::getNewStream(diretta_stream& s) {
    const size_t want = m_streamBytes.load(std::memory_order_acquire);

    // Gate 0: if the Sync is being torn down, emit silence without touching
    // the externally-owned ring (which may be resizing or destroyed).
    //
    // RingUserGuard pairs with deactivate(): once it returns active(),
    // deactivate() is guaranteed to wait for our destructor before
    // letting the owner resize/free m_ring. The two-phase check inside
    // the guard closes the race where deactivate() flips m_active=false
    // between our load and our increment.
    RingUserGuard ringGuard(m_ringUsers, m_active);
    if (!ringGuard.active()) {
        if (want > 0) {
            if (m_streamData.size() < want) {
                m_streamData.assign(want, m_silenceByte.load(std::memory_order_acquire));
            } else {
                std::memset(m_streamData.data(), m_silenceByte.load(std::memory_order_acquire), want);
            }
            s.Data.P = m_streamData.data();
            s.Size   = want;
        } else {
            s.Data.P = nullptr;
            s.Size = 0;
        }
        return true;
    }

    // Apply SCHED_FIFO once on the first real call, running on the SDK
    // worker thread so the priority affects the actual audio pull path.
    if (!m_rtPriorityApplied.load(std::memory_order_acquire)) {
        const int prio = m_rtPriority.load(std::memory_order_relaxed);
        if (prio >= 1) {
#if defined(__linux__)
            struct sched_param param;
            param.sched_priority = prio;
            if (pthread_setschedparam(pthread_self(), SCHED_FIFO, &param) == 0) {
                std::cout << "[diretta] SDK worker set to SCHED_FIFO priority "
                          << prio << std::endl;
            } else {
                std::cerr << "[diretta] WARNING: Failed to set SDK worker "
                          << "SCHED_FIFO priority " << prio
                          << " (errno=" << errno << ")" << std::endl;
            }
#endif
        }
        m_rtPriorityApplied.store(true, std::memory_order_release);
    }

    if (want == 0 || !m_ring) {
        // Not configured yet; hand back an empty buffer.
        s.Data.P = nullptr;
        s.Size = 0;
        return true;
    }
    if (m_streamData.size() != want) {
        // configureFormat preallocates this; in steady state this branch is
        // never taken. We still resize defensively for the very first call.
        m_streamData.assign(want, m_silenceByte.load(std::memory_order_acquire));
    }
    s.Data.P = m_streamData.data();
    s.Size   = want;

    uint8_t* dest = m_streamData.data();
    const uint8_t silence = m_silenceByte.load(std::memory_order_acquire);
    m_streamCount.fetch_add(1, std::memory_order_relaxed);

    // Gate 0: forced silent warmup. Until we have emitted muteBytes
    // worth of zero PCM through real Diretta pull cycles, every cycle is
    // silent and does NOT pop the ring. This sits BEFORE the prefill gate
    // so even if the queue is already huge at open() time (e.g. ~1.2s
    // accumulated during the format-change cooldown), the target/DAC sees
    // genuine silent cycles first. Click mitigation that the fill-only
    // gate cannot provide.
    if (!m_muteDone.load(std::memory_order_acquire)) {
        const size_t target = m_muteBytes.load(std::memory_order_acquire);
        std::memset(dest, silence, want);
        m_silentCycles.fetch_add(1, std::memory_order_relaxed);
        if (target == 0) {
            m_muteDone.store(true, std::memory_order_release);
        } else {
            size_t emitted = m_muteBytesEmitted.fetch_add(want, std::memory_order_relaxed) + want;
            if (emitted >= target) {
                m_muteDone.store(true, std::memory_order_release);
            }
        }
        return true;
    }

    // Gate 1: still in startup priming. The queue is filling but has not
    // reached the configured threshold yet. Emit silence WITHOUT popping
    // from the ring so the head of the track survives. This is what makes
    // the unified-queue design work: the same queue carries the head
    // of the track through cooldown/open/handshake, and the SDK simply
    // outputs silence until enough is buffered to start playback.
    if (!m_prefillDone.load(std::memory_order_acquire)) {
        size_t target = m_prefillBytes.load(std::memory_order_acquire);
        if (target == 0 || m_ring->available() >= target) {
            m_prefillDone.store(true, std::memory_order_release);
        } else {
            std::memset(dest, silence, want);
            m_silentCycles.fetch_add(1, std::memory_order_relaxed);
            return true;
        }
    }

    // Gate 1.5: startup_real_delay. After the prefill gate releases
    // (queue has enough audio), still emit silence for a configurable
    // window WITHOUT popping from the ring. The head of the track waits
    // intact; the target/DAC sees silence cycles to settle on. This
    // differs from the startup_mute (Gate 0) which sits before the
    // prefill gate; here we know the queue is primed and just delay the
    // first real PCM byte deterministically.
    if (!m_realDelayDone.load(std::memory_order_acquire)) {
        const size_t target = m_realDelayBytes.load(std::memory_order_acquire);
        if (target == 0) {
            m_realDelayDone.store(true, std::memory_order_release);
        } else {
            std::memset(dest, silence, want);
            m_silentCycles.fetch_add(1, std::memory_order_relaxed);
            m_realDelayCycles.fetch_add(1, std::memory_order_relaxed);
            const size_t emitted = m_realDelayEmitted.fetch_add(want, std::memory_order_relaxed) + want;
            if (emitted >= target) {
                m_realDelayDone.store(true, std::memory_order_release);
            }
            return true;
        }
    }

    // Gate 2: rebuffering after a sustained underrun. Hold silence until
    // queue fill recovers to the armed target ( prefer the absolute
    // underrun_rebuffer_ms target if configured, else rebuffer_percent).
    if (m_rebuffering.load(std::memory_order_acquire)) {
        const size_t threshold = m_rebufferTargetBytes.load(std::memory_order_acquire);
        if (m_ring->available() >= threshold) {
            m_rebuffering.store(false, std::memory_order_release);
            // Fall through to a normal pop.
        } else {
            std::memset(dest, silence, want);
            m_silentCycles.fetch_add(1, std::memory_order_relaxed);
            return true;
        }
    }

    // Gate 3: arm rebuffering if a single cycle cannot be satisfied. 
    // pick the rebuffer target. Prefer the absolute byte target derived
    // from underrun_rebuffer_ms (smaller, faster recovery for transient
    // hiccups); else use the rebuffer_percent of capacity. We arm
    // the gate even when both knobs evaluate to 0 bytes so the underrun
    // event count stays meaningful -- but in that case we recover next
    // cycle (threshold == 0 always satisfied).
    if (m_ring->available() < want) {
        size_t target = m_underrunRebufferBytes.load(std::memory_order_acquire);
        if (target == 0) {
            const float pct = m_rebufferPct.load(std::memory_order_acquire);
            target = static_cast<size_t>(m_ring->capacity() * pct);
        }
        m_rebufferTargetBytes.store(target, std::memory_order_release);
        m_rebuffering.store(true, std::memory_order_release);
        m_underrunEvents.fetch_add(1, std::memory_order_relaxed);
        const size_t avail_before = m_ring->available();
        m_ring->popOrSilence(dest, want);
        if (avail_before > 0) {
            m_poppedBytes.fetch_add(avail_before, std::memory_order_relaxed);
            //  feed the egress analyser (real PCM only). Runs BEFORE
            // any fade so the analyser observes raw queue contents.
            if (m_egress_analyzer && !pcm_startup_analyzer_done(m_egress_analyzer)) {
                pcm_startup_analyzer_feed(m_egress_analyzer, dest, avail_before);
            }
            //  apply the egress fade IN PLACE before dumping, so the
            // egress WAV reflects what the SDK actually receives.
            if (m_egress_fader && !pcm_startup_fader_done(m_egress_fader)) {
                pcm_startup_fader_apply(m_egress_fader, dest, avail_before);
            }
            //  dump only the bytes that were actually real PCM (the
            // part popOrSilence padded with silence at the tail does not
            // count as egress real PCM).
            if (m_egress_dumper && pcm_dumper_enabled(m_egress_dumper)) {
                if (pcm_dumper_open_or_rotate(m_egress_dumper,
                                              m_egress_rate, m_egress_channels,
                                              m_egress_bits)) {
                    pcm_dumper_write(m_egress_dumper, dest, avail_before);
                }
            }
        }
        m_realCycles.fetch_add(1, std::memory_order_relaxed);
        return true;
    }

    m_ring->popOrSilence(dest, want);
    m_poppedBytes.fetch_add(want, std::memory_order_relaxed);
    m_realCycles.fetch_add(1, std::memory_order_relaxed);
    //  feed the egress analyser BEFORE the fade so we see raw queue
    // contents, then apply the fade IN PLACE so dump + SDK see post-fade.
    if (m_egress_analyzer && !pcm_startup_analyzer_done(m_egress_analyzer)) {
        pcm_startup_analyzer_feed(m_egress_analyzer, dest, want);
    }
    if (m_egress_fader && !pcm_startup_fader_done(m_egress_fader)) {
        pcm_startup_fader_apply(m_egress_fader, dest, want);
    }
    //  dump the full cycle as real egress PCM. We reached this branch
    // only when ring->available() >= want, so the bytes are guaranteed
    // real.
    if (m_egress_dumper && pcm_dumper_enabled(m_egress_dumper)) {
        if (pcm_dumper_open_or_rotate(m_egress_dumper,
                                      m_egress_rate, m_egress_channels,
                                      m_egress_bits)) {
            pcm_dumper_write(m_egress_dumper, dest, want);
        }
    }
    return true;
}

} // namespace scream_diretta
