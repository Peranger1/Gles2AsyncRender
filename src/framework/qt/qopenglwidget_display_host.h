#pragma once

#include "src/framework/core/display_presenter.h"

#include <QOpenGLContext>
#include <QOpenGLFunctions>
#include <QOpenGLWidget>
#include <QPointer>

#include <QtMath>

class QOpenGLWidgetDisplayHost final : public IDisplayHost
{
public:
    explicit QOpenGLWidgetDisplayHost(QOpenGLWidget *widget = nullptr)
        : m_widget(widget)
    {
    }

    void setWidget(QOpenGLWidget *widget)
    {
        m_widget = widget;
    }

    QOpenGLContext *glContext() const override
    {
        return m_widget ? m_widget->context() : nullptr;
    }

    QOpenGLFunctions *glFunctions() const override
    {
        QOpenGLContext *context = glContext();
        return context ? context->functions() : nullptr;
    }

    QSize outputPixelSize() const override
    {
        if (!m_widget) {
            return {};
        }

        const qreal dpr = m_widget->devicePixelRatioF();
        return QSize(qMax(1, qRound(m_widget->width() * dpr)),
                     qMax(1, qRound(m_widget->height() * dpr)));
    }

    void requestUpdate() override
    {
        if (m_widget) {
            m_widget->update();
        }
    }

private:
    QPointer<QOpenGLWidget> m_widget;
};
