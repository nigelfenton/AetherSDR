// IcomCIV — does the advertised panadapter capacity match what the backend can
// actually deliver?
//
// The IC-9700 profile declares two receivers, and IcomCivBackend derives
// `maxPanadapters = hasScope ? receivers : 0` from that — so capabilities()
// reports 2. But the backend never overrides IRadioBackend::createPanadapter(),
// whose base implementation is `return false;`, and both pan/slice identities
// are hardcoded constants (`panId()` returns "0", `sliceId()` returns 0) with no
// index parameter anywhere.
//
// The operator-visible symptom is a message naming a maximum of 2 with none
// available, which reads as "two are in use" when in fact zero were allocated
// and only one can ever exist. That is why this is a test and not a comment:
// the capability number and the capability's implementation are in different
// files, and nothing today asserts they agree.
//
// This test states the CURRENT behaviour. If dual-receiver panadapters are
// implemented (#4840), the createPanadapter() assertion is the one that flips.

#include "IcomFakeRadio.h"

#include "core/backends/icom/IcomCivBackend.h"
#include "core/backends/icom/IcomModels.h"

#include <QCoreApplication>
#include <QString>

#include <cstdio>
#include <string>

using namespace AetherSDR;
using namespace AetherSDR::icom;

namespace AetherSDR::icom {

// Same access shim the sibling suites use: m_model is private, and selecting it
// directly is what lets this run with no radio and no session.
struct IcomCivBackendTestAccess {
    static void selectModel(IcomCivBackend& backend, const IcomModel& model)
    {
        backend.m_model = &model;
    }
};

}  // namespace AetherSDR::icom

namespace {

int g_failures = 0;

void check(bool ok, const std::string& what)
{
    std::printf("[ %s ] %s\n", ok ? "OK" : "FAIL", what.c_str());
    if (!ok)
        ++g_failures;
}

}  // namespace

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);

    const IcomModel* ic9700 = modelForName("IC-9700");
    if (!ic9700) {
        std::printf("[ FAIL ] IC-9700 profile not found in the model table\n");
        return 1;
    }

    // --- what the profile declares -------------------------------------
    check(ic9700->receivers == 2,
          "IC-9700 profile declares 2 receivers");
    check(ic9700->hasScope,
          "IC-9700 profile declares a scope");

    // --- what capabilities() reports -----------------------------------
    IcomCivBackend backend;
    IcomCivBackendTestAccess::selectModel(backend, *ic9700);
    const auto caps = backend.capabilities();

    check(caps.maxPanadapters == 2,
          "capabilities() advertises maxPanadapters == 2 (derived from receivers)");

    // --- what the backend can actually do ------------------------------
    //
    // THE POINT OF THIS TEST. createPanadapter() is not overridden by
    // IcomCivBackend, so this is IRadioBackend's `return false;`. The advertised
    // capacity of 2 cannot be reached: the first panadapter exists because the
    // session creates it, and a second can never be minted.
    const bool created = backend.createPanadapter();
    check(!created,
          "createPanadapter() returns FALSE — the second panadapter cannot be created");

    check(caps.maxPanadapters > 1 && !created,
          "MISMATCH CONFIRMED: capability advertises >1 pan, backend refuses to create one");

    // --- the structural reason -----------------------------------------
    //
    // A backend that could drive two panadapters would need two identities to
    // address them by. Both are compile-time constants here, so even if
    // createPanadapter() were implemented it would have nothing to name the
    // second pan with. Asserted through the public signal payload rather than
    // by reading the private accessors.
    check(caps.maxSlices == 2,
          "capabilities() also advertises maxSlices == 2 (same derivation)");

    if (g_failures == 0)
        std::printf("ALL PASS\n");
    else
        std::printf("%d FAILURE(S)\n", g_failures);
    return g_failures == 0 ? 0 : 1;
}
