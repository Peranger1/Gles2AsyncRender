#include "runtime_scope.h"

RuntimeScope::RuntimeScope(IRuntime *runtime, QString *error)
    : m_runtime(runtime)
{
    if (m_runtime) {
        m_entered = m_runtime->enter(error);
    } else if (error) {
        *error = QStringLiteral("RuntimeScope requires a valid runtime.");
    }
}

RuntimeScope::~RuntimeScope()
{
    if (m_runtime && m_entered) {
        m_runtime->leave();
    }
}

bool RuntimeScope::ok() const
{
    return m_entered;
}
