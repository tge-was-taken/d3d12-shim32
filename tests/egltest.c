/* Draws a textured quad offscreen with GLES 3 and checks the pixels read back. */
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl3.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define W 64
#define H 64

static GLuint shader(GLenum type, const char *src)
{
   GLuint s = glCreateShader(type);
   glShaderSource(s, 1, &src, NULL);
   glCompileShader(s);
   GLint ok;
   glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
   if (!ok) {
      char log[1024];
      glGetShaderInfoLog(s, sizeof(log), NULL, log);
      fprintf(stderr, "shader: %s\n", log);
      exit(1);
   }
   return s;
}

static int check(const char *what, const uint8_t *got, const uint8_t *want)
{
   for (int i = 0; i < W * H; i++) {
      if (memcmp(got + i * 4, want + i * 4, 4)) {
         printf("%s: FAIL at pixel %d: got %02x%02x%02x%02x want %02x%02x%02x%02x\n", what, i, got[i * 4], got[i * 4 + 1],
                got[i * 4 + 2], got[i * 4 + 3], want[i * 4], want[i * 4 + 1], want[i * 4 + 2], want[i * 4 + 3]);
         return 1;
      }
   }
   printf("%s: ok\n", what);
   return 0;
}

int main(void)
{
   PFNEGLGETPLATFORMDISPLAYEXTPROC get_display = (void *)eglGetProcAddress("eglGetPlatformDisplayEXT");
   EGLDisplay dpy = get_display(EGL_PLATFORM_SURFACELESS_MESA, EGL_DEFAULT_DISPLAY, NULL);
   if (!eglInitialize(dpy, NULL, NULL)) {
      fprintf(stderr, "eglInitialize failed\n");
      return 1;
   }
   eglBindAPI(EGL_OPENGL_ES_API);
   static const EGLint cfg_attrs[] = {EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT, EGL_SURFACE_TYPE, 0, EGL_NONE};
   EGLConfig cfg;
   EGLint n;
   eglChooseConfig(dpy, cfg_attrs, &cfg, 1, &n);
   static const EGLint ctx_attrs[] = {EGL_CONTEXT_MAJOR_VERSION, 3, EGL_NONE};
   EGLContext ctx = eglCreateContext(dpy, n ? cfg : EGL_NO_CONFIG_KHR, EGL_NO_CONTEXT, ctx_attrs);
   if (!ctx || !eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, ctx)) {
      fprintf(stderr, "cannot create context: %#x\n", eglGetError());
      return 1;
   }
   printf("GL_RENDERER: %s\nGL_VERSION: %s\n", glGetString(GL_RENDERER), glGetString(GL_VERSION));

   GLuint fb, rb;
   glGenFramebuffers(1, &fb);
   glBindFramebuffer(GL_FRAMEBUFFER, fb);
   glGenRenderbuffers(1, &rb);
   glBindRenderbuffer(GL_RENDERBUFFER, rb);
   glRenderbufferStorage(GL_RENDERBUFFER, GL_RGBA8, W, H);
   glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, rb);
   glViewport(0, 0, W, H);

   static uint8_t got[W * H * 4], want[W * H * 4];
   int fails = 0;

   glClearColor(1, 0, 0, 1);
   glClear(GL_COLOR_BUFFER_BIT);
   glReadPixels(0, 0, W, H, GL_RGBA, GL_UNSIGNED_BYTE, got);
   for (int i = 0; i < W * H; i++)
      memcpy(want + i * 4, (uint8_t[]){255, 0, 0, 255}, 4);
   fails += check("clear", got, want);

   for (int y = 0; y < H; y++)
      for (int x = 0; x < W; x++) {
         uint8_t *p = want + (y * W + x) * 4;
         p[0] = x * 4;
         p[1] = y * 4;
         p[2] = (x ^ y) * 4;
         p[3] = 255;
      }
   GLuint tex;
   glGenTextures(1, &tex);
   glBindTexture(GL_TEXTURE_2D, tex);
   glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, W, H, 0, GL_RGBA, GL_UNSIGNED_BYTE, want);
   glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
   glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);

   GLuint prog = glCreateProgram();
   glAttachShader(prog, shader(GL_VERTEX_SHADER,
                               "#version 300 es\n"
                               "in vec2 pos;\n"
                               "out vec2 uv;\n"
                               "void main() { uv = pos * 0.5 + 0.5; gl_Position = vec4(pos, 0.0, 1.0); }\n"));
   glAttachShader(prog, shader(GL_FRAGMENT_SHADER,
                               "#version 300 es\n"
                               "precision mediump float;\n"
                               "uniform sampler2D tex;\n"
                               "in vec2 uv;\n"
                               "out vec4 color;\n"
                               "void main() { color = texture(tex, uv); }\n"));
   glBindAttribLocation(prog, 0, "pos");
   glLinkProgram(prog);
   glUseProgram(prog);

   static const float quad[] = {-1, -1, 1, -1, -1, 1, 1, 1};
   GLuint vbo;
   glGenBuffers(1, &vbo);
   glBindBuffer(GL_ARRAY_BUFFER, vbo);
   glBufferData(GL_ARRAY_BUFFER, sizeof(quad), quad, GL_STATIC_DRAW);
   glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, 0);
   glEnableVertexAttribArray(0);

   for (int frame = 0; frame < 3; frame++) {
      glClear(GL_COLOR_BUFFER_BIT);
      glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
      memset(got, 0, sizeof(got));
      glReadPixels(0, 0, W, H, GL_RGBA, GL_UNSIGNED_BYTE, got);
      char name[32];
      snprintf(name, sizeof(name), "textured draw %d", frame);
      fails += check(name, got, want);
      /* Change the texture so later frames depend on fresh uploads. */
      for (int i = 0; i < W * H; i++)
         want[i * 4 + 2] ^= 0x80;
      glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, W, H, GL_RGBA, GL_UNSIGNED_BYTE, want);
   }

   printf(fails ? "FAILED\n" : "PASSED\n");
   eglTerminate(dpy);
   return fails != 0;
}
