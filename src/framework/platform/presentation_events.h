#pragma once

#include "writer.h"

#include <QObject>

class PlatformPresentationEvents final : public QObject, public IWriterEvents
{
    Q_OBJECT

public:
    explicit PlatformPresentationEvents(QObject *parent = nullptr)
        : QObject(parent)
    {
    }

    void onTextureReady(const TextureTicket &ticket) override
    {
        emit textureReady(ticket);
    }

    void onWarning(const QString &reason) override
    {
        emit warning(reason);
    }

signals:
    void publishCapacityAvailable();
    void textureReady(const TextureTicket &ticket);
    void warning(const QString &reason);
};
