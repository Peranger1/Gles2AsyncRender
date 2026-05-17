#pragma once

#include "framework/platform/runtime.h"

#include <memory>

class QOpenGLContext;
struct MacCocoaGlSharedState;

class MacCocoaGlRuntime final : public IRuntime
{
public:
    explicit MacCocoaGlRuntime(const std::shared_ptr<MacCocoaGlSharedState> &sharedState);
    ~MacCocoaGlRuntime() override;

    bool initialize(QString *error) override;
    bool enter(QString *error) override;
    void leave() override;
    void shutdown() override;
    void *resolveProc(const char *name) const override;

private:
    std::shared_ptr<MacCocoaGlSharedState> m_sharedState;
    std::unique_ptr<QOpenGLContext> m_context;
    bool m_initialized = false;
};
