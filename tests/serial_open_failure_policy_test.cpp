// #6972: a serial port that will not open must say why, keep the real error and
// the full path, and offer only the remedy that fits the failure.
#include "gui/SerialOpenFailurePolicy.h"

#include <QCoreApplication>
#include <QtSerialPort/QSerialPort>
#include <cstdio>

// The policy names Qt's error values itself so the GUI does not need the
// optional Qt6::SerialPort headers. This test DOES link it, so it is the place
// that pins those constants to the real enum: if Qt ever renumbers them, this
// fails to compile rather than misclassifying at runtime.
static_assert(static_cast<int>(QSerialPort::NoError)
                  == AetherSDR::kSerialNoError, "NoError drifted");
static_assert(static_cast<int>(QSerialPort::DeviceNotFoundError)
                  == AetherSDR::kSerialDeviceNotFoundError, "DeviceNotFoundError drifted");
static_assert(static_cast<int>(QSerialPort::PermissionError)
                  == AetherSDR::kSerialPermissionError, "PermissionError drifted");
static_assert(static_cast<int>(QSerialPort::ResourceError)
                  == AetherSDR::kSerialResourceError, "ResourceError drifted");

using namespace AetherSDR;

namespace {

int g_failures = 0;

void expect(bool condition, const char* what)
{
    std::printf("%s %s\n", condition ? "[ OK ]" : "[FAIL]", what);
    if (!condition) { ++g_failures; }
}

}  // namespace

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);

    const QString path = QStringLiteral("/dev/ttyUSB0");
    const QString denied = QStringLiteral("Permission denied");

    // --- classification -----------------------------------------------------
    expect(classifySerialOpenFailure(QSerialPort::PermissionError)
               == SerialOpenFailure::Permission,
           "PermissionError -> Permission");
    expect(classifySerialOpenFailure(QSerialPort::DeviceNotFoundError)
               == SerialOpenFailure::Missing,
           "DeviceNotFoundError -> Missing");
    expect(classifySerialOpenFailure(QSerialPort::ResourceError)
               == SerialOpenFailure::InUse,
           "ResourceError -> InUse");
    expect(classifySerialOpenFailure(QSerialPort::UnsupportedOperationError)
               == SerialOpenFailure::Other,
           "an unrelated error -> Other");
    expect(classifySerialOpenFailure(QSerialPort::NoError) == SerialOpenFailure::Other,
           "NoError is not a failure class of its own");
    // Measured on Aether-Linux, Qt 6.12.0: opening /dev/ttyUSB0 (FTDI, root:dialout
    // 0660) as a user outside dialout reports error 2, "Permission denied". The
    // raw value is asserted because that is what crosses portStateChanged.
    expect(classifySerialOpenFailure(2) == SerialOpenFailure::Permission,
           "the measured real-device value (2) classifies as Permission");

    // --- the real error survives, in every branch ---------------------------
    // The report is explicit: do not replace the actual error with a generic
    // "could not connect". Both the native text and the full path stay.
    for (const auto failure : {SerialOpenFailure::Permission, SerialOpenFailure::Missing,
                               SerialOpenFailure::InUse, SerialOpenFailure::Other}) {
        const QString detail =
            serialOpenFailureDetail(failure, path, denied, QStringLiteral("dialout"));
        expect(detail.contains(path), "the full device path is in the message");
        expect(detail.contains(denied), "the native error text is in the message");
    }

    // --- permission: the derived group, and the commands to fix it ----------
    {
        const QString d = serialOpenFailureDetail(SerialOpenFailure::Permission, path,
                                                  denied, QStringLiteral("dialout"));
        expect(d.contains(QStringLiteral("sudo usermod -aG dialout \"$USER\"")),
               "names the usermod command with the device's own group");
        expect(d.contains(QStringLiteral("sudo adduser \"$USER\" dialout")),
               "offers the Debian/Ubuntu alternative");
        expect(d.contains(QStringLiteral("log back in")),
               "says to log out and back in, not just open a new terminal");
    }
    {
        // Arch: the group comes from the device, so it must not be hard-coded.
        const QString d = serialOpenFailureDetail(SerialOpenFailure::Permission, path,
                                                  denied, QStringLiteral("uucp"));
        expect(d.contains(QStringLiteral("sudo usermod -aG uucp \"$USER\"")),
               "uses uucp when that is the device's group");
        expect(!d.contains(QStringLiteral("dialout")),
               "does not mention dialout for a uucp device");
    }
    {
        // Group unknown: say how to look it up rather than name a guess.
        const QString d = serialOpenFailureDetail(SerialOpenFailure::Permission, path, denied);
        expect(d.contains(QStringLiteral("ls -l /dev/ttyUSB0")),
               "with no group, shows how to inspect the device instead");
        expect(!d.contains(QStringLiteral("usermod")),
               "with no group, names no group-membership command");
    }

    // --- the remedy must fit: no group advice on the other failures ---------
    for (const auto failure : {SerialOpenFailure::Missing, SerialOpenFailure::InUse,
                               SerialOpenFailure::Other}) {
        const QString d =
            serialOpenFailureDetail(failure, path, denied, QStringLiteral("dialout"));
        expect(!d.contains(QStringLiteral("usermod")),
               "no group-membership advice outside a permission failure");
        expect(!d.contains(QStringLiteral("adduser")),
               "no adduser advice outside a permission failure");
    }

    // --- Other carries the native error and nothing invented ----------------
    {
        const QString d = serialOpenFailureDetail(SerialOpenFailure::Other, path,
                                                  QStringLiteral("Unknown error"));
        expect(d == QStringLiteral("Can't open /dev/ttyUSB0: Unknown error"),
               "an unclassified failure is exactly the native error, no remedy");
    }

    // --- status labels are short enough to sit beside the Open button -------
    for (const auto failure : {SerialOpenFailure::Permission, SerialOpenFailure::Missing,
                               SerialOpenFailure::InUse, SerialOpenFailure::Other}) {
        const QString s = serialOpenFailureStatus(failure);
        expect(!s.isEmpty() && s.length() <= 20, "status label is short and non-empty");
        expect(s != QStringLiteral("Open"),
               "a failed open never reads as Open (the #6972 symptom)");
    }

    std::printf("serial open failure policy: %d failures\n", g_failures);
    return g_failures ? 1 : 0;
}
