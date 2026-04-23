#pragma once

#include <QString>

struct AngleThreadingInfo
{
    bool isAngleBackend = false;
    bool multithreadProtected = false;
    QString message;
};

AngleThreadingInfo ensureAngleD3D11MultithreadProtection();
