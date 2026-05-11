#include "gles2_shader_utils.h"

#include "gles2_proc_table.h"

#include <QByteArray>

namespace
{
QString shaderLog(const Gles2ProcTable &gl, GLuint shader)
{
    GLint logLength = 0;
    gl.glGetShaderiv(shader, GL_INFO_LOG_LENGTH, &logLength);
    if (logLength <= 1) {
        return {};
    }

    QByteArray buffer(logLength, Qt::Uninitialized);
    GLsizei written = 0;
    gl.glGetShaderInfoLog(shader, logLength, &written, buffer.data());
    return QString::fromLatin1(buffer.constData(), written);
}

QString programLog(const Gles2ProcTable &gl, GLuint program)
{
    GLint logLength = 0;
    gl.glGetProgramiv(program, GL_INFO_LOG_LENGTH, &logLength);
    if (logLength <= 1) {
        return {};
    }

    QByteArray buffer(logLength, Qt::Uninitialized);
    GLsizei written = 0;
    gl.glGetProgramInfoLog(program, logLength, &written, buffer.data());
    return QString::fromLatin1(buffer.constData(), written);
}

bool compileShader(const Gles2ProcTable &gl,
                   GLenum shaderType,
                   const char *source,
                   GLuint *shader,
                   QString *error)
{
    if (shader == nullptr || source == nullptr) {
        if (error) {
            *error = QStringLiteral("Shader compilation prerequisites are incomplete.");
        }
        return false;
    }

    const GLuint createdShader = gl.glCreateShader(shaderType);
    if (createdShader == 0U) {
        if (error) {
            *error = QStringLiteral("glCreateShader failed for shader type 0x%1.")
                         .arg(static_cast<unsigned int>(shaderType), 0, 16);
        }
        return false;
    }

    gl.glShaderSource(createdShader, 1, &source, nullptr);
    gl.glCompileShader(createdShader);

    GLint compileStatus = GL_FALSE;
    gl.glGetShaderiv(createdShader, GL_COMPILE_STATUS, &compileStatus);
    if (compileStatus != GL_TRUE) {
        if (error) {
            const QString stage = shaderType == GL_VERTEX_SHADER
                ? QStringLiteral("vertex")
                : QStringLiteral("fragment");
            const QString log = shaderLog(gl, createdShader);
            *error = log.isEmpty()
                ? QStringLiteral("Failed to compile the %1 shader.").arg(stage)
                : QStringLiteral("Failed to compile the %1 shader: %2").arg(stage, log);
        }
        gl.glDeleteShader(createdShader);
        return false;
    }

    *shader = createdShader;
    return true;
}
}

namespace Gles2ShaderUtils
{
bool buildProgram(const Gles2ProcTable &gl,
                  const char *vertexSource,
                  const char *fragmentSource,
                  GLuint *program,
                  QString *error)
{
    if (program == nullptr) {
        if (error) {
            *error = QStringLiteral("A destination program handle is required.");
        }
        return false;
    }

    GLuint vertexShader = 0;
    GLuint fragmentShader = 0;
    if (!compileShader(gl, GL_VERTEX_SHADER, vertexSource, &vertexShader, error)
        || !compileShader(gl, GL_FRAGMENT_SHADER, fragmentSource, &fragmentShader, error)) {
        if (vertexShader != 0U) {
            gl.glDeleteShader(vertexShader);
        }
        if (fragmentShader != 0U) {
            gl.glDeleteShader(fragmentShader);
        }
        return false;
    }

    const GLuint createdProgram = gl.glCreateProgram();
    if (createdProgram == 0U) {
        if (error) {
            *error = QStringLiteral("glCreateProgram failed.");
        }
        gl.glDeleteShader(vertexShader);
        gl.glDeleteShader(fragmentShader);
        return false;
    }

    gl.glAttachShader(createdProgram, vertexShader);
    gl.glAttachShader(createdProgram, fragmentShader);
    gl.glLinkProgram(createdProgram);

    GLint linkStatus = GL_FALSE;
    gl.glGetProgramiv(createdProgram, GL_LINK_STATUS, &linkStatus);
    gl.glDeleteShader(vertexShader);
    gl.glDeleteShader(fragmentShader);

    if (linkStatus != GL_TRUE) {
        if (error) {
            const QString log = programLog(gl, createdProgram);
            *error = log.isEmpty()
                ? QStringLiteral("Failed to link the GLES2 shader program.")
                : QStringLiteral("Failed to link the GLES2 shader program: %1").arg(log);
        }
        gl.glDeleteProgram(createdProgram);
        return false;
    }

    *program = createdProgram;
    return true;
}

void deleteProgram(const Gles2ProcTable &gl, GLuint *program)
{
    if (program == nullptr || *program == 0U) {
        return;
    }

    gl.glDeleteProgram(*program);
    *program = 0U;
}
}
