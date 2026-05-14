#pragma once

#include "work_types.h"

#include <QImage>
#include <QString>

class IArtifactBuilder
{
public:
    virtual ~IArtifactBuilder() = default;

    virtual bool setTexture(GLuint textureId,
                            const QSize &size,
                            const QMap<QString, QVariant> &metadata,
                            QString *error) = 0;

    virtual bool setSharedHandle(quintptr handle,
                                 const QSize &size,
                                 const QMap<QString, QVariant> &metadata,
                                 QString *error) = 0;

    virtual bool setCpuBitmap(const QImage &image,
                              const QMap<QString, QVariant> &metadata,
                              QString *error) = 0;

    virtual bool setCustom(std::shared_ptr<void> object,
                           const ArtifactDescriptor &descriptor,
                           QString *error) = 0;

    virtual ArtifactSnapshot snapshot() const = 0;
};
