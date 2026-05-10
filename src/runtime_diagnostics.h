#pragma once

#include <QString>

namespace RuntimeDiagnostics
{
bool isEnabled();

void logInfo(const char *scope, const QString &message);
void logWarning(const char *scope, const QString &message);
void logDiag(const char *scope, const QString &message);
}

