#pragma once

#include <QCoreApplication>
#include <QString>
#include <QStringList>

// Deliberately no <QSerialPort> include. Qt6::SerialPort is optional
// (CMakeLists.txt: REQUIRE_SERIALPORT defaults OFF) and the GUI must build
// without it, so the classifier takes the error as a plain int — which is also
// what SerialPortController::portStateChanged carries, for the same reason. The
// three values below are Qt's own; the Win32 path maps its failures onto them so
// one classifier serves both platforms.

namespace AetherSDR {

// What to tell the operator when a serial port will not open (#6972).
//
// The controller knew the reason all along — QSerialPort::error() plus
// errorString() — and threw it away: errorOccurred had no receiver, so a failed
// open reached the user as nothing but an aether.devices log line while Radio
// Setup still read "Open". This turns that reason into one message.
//
// Qt-object-free and header-only so the wording is testable without a serial
// device, a GUI, or a Linux box: every branch is reachable from a
// (SerialPortError, path, group) triple.
//
// Two rules from the report drive the shape:
//   - Keep the real error. The native text and the full device path are always
//     in the message; nothing is replaced by a generic "could not connect".
//   - Only offer the remedy that fits. Group advice appears for a permission
//     failure and nowhere else, so a missing or busy device never tells someone
//     to join a group that would not have helped.
//
// The group is DERIVED, not assumed: the caller reads the device node's owning
// group and passes it in. docs/user/docs/linux.md records why that matters —
// it is `uucp` on Arch and `dialout` on Debian/Ubuntu/Fedora. When it cannot be
// read, the message says how to look it up instead of naming a guess.
enum class SerialOpenFailure {
    Permission,   // EACCES — the node exists, this user may not open it
    Missing,      // the node is not there (unplugged, wrong name, udev renamed it)
    InUse,        // another process (or a stale lock file) holds it
    Other,        // anything else: keep the native text, offer no remedy
};

// QSerialPort::SerialPortError values, named here so the classifier does not
// need the optional Qt6::SerialPort headers. Verified against Qt 6.12.0 on
// Linux: opening a root:dialout 0660 node as a non-member reports error 2 with
// errorString() "Permission denied".
// Values read from Qt 6.12.0's qserialport.h, in declaration order:
// NoError 0, DeviceNotFoundError 1, PermissionError 2, OpenError 3,
// WriteError 4, ReadError 5, ResourceError 6, UnsupportedOperationError 7.
// serial_open_failure_policy_test static_asserts each one against the real enum,
// so a Qt that renumbered them would fail the build rather than misclassify.
enum SerialPortErrorValue {
    kSerialNoError = 0,
    kSerialDeviceNotFoundError = 1,
    kSerialPermissionError = 2,
    kSerialResourceError = 6,
};

inline SerialOpenFailure classifySerialOpenFailure(int error)
{
    switch (error) {
    case kSerialPermissionError:
        return SerialOpenFailure::Permission;
    case kSerialDeviceNotFoundError:
        return SerialOpenFailure::Missing;
    // Qt reports a port already held as ResourceError on some platforms and as
    // PermissionError on others. Only ResourceError is unambiguous here;
    // PermissionError stays above, where the group advice is, because that is
    // the reading that helps the operator on Linux, which is what #6972 is about.
    case kSerialResourceError:
        return SerialOpenFailure::InUse;
    default:
        return SerialOpenFailure::Other;
    }
}

// The short line for the Radio Setup status label. Deliberately not the dialog
// text: the label sits beside the Open button and has room for a few words.
inline QString serialOpenFailureStatus(SerialOpenFailure failure)
{
    switch (failure) {
    case SerialOpenFailure::Permission:
        return QCoreApplication::translate("SerialOpenFailure", "Permission denied");
    case SerialOpenFailure::Missing:
        return QCoreApplication::translate("SerialOpenFailure", "Not found");
    case SerialOpenFailure::InUse:
        return QCoreApplication::translate("SerialOpenFailure", "In use");
    case SerialOpenFailure::Other:
        break;
    }
    return QCoreApplication::translate("SerialOpenFailure", "Failed");
}

// The dialog body. `path` is the full device path as it was attempted, `native`
// is QSerialPort::errorString(), and `deviceGroup` is the node's owning group
// when the caller could read it (empty otherwise).
//
// Everything after the first line is remedy, so a caller that only wants the
// failure itself can show the first paragraph alone.
inline QString serialOpenFailureDetail(SerialOpenFailure failure,
                                       const QString& path,
                                       const QString& native,
                                       const QString& deviceGroup = QString())
{
    const QString head = QCoreApplication::translate(
        "SerialOpenFailure", "Can't open %1: %2").arg(path, native);

    switch (failure) {
    case SerialOpenFailure::Permission: {
        if (deviceGroup.isEmpty()) {
            // No group to name: say how to find it rather than guess one. Naming
            // the wrong group sends someone to log out and back in for nothing.
            return head + QStringLiteral("\n\n")
                + QCoreApplication::translate(
                      "SerialOpenFailure",
                      "Your user account is not allowed to open this device. Check who owns "
                      "it and which group may use it:\n\n"
                      "    ls -l %1\n\n"
                      "then add yourself to that group, log out of your desktop session and "
                      "log back in, and start AetherSDR again.").arg(path);
        }
        return head + QStringLiteral("\n\n")
            + QCoreApplication::translate(
                  "SerialOpenFailure",
                  "This device belongs to the \"%1\" group, which your user account is not "
                  "in. Add yourself to it:\n\n"
                  "    sudo usermod -aG %1 \"$USER\"\n\n"
                  "On Debian and Ubuntu this also works:\n\n"
                  "    sudo adduser \"$USER\" %1\n\n"
                  "Then log out of your desktop session and log back in — a new terminal is "
                  "not enough — and start AetherSDR again.").arg(deviceGroup);
    }
    case SerialOpenFailure::Missing:
        return head + QStringLiteral("\n\n")
            + QCoreApplication::translate(
                  "SerialOpenFailure",
                  "The device is not there. Check that it is plugged in, and that the port "
                  "name still matches — USB serial adapters can come back on a different "
                  "name after a replug.");
    case SerialOpenFailure::InUse:
        return head + QStringLiteral("\n\n")
            + QCoreApplication::translate(
                  "SerialOpenFailure",
                  "Another program has the port open. Close anything else using it — a "
                  "terminal program, a rig-control daemon, a second copy of AetherSDR — and "
                  "try again.");
    case SerialOpenFailure::Other:
        break;
    }
    // No remedy we can stand behind: the native error is the whole message.
    return head;
}

}  // namespace AetherSDR
