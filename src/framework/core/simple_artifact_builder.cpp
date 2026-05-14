#include "simple_artifact_builder.h"

void SimpleArtifactBuilder::reset()
{
    m_textureArtifact = {};
    m_descriptor = {};
    m_sharedHandle = 0U;
    m_cpuBitmap = QImage();
    m_customObject.reset();
}

bool SimpleArtifactBuilder::setTexture(GLuint textureId,
                                       const QSize &size,
                                       const QMap<QString, QVariant> &metadata,
                                       QString *error)
{
    if (textureId == 0U || !size.isValid()) {
        if (error) {
            *error = QStringLiteral("Invalid texture artifact.");
        }
        return false;
    }

    m_textureArtifact.textureId = textureId;
    m_textureArtifact.size = size;
    m_textureArtifact.metadata = metadata;
    m_sharedHandle = 0U;
    m_cpuBitmap = QImage();
    m_descriptor = {};
    m_descriptor.kind = QStringLiteral("texture");
    m_descriptor.logicalSize = size;
    m_descriptor.metadata = metadata;
    return true;
}

bool SimpleArtifactBuilder::setSharedHandle(quintptr handle,
                                            const QSize &size,
                                            const QMap<QString, QVariant> &metadata,
                                            QString *error)
{
    if (handle == 0U || !size.isValid()) {
        if (error) {
            *error = QStringLiteral("Invalid shared-handle artifact.");
        }
        return false;
    }

    m_textureArtifact.textureId = 0U;
    m_textureArtifact.size = size;
    m_textureArtifact.metadata = metadata;
    m_sharedHandle = handle;
    m_cpuBitmap = QImage();
    m_descriptor = {};
    m_descriptor.kind = QStringLiteral("shared_handle");
    m_descriptor.logicalSize = size;
    m_descriptor.metadata = metadata;
    m_descriptor.artifactKey = QStringLiteral("0x%1").arg(handle, 0, 16);
    return true;
}

bool SimpleArtifactBuilder::setCpuBitmap(const QImage &image,
                                         const QMap<QString, QVariant> &metadata,
                                         QString *error)
{
    if (image.isNull()) {
        if (error) {
            *error = QStringLiteral("Invalid CPU bitmap artifact.");
        }
        return false;
    }

    m_textureArtifact.textureId = 0U;
    m_textureArtifact.size = image.size();
    m_textureArtifact.metadata = metadata;
    m_sharedHandle = 0U;
    m_cpuBitmap = image;
    m_descriptor = {};
    m_descriptor.kind = QStringLiteral("cpu_bitmap");
    m_descriptor.logicalSize = image.size();
    m_descriptor.metadata = metadata;
    return true;
}

bool SimpleArtifactBuilder::setCustom(std::shared_ptr<void> object,
                                      const ArtifactDescriptor &descriptor,
                                      QString *error)
{
    if (!object) {
        if (error) {
            *error = QStringLiteral("Invalid custom artifact.");
        }
        return false;
    }

    m_customObject = std::move(object);
    m_sharedHandle = 0U;
    m_cpuBitmap = QImage();
    m_descriptor = descriptor;
    return true;
}

TextureArtifact SimpleArtifactBuilder::textureArtifact() const
{
    return m_textureArtifact;
}

ArtifactDescriptor SimpleArtifactBuilder::descriptor() const
{
    return m_descriptor;
}

ArtifactSnapshot SimpleArtifactBuilder::snapshot() const
{
    ArtifactSnapshot snapshot;
    snapshot.descriptor = m_descriptor;
    snapshot.textureId = m_textureArtifact.textureId;
    snapshot.sharedHandle = m_sharedHandle;
    snapshot.cpuBitmap = m_cpuBitmap;
    snapshot.customObject = m_customObject;
    return snapshot;
}
