#pragma once

#include "presentation_target.h"
#include "work_types.h"

#include <QString>

struct PresentationFeedback final
{
    bool releasedPublicationCapacity = false;
};

class IArtifactPresenter
{
public:
    virtual ~IArtifactPresenter() = default;

    virtual bool initialize(IPresentationTarget &target, QString *error) = 0;
    virtual bool enqueue(const PublicationTicket &ticket, QString *error) = 0;
    virtual bool present(PresentationFeedback *feedback, QString *error) = 0;
    virtual void shutdown() = 0;
};
