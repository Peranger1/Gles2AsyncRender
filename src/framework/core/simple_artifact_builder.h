#pragma once

#include "artifact_builder.h"

class SimpleArtifactBuilder final : public IArtifactBuilder
{
public:
    void reset();

    bool setTexture(GLuint textureId,
                    const QSize &size,
                    const QMap<QString, QVariant> &metadata,
                    QString *error) override;

    bool setSharedHandle(quintptr handle,
                         const QSize &size,
                         const QMap<QString, QVariant> &metadata,
                         QString *error) override;

    bool setCpuBitmap(const QImage &image,
                      const QMap<QString, QVariant> &metadata,
                      QString *error) override;

    bool setCustom(std::shared_ptr<void> object,
                   const ArtifactDescriptor &descriptor,
                   QString *error) override;

    TextureArtifact textureArtifact() const;
    ArtifactDescriptor descriptor() const;
    ArtifactSnapshot snapshot() const override;

private:
    TextureArtifact m_textureArtifact;
    ArtifactDescriptor m_descriptor;
    quintptr m_sharedHandle = 0U;
    QImage m_cpuBitmap;
    std::shared_ptr<void> m_customObject;
};
