/*
 * tinygl_stubs.c — GL functions missing from TinyGL that raylib requires.
 *
 * TinyGL (C-Chads fork) provides a fast OpenGL 1.1 subset but omits
 * several entry points raylib references. This file supplies them:
 *   - Real implementations where correctness matters (glOrtho, glColor4ub, glVertex2i)
 *   - No-op stubs where the feature can be safely ignored on this platform
 *
 * Compiled into the final binary alongside libTinyGL.a and libraylib.a.
 */

#include <GL/gl.h>
#include <string.h>

/* ---- glOrtho: orthographic projection matrix (used by raylib 2D mode) ---- */
void glOrtho(GLdouble left, GLdouble right, GLdouble bottom, GLdouble top,
             GLdouble nearVal, GLdouble farVal)
{
    GLfloat m[16];
    memset(m, 0, sizeof(m));

    GLfloat rl = (GLfloat)(right - left);
    GLfloat tb = (GLfloat)(top - bottom);
    GLfloat fn = (GLfloat)(farVal - nearVal);

    m[0]  =  2.0f / rl;
    m[5]  =  2.0f / tb;
    m[10] = -2.0f / fn;
    m[12] = -(GLfloat)(right + left) / rl;
    m[13] = -(GLfloat)(top + bottom) / tb;
    m[14] = -(GLfloat)(farVal + nearVal) / fn;
    m[15] = 1.0f;

    glMultMatrixf(m);
}

/* ---- Vertex/color wrappers for types TinyGL doesn't provide ---- */
void glColor4ub(GLubyte r, GLubyte g, GLubyte b, GLubyte a)
{
    glColor4f(r / 255.0f, g / 255.0f, b / 255.0f, a / 255.0f);
}

void glVertex2i(GLint x, GLint y)
{
    glVertex3f((GLfloat)x, (GLfloat)y, 0.0f);
}

/* ---- Depth ---- */
void glDepthFunc(GLint func)   { (void)func; }

/* ---- Line width ---- */
void glLineWidth(GLfloat width) { (void)width; }

/* ---- Stubs for features TinyGL does not support ---- */
void glColorMask(GLboolean r, GLboolean g, GLboolean b, GLboolean a)
{
    (void)r; (void)g; (void)b; (void)a;
}

void glScissor(GLint x, GLint y, GLsizei w, GLsizei h)
{
    (void)x; (void)y; (void)w; (void)h;
}

void glPixelStorei(GLenum pname, GLint param)
{
    (void)pname; (void)param;
}

void glTexSubImage2D(GLenum target, GLint level,
                     GLint xoffset, GLint yoffset,
                     GLsizei width, GLsizei height,
                     GLenum format, GLenum type, const void *data)
{
    (void)target; (void)level;
    (void)xoffset; (void)yoffset;
    (void)width; (void)height;
    (void)format; (void)type; (void)data;
}

void glGetTexImage(GLenum target, GLint level,
                   GLenum format, GLenum type, void *pixels)
{
    (void)target; (void)level;
    (void)format; (void)type; (void)pixels;
}

void glDrawElements(GLenum mode, GLsizei count,
                    GLenum type, const void *indices)
{
    (void)mode; (void)count; (void)type; (void)indices;
}
