#pragma once

#include "framework/execution/execution_common.h"
#include "photo_editor_render_args.h"
#include "photo_editor_result_types.h"

class PhotoEditorCpuRenderer final
{
public:
    static ExecutionOutcome<CpuImageResult> renderPreview(const PhotoEditorCpuPreviewArgs &args);
};
