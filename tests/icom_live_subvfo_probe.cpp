// IcomCIV live SUB-VFO probe — REQUIRES A REAL RADIO.  EXCLUDE_FROM_ALL and
// deliberately not registered with add_test(), like its two sibling probes.
//
//   cmake --build build --target icom_live_subvfo_probe
//   ICOM_USER=... ICOM_PW=... ./build/icom_live_subvfo_probe <host> [civ-hex]
//
// CREDENTIALS COME FROM THE ENVIRONMENT, NOT argv — a password in argv is
// readable through /proc/<pid>/cmdline and lands in shell history.  Same rule
// as icom_live_civ_probe, and for the same reason.
//
// ── WHY THIS EXISTS ──────────────────────────────────────────────────────
//
// #5348 proposes showing both IC-9700 receivers in one VFO flag.  The layout
// is built and proven; what is NOT known is whether the data can be had at
// all.  IcomCivBackend decodes the SELECTED VFO only — IcomCivBackend.cpp:2437,
// "A reply for the unselected one describes a VFO the app does not model" —
// and CI-V 0x25 (read/set frequency of the un/selected VFO) is not implemented
// anywhere in the tree.
//
// Before anyone refactors a 6,248-line backend across 49 hardcoded sliceId()
// sites, four questions want answering ON HARDWARE:
//
//   Q1  Does 0x25 work at all?  Asked for the SELECTED VFO first, and checked
//       against what 0x03 independently reports.  A codec validated against a
//       known answer before it is trusted for an unknown one.
//   Q2  Does the UNSELECTED VFO answer?  0x25 01.  Checked for a REAL reply,
//       not merely the absence of an error: this radio NAKs `fa` on some
//       requests while the layer above still reports success, so "no error"
//       proves nothing.
//   Q3  Does it TRACK?  One reading could be a stale echo.  The operator turns
//       the Sub dial and the probe reports whether the value follows.
//   Q4  What does the extra polling COST?  0x25 issued at a realistic cadence
//       while the session runs, measuring reply latency and misses.  This is
//       the question that decides whether the scheduler work in #5348 is half
//       a day or two — and it is the one that cannot be answered from source.
//
// ── SAFETY ───────────────────────────────────────────────────────────────
//
// RX-ONLY.  There is no PTT, no MOX, no TUNE and no frequency WRITE anywhere
// below; every 0x25 issued is the READ form (no payload).  The probe never
// changes what the radio is doing.
//
// ⚠ ONE SESSION, REUSED.  Rapid RS-BA1 session churn drove a lab IC-9700 into
// a CI-V stall that outlasted an 8-minute backoff and cleared only on a power
// cycle (measured 2026-08-14).  All four questions are answered inside a
// single connect for that reason — do not "helpfully" split them into separate
// runs.
//
// ⚠ CHECK THE FREQUENCIES BEFORE RUNNING.  Read-only or not, know where the
// radio is parked before starting a session against it.

#include "core/backends/icom/CivCodec.h"
#include "core/backends/icom/IcomCivBackend.h"
#include "core/backends/icom/IcomSession.h"

#include <QCoreApplication>
#include <QElapsedTimer>

#include <cstdio>
#include <cstdlib>
#include <optional>
#include <vector>

using namespace AetherSDR;
using namespace AetherSDR::icom;

namespace AetherSDR::icom {

// The backend owns its session privately; the existing friend declaration is
// how the sibling suites reach in.  Used here to put a raw frame on the wire
// and to learn the resolved CI-V address, neither of which has a public path.
struct IcomCivBackendTestAccess {
    static IcomSession* session(IcomCivBackend& b) { return b.m_session.get(); }
    // Mirrors what the backend itself addresses frames to: the address adopted
    // from a 0x19 0x00 reply this session if there is one, otherwise the seed
    // the session opened with.  ⚠ m_civAmbiguous means TWO different addresses
    // answered — on a bus fronted by RS-BA1 the second responder may be a
    // rotator or an amp — so the probe refuses to guess in that case.
    static std::uint8_t civAddress(const IcomCivBackend& b)
    {
        if (b.m_civAmbiguous) return 0;
        return b.m_civReported ? b.m_civReported : b.m_civSeedAddress;
    }
    static bool civAmbiguous(const IcomCivBackend& b) { return b.m_civAmbiguous; }
};

}  // namespace AetherSDR::icom

namespace {

// CI-V 0x25: read the frequency of the selected (00) or unselected (01) VFO.
// The READ form carries no payload — a data byte would make it a WRITE, which
// this probe must never send.
//
// ⚠ MEASURED 2026-08-31, AND IT IS NOT WHAT #5348 ASSUMED.  On an IC-9700,
// 0x25 01 returns the other VFO **of the current receiver** — not the Sub
// receiver.  Confirmed against the radio's own display: with Main on 145.070
// and Sub on 435.825, `0x25 01` answered 146.520, which is what Main's VFO B
// was showing.  The command works and decodes correctly; it addresses the
// wrong axis.  Main/Sub is a RECEIVER selection (0x07 D0/D1), which is not
// implemented anywhere in this tree — and being a *selection*, polling it
// would repeatedly change which receiver is active under the operator.
constexpr std::uint8_t kCmdVfoFreq   = 0x25;
constexpr std::uint8_t kSubSelected   = 0x00;
constexpr std::uint8_t kSubUnselected = 0x01;

struct Reply {
    bool     arrived = false;
    bool     nak     = false;
    double   mhz     = 0.0;
    qint64   latencyMs = 0;
};

int g_failures = 0;

void verdict(const char* q, bool ok, const char* detail)
{
    std::printf("[ %s ] %s%s%s\n", ok ? "OK" : "??", q,
                detail && *detail ? " — " : "", detail ? detail : "");
    if (!ok) ++g_failures;
}

void pump(int ms)
{
    QElapsedTimer t; t.start();
    while (t.elapsed() < ms)
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
}

}  // namespace

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);

    const QString user = QString::fromLocal8Bit(qgetenv("ICOM_USER"));
    const QString pass = QString::fromLocal8Bit(qgetenv("ICOM_PW"));
    if (argc < 2 || user.isEmpty() || pass.isEmpty()) {
        std::fprintf(stderr,
            "usage: ICOM_USER=... ICOM_PW=... %s <host> [civ-hex]\n"
            "\n"
            "  RX-ONLY. Issues CI-V 0x25 READ frames only; never writes a\n"
            "  frequency and never keys the radio.\n"
            "\n"
            "  Q3 needs a person at the radio: when prompted, turn the SUB\n"
            "  dial. Everything else runs unattended.\n", argv[0]);
        return 2;
    }

    const QString host = QString::fromLocal8Bit(argv[1]);
    const bool haveCiv = argc > 2;
    const unsigned civArg = haveCiv ? std::strtoul(argv[2], nullptr, 16) : 0;

    IcomCivBackend backend;

    bool     connected = false;
    QString  connectError;
    double   seenSelectedFreq = 0.0;   // via the backend's own 0x03 path

    // Raw 0x25 replies, captured off the session before the backend discards
    // them (the backend drops unselected-VFO replies by design).
    std::vector<CivFrame> raw25;
    QElapsedTimer sinceSend;

    QObject::connect(&backend, &IRadioBackend::connected, &app,
                     [&] { connected = true; });
    QObject::connect(&backend, &IRadioBackend::connectionError, &app,
                     [&](const QString& m) { connectError = m; });
    QObject::connect(&backend, &IRadioBackend::sliceChanged, &app,
                     [&](int, const SliceDelta& d) {
                         if (d.frequency) seenSelectedFreq = *d.frequency;
                     });

    RadioConnectRequest req;
    req.host = host;
    req.params.insert(QStringLiteral("icom.username"), user);
    req.params.insert(QStringLiteral("icom.password"), pass);
    if (haveCiv)
        req.params.insert(QStringLiteral("icom.civAddress"), civArg);

    std::printf("== IC-9700 sub-VFO probe ==\n");
    std::printf("host=%s civ=%s   RX-ONLY, 0x25 READ frames only\n\n",
                qPrintable(host),
                haveCiv ? qPrintable(QString::number(civArg, 16).toUpper())
                        : "(auto)");

    backend.connectRadio(req);

    QElapsedTimer clock; clock.start();
    while (clock.elapsed() < 20000 && seenSelectedFreq == 0.0 && connectError.isEmpty())
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);

    if (!connectError.isEmpty()) {
        std::printf("CONNECT FAILED: %s\n", qPrintable(connectError));
        return 1;
    }
    if (seenSelectedFreq == 0.0) {
        std::printf("NO STATE ARRIVED — session connected but deaf; wrong CI-V "
                    "address is the usual cause. Nothing below would mean anything.\n");
        return 1;
    }
    std::printf("session live: selected VFO reads %.6f MHz (via 0x03)\n\n",
                seenSelectedFreq);

    IcomSession* sess = IcomCivBackendTestAccess::session(backend);
    const std::uint8_t civ = IcomCivBackendTestAccess::civAddress(backend);
    if (!sess) {
        std::printf("no session handle — cannot issue raw frames\n");
        return 1;
    }
    if (IcomCivBackendTestAccess::civAmbiguous(backend) || civ == 0) {
        std::printf("CI-V address is ambiguous or unresolved — two responders "
                    "answered, or none did.\nRe-run with an explicit civ-hex "
                    "argument; guessing here would address the wrong device.\n");
        return 1;
    }
    std::printf("addressing CI-V 0x%02X\n\n", civ);

    // Watch the wire directly: the backend deliberately drops replies for the
    // unselected VFO, so Q2 can only be answered upstream of it.
    QObject::connect(sess, &IcomSession::civFrameReady, &app,
                     [&](const CivFrame& f) {
                         if (f.cmd == kCmdVfoFreq || f.isNg())
                             raw25.push_back(f);
                     });

    const auto ask = [&](std::uint8_t sub, int waitMs) -> Reply {
        raw25.clear();
        sinceSend.restart();
        sess->sendCiv(buildFrameSub(civ, kCmdVfoFreq, sub, {}));
        pump(waitMs);
        Reply r;
        for (const CivFrame& f : raw25) {
            if (f.isNg()) { r.nak = true; continue; }
            if (f.cmd != kCmdVfoFreq) continue;
            if (const auto hz = decodeFreq(f.data)) {
                r.arrived = true;
                r.mhz = static_cast<double>(*hz) / 1e6;
                r.latencyMs = sinceSend.elapsed();
                break;
            }
        }
        return r;
    };

    // ── Q1: does 0x25 work at all? ───────────────────────────────────────
    // Validated against 0x03's answer, so a decode bug cannot masquerade as a
    // radio behaviour.
    const Reply sel = ask(kSubSelected, 2500);
    char buf[220];
    if (sel.arrived) {
        const double delta = sel.mhz - seenSelectedFreq;
        std::snprintf(buf, sizeof buf,
                      "0x25 00 -> %.6f MHz, 0x03 -> %.6f MHz (delta %.6f), %lld ms",
                      sel.mhz, seenSelectedFreq, delta, (long long)sel.latencyMs);
        verdict("Q1 0x25 works and agrees with 0x03",
                std::abs(delta) < 0.000002, buf);
    } else {
        std::snprintf(buf, sizeof buf, "no reply%s", sel.nak ? " (radio NAKed)" : "");
        verdict("Q1 0x25 works and agrees with 0x03", false, buf);
        std::printf("\n  0x25 is unsupported on this radio/firmware. #5348's\n"
                    "  approach needs a different command; stop here.\n");
        return 1;
    }

    // ── Q2: does the UNSELECTED VFO answer? ──────────────────────────────
    const Reply unsel = ask(kSubUnselected, 2500);
    if (unsel.arrived) {
        std::snprintf(buf, sizeof buf, "0x25 01 -> %.6f MHz, %lld ms",
                      unsel.mhz, (long long)unsel.latencyMs);
        verdict("Q2 unselected VFO reports a frequency", true, buf);
        std::printf("      ⚠ READ THIS AS VFO B OF THE CURRENT RECEIVER, not as Sub.\n"
                    "        Verified 2026-08-31 against the radio's display: Main\n"
                    "        145.070 / Sub 435.825 / Main-VFO-B 146.520, and this\n"
                    "        command answered 146.520.  If the value below is not\n"
                    "        what the SUB receiver shows, that is the expected\n"
                    "        result and not a fault.\n");
        if (std::abs(unsel.mhz - sel.mhz) < 0.000002)
            std::printf("      ⚠ SAME as the selected VFO — either both are genuinely\n"
                        "        on one frequency, or 01 is being ignored and echoing\n"
                        "        00. Q3 distinguishes these.\n");
    } else {
        std::snprintf(buf, sizeof buf, "no reply%s", unsel.nak ? " (radio NAKed)" : "");
        verdict("Q2 unselected VFO reports a frequency", false, buf);
        std::printf("\n  The second receiver cannot be read this way. #5348's data\n"
                    "  path needs rethinking before any backend work.\n");
        return 1;
    }

    // ── Q3: does it TRACK? ───────────────────────────────────────────────
    std::printf("\n-- Q3: turn the SUB dial now (20 s) --\n");
    const double before = unsel.mhz;
    double after = before;
    QElapsedTimer q3; q3.start();
    while (q3.elapsed() < 20000) {
        pump(1500);
        const Reply r = ask(kSubUnselected, 800);
        if (r.arrived) {
            after = r.mhz;
            if (std::abs(after - before) > 0.000002) break;
        }
    }
    std::snprintf(buf, sizeof buf, "%.6f -> %.6f MHz", before, after);
    verdict("Q3 unselected VFO tracks the dial",
            std::abs(after - before) > 0.000002, buf);

    // ── Q4: what does the polling cost? ──────────────────────────────────
    // A realistic cadence for a UI that wants the second frequency live.
    std::printf("\n-- Q4: 30 s at 4 Hz --\n");
    int sent = 0, got = 0;
    qint64 total = 0, worst = 0;
    QElapsedTimer q4; q4.start();
    while (q4.elapsed() < 30000) {
        const Reply r = ask(kSubUnselected, 250);
        ++sent;
        if (r.arrived) { ++got; total += r.latencyMs; worst = qMax(worst, r.latencyMs); }
    }
    const double lossPct = sent ? 100.0 * (sent - got) / sent : 100.0;
    std::snprintf(buf, sizeof buf,
                  "%d sent, %d answered (%.1f%% lost), mean %lld ms, worst %lld ms",
                  sent, got, lossPct,
                  (long long)(got ? total / got : 0), (long long)worst);
    verdict("Q4 polling is affordable at 4 Hz", lossPct < 5.0, buf);

    // Did the session survive its own probing?
    std::printf("\nsession after the run: %s\n",
                connectError.isEmpty() ? "healthy" : qPrintable(connectError));

    backend.disconnectRadio();
    pump(1200);

    std::printf("\n%s\n", g_failures == 0
        ? "ALL FOUR ANSWERED — #5348's data path is viable; the remaining work "
          "is modelling a second receiver."
        : "SOMETHING IS UNANSWERED — see the ?? lines above before writing code.");
    return g_failures == 0 ? 0 : 1;
}
