#pragma once

#include <QString>

#include <QtANGLE/GLES2/gl2.h>

class Gles2ProcTable;

namespace Gles2ShaderUtils
{
bool buildProgram(const Gles2ProcTable &gl,
                  const char *vertexSource,
                  const char *fragmentSource,
                  GLuint *program,
                  QString *error);

void deleteProgram(const Gles2ProcTable &gl, GLuint *program);
}

