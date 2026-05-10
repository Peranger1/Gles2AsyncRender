#pragma once

#include "image_effect_types.h"

#include <QImage>
#include <QSize>
#include <QString>

#include <QtGui/qopengl.h>

class QObject;

using PhotoEditorResolveGlProc = void *(*)(const char *name);
using PhotoEditorProgressCallback = void (*)(int progress, bool isEnd, void *userData);

bool photo_editor_init(PhotoEditorResolveGlProc resolver, QString *error);
void *photo_editor_create(const QImage &sourceImage, QString *error);
void photo_editor_destroy(void *handle);

bool photo_editor_set_output_size(void *handle, const QSize &outputSize, QString *error);
bool photo_editor_set_opcode(void *handle, const ImageEffectParameters &parameters, QString *error);
bool photo_editor_process(void *handle,
                          QObject *callbackContext,
                          PhotoEditorProgressCallback callback,
                          void *userData,
                          QString *error);
bool photo_editor_render(void *handle, GLuint *textureId, QSize *size, QString *error);
