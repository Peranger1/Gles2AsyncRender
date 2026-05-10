#include "runtime_diagnostics.h"

#include <QByteArray>
#include <QDebug>

namespace RuntimeDiagnostics
{
namespace
{
bool parseEnabledValue(const QByteArray &value)
{
    if (value.isEmpty()) {
        return false;
    }

    const QByteArray normalized = value.trimmed().toLower();
    return normalized != "0"
        && normalized != "false"
        && normalized != "off"
        && normalized != "no";
}
}

bool isEnabled()
{
    static const bool enabled = parseEnabledValue(qgetenv("GLES2ASYNC_DIAG"));
    return enabled;
}

void logInfo(const char *scope, const QString &message)
{
    if (!message.isEmpty()) {
        qInfo().noquote() << scope << message;
    }
}

void logWarning(const char *scope, const QString &message)
{
    if (!message.isEmpty()) {
        qWarning().noquote() << scope << message;
    }
}

void logDiag(const char *scope, const QString &message)
{
    if (isEnabled()) {
        logInfo(scope, message);
    }
}
}
