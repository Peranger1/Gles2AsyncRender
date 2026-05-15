#pragma once

#include "gl_display_target.h"
#include "shared_frame_slot_pool.h"
#include "work_types.h"

#include <QString>

struct FramePresentationFeedback final
{
    bool releasedPublicationCapacity = false;
};

class IFramePresenter
{
public:
    virtual ~IFramePresenter() = default;

    virtual bool initialize(IDisplayTarget &target,
                            IFrameReader &frameReader,
                            QString *error) = 0;
    virtual bool enqueue(const FrameTicket &ticket, QString *error) = 0;
    virtual bool present(FramePresentationFeedback *feedback, QString *error) = 0;
    virtual QString diagnosticText() const = 0;
    virtual void shutdown() = 0;
};
