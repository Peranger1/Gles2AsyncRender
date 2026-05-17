#pragma once

#include "framework/execution/execution_common.h"
#include "framework/execution/execution_types.h"
#include "photo_editor_render_args.h"

class PhotoEditorCpuRenderer final
{
public:
    static ExecutionOutcome<CpuImageResult> renderPreview(const PhotoEditorCpuPreviewArgs &args);
};
