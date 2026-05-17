#pragma once

#include "framework/execution/runtime_host.h"

#include <QObject>

#include <memory>

class IRuntime;

class PhotoEditorRuntimeHost final : public QObject, public RuntimeHost
{
    Q_OBJECT

public:
    explicit PhotoEditorRuntimeHost(std::unique_ptr<IRuntime> runtime,
                                    QObject *parent = nullptr);
    ~PhotoEditorRuntimeHost() override;

    bool start(QString *error) override;
    void shutdown() override;

    IRuntime *runtime() const override;
    bool isOnRuntimeThread() const override;

    bool dispatchAsync(RuntimeClosure closure, QString *error) override;
    bool dispatchSync(RuntimeClosure closure, QString *error) override;

private:
    std::unique_ptr<IRuntime> m_runtime;
    bool m_started = false;
    bool m_shuttingDown = false;
};
