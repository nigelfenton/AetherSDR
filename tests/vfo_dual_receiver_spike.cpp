// SPIKE HARNESS — dual-receiver VFO flag layout.
//
// Builds two real VfoWidgets side by side: one as it renders today, one with
// the second-receiver row shown.  Measures both with the widget's own metrics
// and writes a PNG so the layout can be judged rather than imagined.
//
// WHY THIS EXISTS: a mockup cannot tell you whether text fits.  The Ulanzi
// advisory (#3485) shipped a status string that was measured at 1,647 px into
// a 212 px label and clipped mid-word, because it looked fine in a picture.
// The rule that came out of that is to measure in the real widget, in its own
// font, before believing a layout — which is what this does.
//
// NOT A REGRESSION TEST.  It asserts the invariants that matter (the single-
// receiver flag is unchanged; the second row actually fits) and otherwise
// exists to produce an image.  Run:
//
//   ./vfo_dual_receiver_spike            (offscreen, writes vfo-dual-spike.png)
//   ./vfo_dual_receiver_spike --show     (opens a window)

#include "gui/VfoWidget.h"

#include <QApplication>
#include <QFontMetrics>
#include <QHBoxLayout>
#include <QLabel>
#include <QPixmap>
#include <QTimer>
#include <QWidget>

#include <cstdio>
#include <cstring>

using AetherSDR::VfoWidget;

namespace {

int g_failures = 0;

void check(const char* what, bool ok, const char* detail = nullptr)
{
    std::printf("[ %s ] %s%s%s\n", ok ? "OK" : "FAIL", what,
                detail ? " — " : "", detail ? detail : "");
    if (!ok)
        ++g_failures;
}

}  // namespace

int main(int argc, char** argv)
{
    bool show = false;
    for (int i = 1; i < argc; ++i)
        if (std::strcmp(argv[i], "--show") == 0)
            show = true;

    QApplication app(argc, argv);

    // ---- the flag as it renders today -------------------------------------
    auto* before = new VfoWidget;
    before->setFixedWidth(300);

    // ---- the same flag with the second receiver shown ---------------------
    // A satellite pass: 2 m uplink on MAIN, 70 cm downlink on SUB.  The
    // receiving side is the one the waterfall and S-meter belong to.
    auto* after = new VfoWidget;
    after->setFixedWidth(300);
    after->setSecondReceiver(435.810000,
                             QStringLiteral("MAIN"), QStringLiteral("SUB"),
                             QStringLiteral("tx"),   QStringLiteral("rx"));

    // ---- invariant 1: today's flag is untouched ---------------------------
    check("single-receiver flag reports no second receiver",
          !before->hasSecondReceiver());
    check("dual flag reports a second receiver",
          after->hasSecondReceiver());

    before->adjustSize();
    after->adjustSize();
    const QSize sBefore = before->sizeHint();
    const QSize sAfter  = after->sizeHint();

    char buf[160];
    std::snprintf(buf, sizeof buf, "single %dx%d, dual %dx%d",
                  sBefore.width(), sBefore.height(),
                  sAfter.width(), sAfter.height());
    check("width is unchanged by the second row",
          sBefore.width() == sAfter.width(), buf);
    check("the second row costs height, not width",
          sAfter.height() > sBefore.height(), buf);

    // ---- invariant 2: THE ULANZI CHECK — does the text actually fit? ------
    // The failure this guards against is silent: a QLabel that neither wraps
    // nor elides just cuts mid-digit, and the automation bridge would still
    // read the full string back.
    if (QLabel* lbl = after->secondReceiverLabel()) {
        const QFontMetrics fm(lbl->font());
        const int needed = fm.horizontalAdvance(lbl->text());
        const int have   = lbl->width();
        std::snprintf(buf, sizeof buf, "\"%s\" needs %d px, label is %d px",
                      lbl->text().toUtf8().constData(), needed, have);
        check("second-receiver digits fit their label", needed <= have, buf);

        // Widest realistic case: a 4-digit MHz reading (23 cm / XVTR).
        const int widest = fm.horizontalAdvance(QStringLiteral("1296.000.000"));
        std::snprintf(buf, sizeof buf, "widest case 1296.000.000 needs %d px, label is %d px",
                      widest, have);
        check("widest plausible frequency still fits", widest <= have, buf);
    } else {
        check("second-receiver label exists", false);
    }

    // ---- invariant 3: hiding it restores the original exactly -------------
    after->setSecondReceiver(-1.0);
    after->adjustSize();
    check("hiding the second receiver restores the single-receiver height",
          after->sizeHint().height() == sBefore.height());
    after->setSecondReceiver(435.810000,
                             QStringLiteral("MAIN"), QStringLiteral("SUB"),
                             QStringLiteral("tx"),   QStringLiteral("rx"));
    after->adjustSize();

    // ---- render both, side by side ----------------------------------------
    auto* sheet = new QWidget;
    sheet->setStyleSheet("background:#000;");
    auto* row = new QHBoxLayout(sheet);
    row->setContentsMargins(24, 24, 24, 24);
    row->setSpacing(28);
    row->addWidget(before, 0, Qt::AlignTop);
    row->addWidget(after,  0, Qt::AlignTop);
    sheet->adjustSize();

    if (show) {
        sheet->show();
        return app.exec();
    }

    sheet->resize(sheet->sizeHint());
    // Let the event loop settle so stylesheets and font metrics are applied
    // before the grab — a grab on the first tick catches unstyled widgets.
    QTimer::singleShot(0, &app, [&] {
        const QPixmap shot = sheet->grab();
        const char* out = "vfo-dual-spike.png";
        if (shot.save(out))
            std::printf("\nwrote %s (%dx%d)\n", out, shot.width(), shot.height());
        else
            std::printf("\nFAILED to write %s\n", out);
        app.quit();
    });
    app.exec();

    if (g_failures == 0)
        std::printf("ALL PASS\n");
    else
        std::printf("%d FAILURE(S)\n", g_failures);
    return g_failures == 0 ? 0 : 1;
}
