// Does the Icom transmit path's 24 kHz -> 48 kHz resample survive 1200-baud
// AFSK?
//
// WHY THIS EXISTS. Chasing an AX.25 transmission that nothing would decode
// (#5011, 2026-08-12 onward), instrumentation on the Icom transmit path first
// appeared to bracket a fault across the resampler. That reading did not
// survive: the apparent difference was an artefact of the instruments
// themselves, and AE's client TX path measured CLEAN end to end -- resample
// ratio 1.0000, LPCM round-trip worst error 0.000026, AFSK tones preserved,
// and Direwolf atest decoding AE's own transmit audio 3/3 frames. The live
// candidate is radio-side (a deviation shortfall), not the client chain.
//
// So this test is a REGRESSION PIN, not a hunt: it fixes in place the
// client-side behaviour that investigation established is correct, so the
// same ground never has to be re-measured by hand on a radio bench.
//
// This test isolates the FIRST of those three with no radio, no sockets and no
// Qt event loop: synthesise a real AX.25 SABM as Bell 202 AFSK, run it through
// the same Resampler the backend uses, and measure whether the tones survive.
//
// It deliberately does NOT decode AX.25 — that would drag libmodem into a unit
// test. It measures the two properties a 1200-baud demodulator actually needs:
//
//   1. DURATION. A resampler that emits the wrong number of samples shifts
//      every bit period. At 1200 baud a 1 % error walks a full bit in 83 bits,
//      which is inside one frame.
//   2. TONE PURITY. Mark must stay at 1200 Hz and space at 2200 Hz, each
//      dominant in its own symbol, with no third frequency appearing between
//      them.
//
// If both hold, the resampler is faithful. The LPCM encode and the TX
// packetiser below it are covered by the later cases in this file.

#include "core/Resampler.h"
#include "core/backends/icom/IcomAudio.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <set>
#include <vector>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

using AetherSDR::Resampler;

namespace {

int g_failures = 0;

void check(bool cond, const char* what)
{
    if (!cond) {
        std::fprintf(stderr, "FAIL: %s\n", what);
        ++g_failures;
    }
}

constexpr int kSrcRate = 24000;   // AudioEngine::DEFAULT_SAMPLE_RATE
constexpr int kDstRate = 48000;   // the IC-9700's negotiated audio rate
constexpr int kBaud    = 1200;
constexpr double kMarkHz  = 1200.0;
constexpr double kSpaceHz = 2200.0;
// The modulator's own amplitude (AetherAx25LibmodemShim.cpp).
constexpr double kAmplitude = 0.35;

// Continuous-phase AFSK, matching the shim: phase carries across bit
// boundaries, so there is no discontinuity for the resampler to ring on. A
// discontinuous generator would make this test fail for a reason that is the
// test's fault rather than the code's.
std::vector<float> synthAfsk(const std::vector<uint8_t>& bits, int sampleRate)
{
    const int samplesPerSymbol = sampleRate / kBaud;
    std::vector<float> out;
    out.reserve(bits.size() * samplesPerSymbol);
    double phase = 0.0;
    for (uint8_t bit : bits) {
        const double freq = bit ? kMarkHz : kSpaceHz;
        const double step = 2.0 * M_PI * freq / sampleRate;
        for (int i = 0; i < samplesPerSymbol; ++i) {
            out.push_back(static_cast<float>(kAmplitude * std::sin(phase)));
            phase += step;
            if (phase >= 2.0 * M_PI)
                phase -= 2.0 * M_PI;
        }
    }
    return out;
}

// Goertzel magnitude at one frequency. Cheaper than an FFT and enough to ask
// "is this window dominated by the tone it should be".
double toneMagnitude(const float* x, int n, double freq, int sampleRate)
{
    const double w = 2.0 * M_PI * freq / sampleRate;
    const double coeff = 2.0 * std::cos(w);
    double s1 = 0.0, s2 = 0.0;
    for (int i = 0; i < n; ++i) {
        const double s0 = x[i] + coeff * s1 - s2;
        s2 = s1;
        s1 = s0;
    }
    const double real = s1 - s2 * std::cos(w);
    const double imag = s2 * std::sin(w);
    return std::hypot(real, imag) / (n / 2.0);
}

}  // namespace

int main()
{
    // A plausible SABM bit pattern: 64 flag bits' worth of alternating preamble
    // then mixed data. The exact frame does not matter — what matters is that
    // both tones appear, in runs and in alternation, the way real HDLC does.
    std::vector<uint8_t> bits;
    for (int i = 0; i < 64 * 8; ++i)            // preamble: 0x7E flags
        bits.push_back((i % 8) == 7 ? 0 : 1);
    for (int i = 0; i < 300; ++i)               // data: alternating and runs
        bits.push_back((i / 3) % 2);

    const std::vector<float> src = synthAfsk(bits, kSrcRate);
    check(!src.empty(), "synthesised source audio");

    Resampler rs(kSrcRate, kDstRate, 4096);
    // Feed it in 20 ms blocks, exactly as Ax25HfPacketDecodeDialog paces chunks
    // (kTxChunkMs = 20) and as submitTxAudio therefore receives them. A
    // resampler that only misbehaves across block boundaries would pass a
    // single-shot test and fail in production.
    const int blockSamples = kSrcRate / 50;
    std::vector<float> dst;
    for (std::size_t off = 0; off < src.size(); off += blockSamples) {
        const int n = static_cast<int>(std::min<std::size_t>(blockSamples, src.size() - off));
        const QByteArray out = rs.process(src.data() + off, n);
        const auto* f = reinterpret_cast<const float*>(out.constData());
        dst.insert(dst.end(), f, f + out.size() / static_cast<int>(sizeof(float)));
    }
    check(!dst.empty(), "resampler produced output");

    // ── 1. DURATION ────────────────────────────────────────────────────────
    // 24k -> 48k must double the sample count. r8brain's filter latency means
    // the first call emits slightly fewer samples, so allow 1 % — far tighter
    // than the ~8 % a single dropped bit period would cost at 1200 baud.
    const double expected = src.size() * (double(kDstRate) / kSrcRate);
    const double ratio = dst.size() / expected;
    std::printf("duration: in=%zu out=%zu expected=%.0f ratio=%.4f\n",
                src.size(), dst.size(), expected, ratio);
    check(ratio > 0.99 && ratio < 1.01,
          "resampled length is within 1 % of 2x the input");

    // ── 2. TONE PURITY ─────────────────────────────────────────────────────
    // Sample well inside the preamble, past any startup transient. A steady
    // run of flags is mark-heavy, so mark must dominate space there.
    // ONE SYMBOL, not a 50 ms window. At 1200 baud a symbol is 833 us, so a
    // 50 ms window spans 60 of them and averages mark against space until both
    // cancel — which is exactly what the first version of this test measured
    // (mark 0.00000, space 0.00000, stray 0.06369: impossible for real audio,
    // and a measurement bug rather than a finding).
    const int samplesPerSymbol = kDstRate / kBaud;   // 40 at 48 kHz
    const int win = samplesPerSymbol;
    // FIND THE AUDIO, do not assume where it starts. r8brain emits leading
    // zeros while its filter history fills (see Resampler::prewarm), and a
    // fixed offset landed inside that region: the window was silent and every
    // Goertzel read 0.00000, which reads as "the tones vanished" and is really
    // "you measured silence". Seek the first sample above a floor, then step a
    // few symbols further in so the window sits on steady tone.
    int firstSignal = 0;
    while (firstSignal < static_cast<int>(dst.size())
           && std::abs(dst[firstSignal]) < 0.01f)
        ++firstSignal;
    const int atSample = firstSignal + samplesPerSymbol * 4;
    check(dst.size() > static_cast<std::size_t>(atSample + win), "enough output to measure");
    if (dst.size() > static_cast<std::size_t>(atSample + win)) {
        // Prove the window actually contains audio before believing any
        // magnitude computed from it. A zero-amplitude window makes every
        // Goertzel read 0.00000, which looks like "the tone vanished" and is
        // really "you measured silence".
        float winPeak = 0.0f;
        for (int i = 0; i < win; ++i)
            winPeak = std::max(winPeak, std::abs(dst[atSample + i]));
        std::printf("measure window: start=%d len=%d peak=%.5f\n", atSample, win, winPeak);
        check(winPeak > 0.01f, "the measurement window contains signal");

        const double mark  = toneMagnitude(dst.data() + atSample, win, kMarkHz,  kDstRate);
        const double space = toneMagnitude(dst.data() + atSample, win, kSpaceHz, kDstRate);
        // A frequency that belongs to NEITHER tone. Intermodulation or aliasing
        // would put energy here; a clean resample leaves it near the floor.
        const double stray = toneMagnitude(dst.data() + atSample, win, 1700.0, kDstRate);
        std::printf("preamble: mark(1200)=%.5f space(2200)=%.5f stray(1700)=%.5f\n",
                    mark, space, stray);
        check(mark > space, "mark dominates space in a flag preamble");
        check(stray < mark, "the between-tones frequency is not the dominant one");
        // The modulator writes 0.35 amplitude; a resample that halves or doubles
        // the level is a bug even when the tone frequency survives. One symbol
        // is a short window for a Goertzel, so the bar is deliberately low —
        // this is a "did the tone survive at all" check, not a calibration.
        check(mark > 0.02, "mark tone retains a usable amplitude");
    }

    // ── 3. NO CLIPPING INTRODUCED ──────────────────────────────────────────
    float peak = 0.0f;
    for (float v : dst)
        peak = std::max(peak, std::abs(v));
    std::printf("peak: %.4f (input %.4f)\n", peak, kAmplitude);
    check(peak < 0.99f, "resampler did not drive the signal into clipping");

    // ── 4. THE LPCM ENCODE ─────────────────────────────────────────────────
    // encodeAudio() is what turns the resampled floats into the bytes that go
    // on the wire. It is twenty lines, but it is twenty lines carrying every
    // AFSK transition, and a sign or endianness error here would produce audio
    // that is loud, correctly timed and undecodable — the exact symptom.
    //
    // Round-trip through the matching decodeAudio() rather than eyeballing
    // bytes: the pair is what the radio and this client have to agree on, and
    // a symmetric error in both would still show up as a mismatch against the
    // original floats.
    using namespace AetherSDR::icom;
    const auto encoded = encodeAudio(AudioCodec::Lpcm1ch16, dst);
    check(encoded.size() == dst.size() * 2, "LPCM16 encodes two bytes per sample");

    const auto decoded = decodeAudio(AudioCodec::Lpcm1ch16, encoded);
    check(decoded.size() == dst.size(), "round trip preserves the sample count");

    double worst = 0.0;
    for (std::size_t i = 0; i < std::min(decoded.size(), dst.size()); ++i)
        worst = std::max(worst, std::abs(double(decoded[i]) - double(dst[i])));
    std::printf("lpcm round-trip: worst sample error %.6f (quantum %.6f)\n",
                worst, 1.0 / 32767.0);
    // One 16-bit quantum is 3.05e-5. Anything larger is a real encode fault,
    // not rounding.
    check(worst < 2.0 / 32767.0, "LPCM round trip is within one quantisation step");

    // The tone must survive the encode, not just the sample values — measure
    // the decoded audio the same way the resampled audio was measured.
    if (decoded.size() > static_cast<std::size_t>(atSample + win)) {
        const double m = toneMagnitude(decoded.data() + atSample, win, kMarkHz, kDstRate);
        const double s = toneMagnitude(decoded.data() + atSample, win, kSpaceHz, kDstRate);
        std::printf("after LPCM: mark=%.5f space=%.5f\n", m, s);
        check(m > s, "mark still dominates after the LPCM round trip");
    }

    // ── 5. THE PACKETISER ──────────────────────────────────────────────────
    // TxPacketizer::takeFrame() only emits whole 1920-byte frames and splits
    // each into two unequal chunks. Two things could silently eat AFSK here:
    // the "drop the OLDEST on overflow" rule (correct for voice, wrong for a
    // burst) and the short-frame discard at the tail.
    //
    // Feed the encoded audio in exactly as submit() would, drain as the 10 ms
    // pump does, and reassemble. What comes out must be a byte-identical
    // prefix of what went in — any reordering, gap or drop is a defect.
    TxPacketizer tx(AudioCodec::Lpcm1ch16);
    tx.submit(dst);
    std::vector<std::uint8_t> drained;
    for (auto chunks = tx.takeFrame(); !chunks.empty(); chunks = tx.takeFrame())
        for (const auto& c : chunks)
            drained.insert(drained.end(), c.bytes.begin(), c.bytes.end());

    std::printf("packetiser: submitted %zu bytes, drained %zu, residue %zu, lost %zu\n",
                encoded.size(), drained.size(), tx.pendingBytes(),
                encoded.size() - drained.size() - tx.pendingBytes());
    check(!drained.empty(), "packetiser emitted frames");
    check(drained.size() % kAudioFrameBytes == 0, "drained a whole number of frames");

    // ── 5b. THE REAL ARRIVAL PATTERN, NOT A BULK TRANSFER ──────────────────
    // The section above hands the whole burst over in ONE submit() and then
    // drains it. That is not how a transmission happens, and it is exactly the
    // case that hides the defect this test was written to find.
    //
    // On the air the modem emits ~30 chunks at 20 ms intervals (its own log
    // says "Sending AX.25 AFSK audio: 30 chunks at 20 ms") while onTxPump()
    // drains on a 10 ms timer. Feed and drain therefore INTERLEAVE, and a pump
    // tick that lands between two chunks finds less than a whole 1920-byte
    // frame — takeFrame() returns {} and the pump sends NOTHING that tick.
    //
    // That is silent: an empty takeFrame() is indistinguishable from "nothing
    // to send". What must NOT happen is audio going missing or arriving out of
    // order, because on the wire that is a burst whose preamble goes out and
    // whose information field does not.
    {
        TxPacketizer paced(AudioCodec::Lpcm1ch16);
        const std::size_t chunkSamples = kDstRate / 50;   // 20 ms at 48 kHz
        std::vector<std::uint8_t> pacedOut;
        std::size_t emptyTicks = 0, ticks = 0;
        for (std::size_t off = 0; off < dst.size(); off += chunkSamples) {
            const std::size_t n = std::min(chunkSamples, dst.size() - off);
            paced.submit(std::span<const float>(dst.data() + off, n));
            // Two pump ticks per 20 ms chunk — the real 10 ms cadence.
            for (int t = 0; t < 2; ++t) {
                ++ticks;
                auto chunks = paced.takeFrame();
                if (chunks.empty()) { ++emptyTicks; continue; }
                for (const auto& c : chunks)
                    pacedOut.insert(pacedOut.end(), c.bytes.begin(), c.bytes.end());
            }
        }
        for (auto chunks = paced.takeFrame(); !chunks.empty(); chunks = paced.takeFrame())
            for (const auto& c : chunks)
                pacedOut.insert(pacedOut.end(), c.bytes.begin(), c.bytes.end());

        std::printf("paced feed: %zu ticks, %zu found no whole frame, %zu bytes out "
                    "(bulk drained %zu)\n",
                    ticks, emptyTicks, pacedOut.size(), drained.size());

        // ⭐⭐ THE PACED FEED AND THE BULK DRAIN ARE NOT COMPARABLE, and that is
        // the finding — not a defect in either.
        //
        // The bulk section above hands 64960 bytes to a 24000-byte queue in ONE
        // submit(), so the overflow rule immediately discards 40960 bytes from
        // the FRONT and only 23040 survive. Paced 20 ms at a time the queue
        // never approaches the cap and nothing is dropped at all: 63360 bytes
        // out, 1600 residue, every byte accounted for.
        //
        // ⇒ THE 250 ms CAP IS AN ARTEFACT OF BULK-SUBMITTING IN A TEST. A real
        // transmission is paced, so overflow is NOT what puts a fragment of an
        // AX.25 burst on the air. Comparing the two byte-for-byte would only
        // assert that the bulk path loses its front, which it is designed to.
        check(pacedOut.size() > drained.size(),
              "paced feed carries MORE than the bulk drain — the cap only bites on a bulk submit");
        check(pacedOut.size() + paced.pendingBytes() == encoded.size(),
              "paced feed accounts for every byte submitted — no loss when the feed is paced");

        // The residue is the tail of a burst that is not a whole number of
        // frames (64960 / 1920 = 33.83), and takeFrame() refusing to emit a
        // short frame is deliberate: a short packet reads as a discontinuity to
        // the radio's jitter buffer. flushTxAudio() discards it on unkey, by
        // which point the closing flag is already on the air.
        check(paced.pendingBytes() < kAudioFrameBytes,
              "residue is less than one frame — a partial tail, not a backlog");

        // Half the pump ticks find no whole frame, because the feed is 20 ms
        // and the drain is 10 ms. That is BENIGN: takeFrame() pops whole frames
        // from the front of a FIFO, so an empty tick delays the next frame by
        // 10 ms and can neither drop nor reorder audio.
        check(emptyTicks > 0, "a 20 ms feed against a 10 ms pump does starve some ticks");
        check(pacedOut.size() % kAudioFrameBytes == 0,
              "every byte that left did so as a whole frame, in order");

        // ⭐⭐ DISTINCT frames, not just frames. A COUNT OF THINGS SENT CANNOT
        // DETECT THE SAME THING BEING SENT REPEATEDLY, and that is exactly the
        // failure this test missed for five live runs.
        //
        // Captured off the wire with tshark on 2026-08-17 (independent of AE),
        // one real AX.25 burst left the machine as:
        //
        //     0 1 2 3 4 5 [4 5 alternating x24] 24 25 26 27 28
        //
        // 35 frames, but only ELEVEN distinct, out of the 89 the modulator
        // built: four correct frames, then the two PREAMBLE frames repeated 24
        // times, then five data frames. Post frames 6-23 and 29-88 — the bulk
        // of the information field — never went out.
        //
        // Every level, tone and frame-count measurement called that healthy,
        // because the frames really were sent. They were duplicates.
        {
            std::set<std::vector<std::uint8_t>> distinct;
            const std::size_t frames = pacedOut.size() / kAudioFrameBytes;
            for (std::size_t f = 0; f < frames; ++f)
                distinct.emplace(pacedOut.begin() + f * kAudioFrameBytes,
                                 pacedOut.begin() + (f + 1) * kAudioFrameBytes);
            std::printf("paced feed: %zu frames out, %zu DISTINCT\n",
                        frames, distinct.size());

            // ⚠ AN ABSOLUTE THRESHOLD HERE WOULD BE MEASURING THE TEST SIGNAL,
            // NOT THE PACKETISER. This burst is deliberately preamble-heavy
            // (512 flag bits then 300 bits of (i/3)%2), so it is repetitive by
            // construction — 7 distinct frames of 33 is a property of the input.
            // I asserted "more than half must be distinct" and it failed on a
            // healthy path, which would have been a false alarm.
            //
            // The honest question is whether the packetiser PRESERVES the
            // distinctness it was handed. Count distinct frames in the ENCODED
            // input and require the output to carry the same number: fewer means
            // frames were merged, duplicated or dropped in transit.
            std::set<std::vector<std::uint8_t>> distinctIn;
            const std::size_t inFrames = encoded.size() / kAudioFrameBytes;
            for (std::size_t f = 0; f < inFrames; ++f)
                distinctIn.emplace(encoded.begin() + f * kAudioFrameBytes,
                                   encoded.begin() + (f + 1) * kAudioFrameBytes);
            std::printf("             input had %zu frames, %zu distinct\n",
                        inFrames, distinctIn.size());
            check(distinct.size() >= distinctIn.size() - 1,
                  "the paced feed preserves the DISTINCTNESS it was handed "
                  "(allowing one for the partial tail frame)");
        }
    }

    // ⭐⭐ A SINGLE SUBMIT LARGER THAN kMaxPendingBytes SILENTLY LOSES ITS FRONT.
    //
    // This whole 1.35 s burst went in as one call: 64960 bytes against a 24000
    // byte cap, so submit()'s "drop the OLDEST on overflow" rule discarded
    // 40960 bytes — 63 % of the audio, taken from the START — before takeFrame
    // was ever called. What survives begins mid-frame, with no preamble and no
    // opening flag, which is undecodable by any TNC while still sounding
    // exactly like packet on a receiver.
    //
    // That rule is right for VOICE, which is what the comment at the drop site
    // argues and what this path was built for: the freshest audio is what
    // matters and latency must not grow. It is wrong for a burst, where the
    // oldest bytes ARE the sync sequence.
    //
    // Whether production reaches this depends on pacing. The 10 ms pump
    // normally drains faster than the modem's 20 ms chunks arrive, so the queue
    // stays shallow — but onTxPaceTick() does CATCH-UP pacing: "when a tick
    // lands late this ships a larger chunk to refill the cushion". A GUI-thread
    // stall therefore produces exactly the oversized submit modelled here.
    //
    // Pinned as OBSERVED BEHAVIOUR, not as a passing grade. If someone bounds
    // the submit size or changes the drop to take from the BACK, this assertion
    // is the thing that should be updated.
    const std::size_t lost = encoded.size() - drained.size() - tx.pendingBytes();

    // ⭐ THE COUNTER MUST AGREE WITH THE ARITHMETIC. droppedBytes() is what the
    // backend logs from, and an instrument that under-reports is worse than
    // none: it would turn "this transmission lost its preamble" into silence.
    std::printf("drop counters: droppedBytes=%zu dropEvents=%zu\n",
                tx.droppedBytes(), tx.dropEvents());
    check(tx.droppedBytes() == lost,
          "droppedBytes() matches the bytes actually missing");
    check(tx.dropEvents() == 1, "one oversized submit is one drop event");
    check(encoded.size() > TxPacketizer::kMaxPendingBytes,
          "the test burst is genuinely larger than the queue (else it proves nothing)");
    check(lost > 0, "an oversized single submit DOES lose audio (documented drop-oldest rule)");
    check(drained.size() + tx.pendingBytes() <= TxPacketizer::kMaxPendingBytes,
          "what survives is bounded by the queue cap");

    // What DOES survive must be intact — a drop from the front is survivable in
    // principle, but reordering or corruption of the remainder would not be.
    // The surviving bytes are the TAIL of the encoded audio, so compare against
    // the matching offset rather than the start.
    const std::size_t dropOffset = encoded.size() - (drained.size() + tx.pendingBytes());
    bool tailIntact = true;
    for (std::size_t i = 0; i < drained.size() && tailIntact; ++i)
        tailIntact = (drained[i] == encoded[dropOffset + i]);
    check(tailIntact, "the surviving bytes are an intact, in-order tail of the input");

    // ⭐ THE OVERFLOW RULE, exercised deliberately. A 596 ms AX.25 burst is
    // 2.4x kMaxPendingBytes (250 ms), so if the pump ever stalls the packetiser
    // drops from the FRONT — taking the preamble with it and leaving a frame no
    // TNC can sync to. This does not happen in production (the 10 ms pump
    // drains faster than the 20 ms chunks arrive) but the behaviour is worth
    // pinning so a future pacing change cannot introduce it silently.
    TxPacketizer stalled(AudioCodec::Lpcm1ch16);
    for (int i = 0; i < 40; ++i)          // 40 x 20 ms = 800 ms, never drained
        stalled.submit(dst.data() ? std::span<const float>(dst.data(), std::min<std::size_t>(dst.size(), kDstRate / 50))
                                  : std::span<const float>{});
    std::printf("overflow: pending %zu bytes (cap %zu)\n",
                stalled.pendingBytes(), TxPacketizer::kMaxPendingBytes);
    check(stalled.pendingBytes() <= TxPacketizer::kMaxPendingBytes,
          "an undrained packetiser is bounded by kMaxPendingBytes");

    // ── 6. THREE BURSTS THROUGH ONE PERSISTENT RESAMPLER ───────────────────
    // IcomCivBackend builds m_txResampler once and never resets it — it is only
    // rebuilt when the sample RATE changes, so its filter history carries from
    // one transmission into the next. A capture taken after it showed bursts
    // growing 0.901 s -> 2.473 s -> 2.806 s across three SABMs, against 0.596 s
    // of real audio each, which is what raised the question.
    //
    // Ask it directly: push three identical bursts through ONE instance, with a
    // realistic idle gap between them, and require each to emit the same number
    // of samples. If it accumulates, this fails and the backend needs a reset on
    // unkey. If it does not, the growth came from somewhere else and this test
    // records that the resampler was ruled out rather than suspected.
    {
        Resampler shared(kSrcRate, kDstRate, 4096);
        std::size_t counts[3] = {0, 0, 0};
        for (int burst = 0; burst < 3; ++burst) {
            for (std::size_t off = 0; off < src.size(); off += blockSamples) {
                const int n = static_cast<int>(
                    std::min<std::size_t>(blockSamples, src.size() - off));
                counts[burst] += shared.process(src.data() + off, n).size()
                                 / sizeof(float);
            }
        }
        std::printf("three bursts through one resampler: %zu, %zu, %zu samples\n",
                    counts[0], counts[1], counts[2]);
        // The first burst is allowed to differ: r8brain consumes its filter
        // latency before emitting, so burst 1 is short by that priming cost.
        // Bursts 2 and 3 must match each other exactly — any growth there is
        // state accumulating across transmissions.
        check(counts[1] == counts[2],
              "a reused resampler emits the same length for successive bursts");
        check(counts[2] <= counts[1] + 64,
              "no per-burst growth in a reused resampler");
    }

    if (g_failures == 0) {
        std::printf("icom_tx_resample_ax25_test: all checks passed\n");
        return 0;
    }
    std::fprintf(stderr, "icom_tx_resample_ax25_test: %d failure(s)\n", g_failures);
    return 1;
}
