#include "win_angle_platform_backend.h"

#include "d3d11_shared_texture_slots.h"
#include "win_angle_runtime.h"
#include "win_angle_texture_reader.h"
#include "win_angle_texture_writer.h"

class BackendBoundRuntime final : public IRuntime
{
public:
    explicit BackendBoundRuntime(const std::shared_ptr<WinAngleTextureWriter::RuntimeBindingState> &runtimeBinding)
        : m_runtimeBinding(runtimeBinding)
        , m_runtime(std::make_unique<WinAngleRuntime>())
    {
        if (m_runtimeBinding && m_runtime) {
            m_runtimeBinding->runtime = m_runtime.get();
        }
    }

    ~BackendBoundRuntime() override
    {
        if (m_runtimeBinding) {
            m_runtimeBinding->runtime = nullptr;
        }
    }

    bool initialize(QString *error) override
    {
        return m_runtime && m_runtime->initialize(error);
    }

    bool enter(QString *error) override
    {
        return m_runtime && m_runtime->enter(error);
    }

    void leave() override
    {
        if (m_runtime) {
            m_runtime->leave();
        }
    }

    void shutdown() override
    {
        if (m_runtime) {
            m_runtime->shutdown();
        }
    }

    void *resolveProc(const char *name) const override
    {
        return m_runtime ? m_runtime->resolveProc(name) : nullptr;
    }

private:
    std::shared_ptr<WinAngleTextureWriter::RuntimeBindingState> m_runtimeBinding;
    std::unique_ptr<WinAngleRuntime> m_runtime;
};

WinAnglePlatformBackend::WinAnglePlatformBackend(int slotCount)
    : m_runtimeBinding(std::make_shared<WinAngleTextureWriter::RuntimeBindingState>())
    , m_slotPool(std::make_shared<D3D11SharedTextureSlots>(slotCount))
    , m_reader(std::make_unique<WinAngleTextureReader>(m_slotPool))
    , m_writer(std::make_unique<WinAngleTextureWriter>(m_slotPool, m_runtimeBinding))
{
}

WinAnglePlatformBackend::~WinAnglePlatformBackend() = default;

std::unique_ptr<IRuntime> WinAnglePlatformBackend::createRuntime() const
{
    return std::make_unique<BackendBoundRuntime>(m_runtimeBinding);
}

IReader *WinAnglePlatformBackend::reader() const
{
    return m_reader.get();
}

IWriter *WinAnglePlatformBackend::writer() const
{
    return m_writer.get();
}
