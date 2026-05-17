#include "mac_cocoa_gl_texture_writer.h"

#include "framework/execution/runtime_host.h"
#include "framework/platform/gl_types.h"
#include "framework/platform/runtime.h"

#include <QOpenGLContext>
#include <QOpenGLFunctions>
#include <QByteArray>
#include <QSize>
#include <QString>
#include <QVector>

#include <atomic>

#if defined(Q_OS_MACOS)
#include <CoreFoundation/CoreFoundation.h>
#include <IOSurface/IOSurface.h>
#include <OpenGL/CGLIOSurface.h>
#include <OpenGL/OpenGL.h>
#endif

namespace
{
enum class SubmitTextureResultKind
{
    Published,
    Queued,
    Failed
};

struct SubmitTextureResult final
{
    SubmitTextureResultKind kind = SubmitTextureResultKind::Failed;
    TextureTicket ticket;
    QString error;
};

struct DrainPendingPublishesResult final
{
    QVector<TextureTicket> tickets;
    QString error;
};

struct PendingLatestFrame final
{
    GLuint textureId = 0U;
    QSize size;
    quint64 outputRevision = 0;
    bool valid = false;
};

constexpr GLfloat kVertices[] = {
    -1.0f, -1.0f,
     1.0f, -1.0f,
    -1.0f,  1.0f,
     1.0f,  1.0f
};

constexpr GLfloat kTexCoords2D[] = {
    0.0f, 1.0f,
    1.0f, 1.0f,
    0.0f, 0.0f,
    1.0f, 0.0f
};

QSize sanitizedSize(const QSize &size)
{
    return QSize(qMax(1, size.width()), qMax(1, size.height()));
}

#if defined(Q_OS_MACOS)
CFNumberRef createNumber(int value)
{
    return CFNumberCreate(kCFAllocatorDefault, kCFNumberIntType, &value);
}

IOSurfaceRef createSurface(const QSize &size)
{
    const int width = qMax(1, size.width());
    const int height = qMax(1, size.height());
    const int bytesPerElement = 4;
    const int bytesPerRow = width * bytesPerElement;

    CFNumberRef widthValue = createNumber(width);
    CFNumberRef heightValue = createNumber(height);
    CFNumberRef bytesPerElementValue = createNumber(bytesPerElement);
    CFNumberRef bytesPerRowValue = createNumber(bytesPerRow);
    const void *keys[] = {
        kIOSurfaceWidth,
        kIOSurfaceHeight,
        kIOSurfaceBytesPerElement,
        kIOSurfaceBytesPerRow,
        kIOSurfaceOpenGLTextureCompatibility,
        kIOSurfaceOpenGLFBOCompatibility
    };
    const void *values[] = {
        widthValue,
        heightValue,
        bytesPerElementValue,
        bytesPerRowValue,
        kCFBooleanTrue,
        kCFBooleanTrue
    };

    CFDictionaryRef properties = CFDictionaryCreate(kCFAllocatorDefault,
                                                    keys,
                                                    values,
                                                    int(sizeof(keys) / sizeof(keys[0])),
                                                    &kCFTypeDictionaryKeyCallBacks,
                                                    &kCFTypeDictionaryValueCallBacks);
    IOSurfaceRef surface = properties ? IOSurfaceCreate(properties) : nullptr;

    if (properties != nullptr) {
        CFRelease(properties);
    }
    if (widthValue != nullptr) {
        CFRelease(widthValue);
    }
    if (heightValue != nullptr) {
        CFRelease(heightValue);
    }
    if (bytesPerElementValue != nullptr) {
        CFRelease(bytesPerElementValue);
    }
    if (bytesPerRowValue != nullptr) {
        CFRelease(bytesPerRowValue);
    }
    return surface;
}

QString cglErrorToString(CGLError error)
{
    const char *description = CGLErrorString(error);
    if (description == nullptr) {
        return QStringLiteral("CGLError(%1)").arg(int(error));
    }
    return QString::fromLatin1(description);
}

#endif
}

class MacCocoaGlTextureWriter::Impl final
{
public:
    explicit Impl(const std::shared_ptr<MacIoSurfaceTextureSlots> &slotPool)
        : slotPool(slotPool)
    {
    }

    std::shared_ptr<MacIoSurfaceTextureSlots> slotPool;
    std::atomic<RuntimeHost *> host { nullptr };
    std::atomic<IRuntime *> attachedRuntime { nullptr };
    std::atomic<IWriterEvents *> eventSink { nullptr };
    std::atomic_bool drainScheduled { false };
    GLuint publishFramebufferId = 0U;
    GLuint program2D = 0U;
    GLint positionLocation2D = -1;
    GLint texCoordLocation2D = -1;
    GLint samplerLocation2D = -1;
    quint64 publicationCounter = 0;
    PendingLatestFrame pendingLatestFrame;
    bool initialized = false;

#if defined(Q_OS_MACOS)
    struct SlotResources final
    {
        IOSurfaceRef ioSurface = nullptr;
        QSize size;
        quint64 generation = 0;
        GLuint textureId = 0U;
        GLenum textureTarget = GL_TEXTURE_RECTANGLE_ARB;
    };

    QVector<SlotResources> slotResources;
#endif

    ~Impl()
    {
        releaseSurfaceRefs();
    }

    RuntimeHost *runtimeHost() const noexcept
    {
        return host.load(std::memory_order_acquire);
    }

    IRuntime *runtime() const noexcept
    {
        return attachedRuntime.load(std::memory_order_acquire);
    }

    IWriterEvents *events() const noexcept
    {
        return eventSink.load(std::memory_order_acquire);
    }

    bool hasPendingFrame() const noexcept
    {
        return pendingLatestFrame.valid;
    }

    bool hasGlResources() const noexcept
    {
        if (publishFramebufferId != 0U || program2D != 0U) {
            return true;
        }
#if defined(Q_OS_MACOS)
        for (const SlotResources &slot : slotResources) {
            if (slot.textureId != 0U || slot.ioSurface != nullptr) {
                return true;
            }
        }
#endif
        return pendingLatestFrame.textureId != 0U;
    }

    bool initialize(QString *error)
    {
        if (initialized) {
            return true;
        }
        if (slotPool == nullptr) {
            if (error) {
                *error = QStringLiteral("MacCocoaGlTextureWriter requires a valid IOSurface slot pool.");
            }
            return false;
        }

        QOpenGLContext *context = QOpenGLContext::currentContext();
        QOpenGLFunctions *gl = context ? context->functions() : nullptr;
        if (context == nullptr || gl == nullptr) {
            if (error) {
                *error = QStringLiteral("MacCocoaGlTextureWriter requires a current QOpenGLContext.");
            }
            return false;
        }

        gl->glGenFramebuffers(1, &publishFramebufferId);
        if (publishFramebufferId == 0U) {
            if (error) {
                *error = QStringLiteral("MacCocoaGlTextureWriter failed to create the publish framebuffer.");
            }
            return false;
        }

        if (!buildPrograms(gl, error)) {
            return false;
        }

#if defined(Q_OS_MACOS)
        slotResources.resize(slotPool->slotCount());
#endif
        initialized = true;
        return true;
    }

    SubmitTextureResult submitTexture(GLuint sourceTextureId,
                                      const QSize &sourceSize,
                                      quint64 outputRevision)
    {
        SubmitTextureResult result;
        QString error;
        if (!initialize(&error)) {
            result.error = std::move(error);
            return result;
        }
        if (sourceTextureId == 0U || !sourceSize.isValid()) {
            result.error = QStringLiteral("MacCocoaGlTextureWriter only supports valid GPU texture results.");
            return result;
        }

        const QSize safeSize = sanitizedSize(sourceSize);
        TextureTicket ticket;
        bool busy = false;
        if (tryPublishNow(sourceTextureId, safeSize, outputRevision, &ticket, &error, &busy)) {
            invalidatePendingLatestFrame();
            result.kind = SubmitTextureResultKind::Published;
            result.ticket = ticket;
            return result;
        }

        if (!busy) {
            result.error = std::move(error);
            return result;
        }

        pendingLatestFrame.textureId = sourceTextureId;
        pendingLatestFrame.size = safeSize;
        pendingLatestFrame.outputRevision = outputRevision;
        pendingLatestFrame.valid = true;
        result.kind = SubmitTextureResultKind::Queued;
        return result;
    }

    DrainPendingPublishesResult drainPendingPublishes()
    {
        DrainPendingPublishesResult result;
        if (!pendingLatestFrame.valid) {
            return result;
        }

        TextureTicket ticket;
        QString error;
        bool busy = false;
        if (tryPublishNow(pendingLatestFrame.textureId,
                          pendingLatestFrame.size,
                          pendingLatestFrame.outputRevision,
                          &ticket,
                          &error,
                          &busy)) {
            invalidatePendingLatestFrame();
            result.tickets.push_back(ticket);
            return result;
        }

        if (!busy) {
            result.error = std::move(error);
        }
        return result;
    }

    void shutdown()
    {
        QOpenGLContext *context = QOpenGLContext::currentContext();
        QOpenGLFunctions *gl = context ? context->functions() : nullptr;
        if (gl != nullptr) {
#if defined(Q_OS_MACOS)
            for (SlotResources &slot : slotResources) {
                if (slot.textureId != 0U) {
                    gl->glDeleteTextures(1, &slot.textureId);
                    slot.textureId = 0U;
                }
            }
#endif
            if (publishFramebufferId != 0U) {
                gl->glDeleteFramebuffers(1, &publishFramebufferId);
                publishFramebufferId = 0U;
            }
            if (program2D != 0U) {
                gl->glDeleteProgram(program2D);
                program2D = 0U;
            }
        }

        releaseSurfaceRefs();
        invalidatePendingLatestFrame();
        initialized = false;
    }

private:
    void invalidatePendingLatestFrame()
    {
        pendingLatestFrame.textureId = 0U;
        pendingLatestFrame.size = {};
        pendingLatestFrame.outputRevision = 0;
        pendingLatestFrame.valid = false;
    }

    void releaseSurfaceRefs()
    {
#if defined(Q_OS_MACOS)
        for (SlotResources &slot : slotResources) {
            if (slot.ioSurface != nullptr) {
                CFRelease(slot.ioSurface);
                slot.ioSurface = nullptr;
            }
            slot.size = QSize();
            slot.generation = 0;
            slot.textureTarget = GL_TEXTURE_RECTANGLE_ARB;
        }
#endif
    }

    bool tryPublishNow(GLuint sourceTextureId,
                       const QSize &sourceSize,
                       quint64 outputRevision,
                       TextureTicket *ticket,
                       QString *error,
                       bool *busy)
    {
        if (busy) {
            *busy = false;
        }
        if (ticket == nullptr || slotPool == nullptr) {
            if (error) {
                *error = QStringLiteral("MacCocoaGlTextureWriter publish prerequisites are incomplete.");
            }
            return false;
        }

        int renderSlot = -1;
        if (!slotPool->tryAcquireRenderSlot(&renderSlot)) {
            if (busy) {
                *busy = true;
            }
            return false;
        }

        if (!publishToSlot(sourceTextureId, sourceSize, renderSlot, error)) {
            slotPool->abandonRenderSlot(renderSlot);
            return false;
        }

        if (!slotPool->submitRenderedTexture(renderSlot, ++publicationCounter, outputRevision, ticket)) {
            slotPool->abandonRenderSlot(renderSlot);
            if (error) {
                *error = QStringLiteral("MacCocoaGlTextureWriter failed to publish slot %1 metadata.").arg(renderSlot);
            }
            return false;
        }

        return true;
    }

    bool publishToSlot(GLuint sourceTextureId,
                       const QSize &sourceSize,
                       int slotIndex,
                       QString *error)
    {
#if !defined(Q_OS_MACOS)
        Q_UNUSED(sourceTextureId);
        Q_UNUSED(sourceSize);
        Q_UNUSED(slotIndex);
        if (error) {
            *error = QStringLiteral("MacCocoaGlTextureWriter is only available on macOS.");
        }
        return false;
#else
        if (!ensureSlotResources(slotIndex, sourceSize, error)) {
            return false;
        }

        QOpenGLContext *context = QOpenGLContext::currentContext();
        QOpenGLFunctions *gl = context ? context->functions() : nullptr;
        if (context == nullptr || gl == nullptr) {
            if (error) {
                *error = QStringLiteral("MacCocoaGlTextureWriter requires a current QOpenGLContext.");
            }
            return false;
        }

        SlotResources &slot = slotResources[slotIndex];

        gl->glBindFramebuffer(GL_FRAMEBUFFER, publishFramebufferId);
        gl->glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, slot.textureTarget, slot.textureId, 0);
        const GLenum status = gl->glCheckFramebufferStatus(GL_FRAMEBUFFER);
        if (status != GL_FRAMEBUFFER_COMPLETE) {
            gl->glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, slot.textureTarget, 0, 0);
            gl->glBindFramebuffer(GL_FRAMEBUFFER, 0);
            if (error) {
                *error = QStringLiteral("The macOS IOSurface framebuffer is incomplete: 0x%1")
                             .arg(unsigned(status), 0, 16);
            }
            return false;
        }

        gl->glViewport(0, 0, sourceSize.width(), sourceSize.height());
        gl->glDisable(GL_DEPTH_TEST);
        gl->glUseProgram(program2D);
        gl->glActiveTexture(GL_TEXTURE0);
        gl->glBindTexture(GL_TEXTURE_2D, sourceTextureId);
        gl->glUniform1i(samplerLocation2D, 0);
        gl->glVertexAttribPointer(GLuint(positionLocation2D), 2, GL_FLOAT, GL_FALSE, 0, kVertices);
        gl->glEnableVertexAttribArray(GLuint(positionLocation2D));
        gl->glVertexAttribPointer(GLuint(texCoordLocation2D), 2, GL_FLOAT, GL_FALSE, 0, kTexCoords2D);
        gl->glEnableVertexAttribArray(GLuint(texCoordLocation2D));
        gl->glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
        gl->glDisableVertexAttribArray(GLuint(positionLocation2D));
        gl->glDisableVertexAttribArray(GLuint(texCoordLocation2D));
        gl->glBindTexture(GL_TEXTURE_2D, 0);
        gl->glUseProgram(0);
        gl->glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, slot.textureTarget, 0, 0);
        gl->glBindFramebuffer(GL_FRAMEBUFFER, 0);
        gl->glFlush();

        const GLenum glError = gl->glGetError();
        if (glError != GL_NO_ERROR) {
            if (error) {
                *error = QStringLiteral("MacCocoaGlTextureWriter IOSurface draw failed with GL error 0x%1")
                             .arg(unsigned(glError), 0, 16);
            }
            return false;
        }
        return true;
#endif
    }

 #if defined(Q_OS_MACOS)
    bool ensureSlotResources(int slotIndex, const QSize &sourceSize, QString *error)
    {
        if (slotIndex < 0 || slotIndex >= slotResources.size()) {
            if (error) {
                *error = QStringLiteral("MacCocoaGlTextureWriter slot index %1 is invalid.").arg(slotIndex);
            }
            return false;
        }

        QOpenGLContext *context = QOpenGLContext::currentContext();
        QOpenGLFunctions *gl = context ? context->functions() : nullptr;
        if (context == nullptr || gl == nullptr) {
            if (error) {
                *error = QStringLiteral("MacCocoaGlTextureWriter requires a current QOpenGLContext.");
            }
            return false;
        }

        SlotResources &slot = slotResources[slotIndex];
        if (slot.textureId != 0U && slot.size == sourceSize && slot.ioSurface != nullptr) {
            return true;
        }

        if (slot.textureId != 0U) {
            gl->glDeleteTextures(1, &slot.textureId);
            slot.textureId = 0U;
        }
        if (slot.ioSurface != nullptr) {
            CFRelease(slot.ioSurface);
            slot.ioSurface = nullptr;
        }

        IOSurfaceRef ioSurface = createSurface(sourceSize);
        if (ioSurface == nullptr) {
            if (error) {
                *error = QStringLiteral("MacCocoaGlTextureWriter failed to create an IOSurface.");
            }
            return false;
        }

        gl->glGenTextures(1, &slot.textureId);
        if (slot.textureId == 0U) {
            CFRelease(ioSurface);
            if (error) {
                *error = QStringLiteral("MacCocoaGlTextureWriter failed to allocate the IOSurface texture object.");
            }
            return false;
        }

        gl->glBindTexture(GL_TEXTURE_RECTANGLE_ARB, slot.textureId);
        gl->glTexParameteri(GL_TEXTURE_RECTANGLE_ARB, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        gl->glTexParameteri(GL_TEXTURE_RECTANGLE_ARB, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        gl->glTexParameteri(GL_TEXTURE_RECTANGLE_ARB, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        gl->glTexParameteri(GL_TEXTURE_RECTANGLE_ARB, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

        CGLContextObj cglContext = CGLGetCurrentContext();
        const CGLError bindError = CGLTexImageIOSurface2D(cglContext,
                                                          GL_TEXTURE_RECTANGLE_ARB,
                                                          GL_RGBA,
                                                          sourceSize.width(),
                                                          sourceSize.height(),
                                                          GL_BGRA,
                                                          GL_UNSIGNED_INT_8_8_8_8_REV,
                                                          ioSurface,
                                                          0);
        gl->glBindTexture(GL_TEXTURE_RECTANGLE_ARB, 0);
        if (bindError != kCGLNoError) {
            gl->glDeleteTextures(1, &slot.textureId);
            slot.textureId = 0U;
            CFRelease(ioSurface);
            if (error) {
                *error = QStringLiteral("CGLTexImageIOSurface2D failed for slot %1: %2")
                             .arg(slotIndex)
                             .arg(cglErrorToString(bindError));
            }
            return false;
        }

        slot.ioSurface = ioSurface;
        slot.size = sourceSize;
        slot.generation += 1;
        slot.textureTarget = GL_TEXTURE_RECTANGLE_ARB;
        slotPool->updateSlot(slotIndex, slot.ioSurface, slot.size, slot.generation, slot.textureTarget);
        return true;
    }
#endif

    bool buildPrograms(QOpenGLFunctions *gl, QString *error)
    {
        static const char *vertexShader = R"(
attribute highp vec2 aPosition;
attribute mediump vec2 aTexCoord;
varying mediump vec2 vTexCoord;

void main()
{
    vTexCoord = aTexCoord;
    gl_Position = vec4(aPosition, 0.0, 1.0);
}
)";

        static const char *fragmentShader2D = R"(
uniform sampler2D uTexture;
varying mediump vec2 vTexCoord;

void main()
{
    gl_FragColor = texture2D(uTexture, vTexCoord);
}
)";

        if (!buildProgram(gl,
                          vertexShader,
                          fragmentShader2D,
                          &program2D,
                          &positionLocation2D,
                          &texCoordLocation2D,
                          &samplerLocation2D,
                          error)) {
            return false;
        }
        return true;
    }

    bool buildProgram(QOpenGLFunctions *gl,
                      const char *vertexSource,
                      const char *fragmentSource,
                      GLuint *program,
                      GLint *positionLocation,
                      GLint *texCoordLocation,
                      GLint *samplerLocation,
                      QString *error)
    {
        const GLuint vertexShader = gl->glCreateShader(GL_VERTEX_SHADER);
        const GLuint fragmentShader = gl->glCreateShader(GL_FRAGMENT_SHADER);
        if (!compileShader(gl, vertexShader, vertexSource, error)
            || !compileShader(gl, fragmentShader, fragmentSource, error)) {
            if (vertexShader != 0U) {
                gl->glDeleteShader(vertexShader);
            }
            if (fragmentShader != 0U) {
                gl->glDeleteShader(fragmentShader);
            }
            return false;
        }

        *program = gl->glCreateProgram();
        gl->glAttachShader(*program, vertexShader);
        gl->glAttachShader(*program, fragmentShader);
        gl->glLinkProgram(*program);
        gl->glDeleteShader(vertexShader);
        gl->glDeleteShader(fragmentShader);

        GLint linked = 0;
        gl->glGetProgramiv(*program, GL_LINK_STATUS, &linked);
        if (linked == 0) {
            if (error) {
                *error = programInfoLog(gl, *program);
            }
            gl->glDeleteProgram(*program);
            *program = 0U;
            return false;
        }

        *positionLocation = gl->glGetAttribLocation(*program, "aPosition");
        *texCoordLocation = gl->glGetAttribLocation(*program, "aTexCoord");
        *samplerLocation = gl->glGetUniformLocation(*program, "uTexture");
        if (*positionLocation < 0 || *texCoordLocation < 0 || *samplerLocation < 0) {
            if (error) {
                *error = QStringLiteral("MacCocoaGlTextureWriter shader program is missing required attributes or uniforms.");
            }
            gl->glDeleteProgram(*program);
            *program = 0U;
            return false;
        }

        return true;
    }

    bool compileShader(QOpenGLFunctions *gl, GLuint shader, const char *source, QString *error)
    {
        gl->glShaderSource(shader, 1, &source, nullptr);
        gl->glCompileShader(shader);
        GLint compiled = 0;
        gl->glGetShaderiv(shader, GL_COMPILE_STATUS, &compiled);
        if (compiled != 0) {
            return true;
        }

        if (error) {
            *error = shaderInfoLog(gl, shader);
        }
        return false;
    }

    QString shaderInfoLog(QOpenGLFunctions *gl, GLuint shader) const
    {
        GLint length = 0;
        gl->glGetShaderiv(shader, GL_INFO_LOG_LENGTH, &length);
        if (length <= 0) {
            return QStringLiteral("OpenGL shader compilation failed.");
        }

        QByteArray log(length, Qt::Uninitialized);
        GLsizei written = 0;
        gl->glGetShaderInfoLog(shader, length, &written, log.data());
        return QString::fromLocal8Bit(log.constData(), written);
    }

    QString programInfoLog(QOpenGLFunctions *gl, GLuint program) const
    {
        GLint length = 0;
        gl->glGetProgramiv(program, GL_INFO_LOG_LENGTH, &length);
        if (length <= 0) {
            return QStringLiteral("OpenGL program link failed.");
        }

        QByteArray log(length, Qt::Uninitialized);
        GLsizei written = 0;
        gl->glGetProgramInfoLog(program, length, &written, log.data());
        return QString::fromLocal8Bit(log.constData(), written);
    }
};

MacCocoaGlTextureWriter::MacCocoaGlTextureWriter(const std::shared_ptr<MacIoSurfaceTextureSlots> &slotPool)
    : m_impl(std::make_unique<Impl>(slotPool))
{
}

MacCocoaGlTextureWriter::~MacCocoaGlTextureWriter()
{
    reset();
}

void MacCocoaGlTextureWriter::attach(RuntimeHost *host, IRuntime *runtime, IWriterEvents *events)
{
    if (!m_impl) {
        return;
    }

    m_impl->host.store(host, std::memory_order_release);
    m_impl->attachedRuntime.store(runtime, std::memory_order_release);
    m_impl->eventSink.store(events, std::memory_order_release);
    m_impl->drainScheduled.store(false, std::memory_order_release);
}

bool MacCocoaGlTextureWriter::submitTexture(GLuint sourceTextureId,
                                            const QSize &size,
                                            quint64 outputRevision,
                                            QString *error)
{
    RuntimeHost *host = m_impl ? m_impl->runtimeHost() : nullptr;
    IRuntime *runtime = m_impl ? m_impl->runtime() : nullptr;
    IWriterEvents *events = m_impl ? m_impl->events() : nullptr;
    if (!m_impl || !host || !runtime || !events || !m_impl->slotPool) {
        if (error) {
            *error = QStringLiteral("MacCocoaGlTextureWriter requires a valid runtime host, runtime, event sink, and slot pool.");
        }
        return false;
    }

    SubmitTextureResult result;
    QString dispatchError;
    const bool dispatched = host->dispatchSync([this, sourceTextureId, size, outputRevision, &result](IRuntime *invokedRuntime) {
        if (!m_impl || invokedRuntime == nullptr || invokedRuntime != m_impl->runtime()) {
            result.kind = SubmitTextureResultKind::Failed;
            result.error = QStringLiteral("MacCocoaGlTextureWriter runtime dispatch reached an unexpected runtime.");
            return;
        }

        QString enterError;
        if (!invokedRuntime->enter(&enterError)) {
            result.kind = SubmitTextureResultKind::Failed;
            result.error = std::move(enterError);
            return;
        }

        result = m_impl->submitTexture(sourceTextureId, size, outputRevision);
        invokedRuntime->leave();
    }, &dispatchError);
    if (!dispatched) {
        if (error) {
            *error = dispatchError.isEmpty()
                ? QStringLiteral("MacCocoaGlTextureWriter failed to dispatch to the runtime host.")
                : dispatchError;
        }
        return false;
    }

    switch (result.kind) {
    case SubmitTextureResultKind::Published:
        events->onTextureReady(result.ticket);
        return true;
    case SubmitTextureResultKind::Queued:
        return true;
    case SubmitTextureResultKind::Failed:
    default:
        if (error) {
            *error = result.error;
        }
        return false;
    }
}

void MacCocoaGlTextureWriter::notifyPresentationCapacityAvailable()
{
    if (!m_impl) {
        return;
    }
    if (m_impl->drainScheduled.exchange(true, std::memory_order_acq_rel)) {
        return;
    }

    RuntimeHost *host = m_impl->runtimeHost();
    if (host == nullptr) {
        m_impl->drainScheduled.store(false, std::memory_order_release);
        return;
    }

    QString dispatchError;
    const bool dispatched = host->dispatchAsync([this](IRuntime *runtime) {
        if (!m_impl) {
            return;
        }

        m_impl->drainScheduled.store(false, std::memory_order_release);
        if (!m_impl->hasPendingFrame() || runtime == nullptr || runtime != m_impl->runtime()) {
            return;
        }

        IWriterEvents *events = m_impl->events();
        QString enterError;
        if (!runtime->enter(&enterError)) {
            if (events != nullptr && !enterError.isEmpty()) {
                events->onWarning(enterError);
            }
            return;
        }

        const DrainPendingPublishesResult result = m_impl->drainPendingPublishes();
        runtime->leave();

        if (events == nullptr) {
            return;
        }
        if (!result.error.isEmpty()) {
            events->onWarning(result.error);
            return;
        }
        for (const TextureTicket &ticket : result.tickets) {
            events->onTextureReady(ticket);
        }
    }, &dispatchError);

    if (!dispatched) {
        m_impl->drainScheduled.store(false, std::memory_order_release);
        IWriterEvents *events = m_impl->events();
        if (events != nullptr && !dispatchError.isEmpty()) {
            events->onWarning(dispatchError);
        }
    }
}

void MacCocoaGlTextureWriter::reset()
{
    if (!m_impl) {
        return;
    }

    RuntimeHost *host = m_impl->runtimeHost();
    IRuntime *runtime = m_impl->runtime();
    if (m_impl->hasGlResources()) {
        QString dispatchError;
        if (host != nullptr && runtime != nullptr) {
            const bool dispatched = host->dispatchSync([this](IRuntime *invokedRuntime) {
                if (!m_impl || invokedRuntime == nullptr || invokedRuntime != m_impl->runtime()) {
                    return;
                }

                QString enterError;
                if (!invokedRuntime->enter(&enterError)) {
                    return;
                }
                m_impl->shutdown();
                invokedRuntime->leave();
            }, &dispatchError);
            if (!dispatched) {
                m_impl->shutdown();
            }
        } else {
            m_impl->shutdown();
        }
    } else {
        m_impl->shutdown();
    }

    m_impl->host.store(nullptr, std::memory_order_release);
    m_impl->eventSink.store(nullptr, std::memory_order_release);
    m_impl->drainScheduled.store(false, std::memory_order_release);
    m_impl->attachedRuntime.store(nullptr, std::memory_order_release);
    if (m_impl->slotPool) {
        m_impl->slotPool->reset();
    }
}
