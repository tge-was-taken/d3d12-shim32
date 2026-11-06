/* Blits depth buffers upside down, which Mesa's d3d12 driver turns into one
 * texture copy per row, and checks the result against the source. */
#define GL_GLEXT_PROTOTYPES
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GL/gl.h>
#include <GL/glext.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define W 160
#define H 120

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

static GLuint depth_fb(GLenum format)
{
   GLuint tex, fb;
   glGenTextures(1, &tex);
   glBindTexture(GL_TEXTURE_2D, tex);
   glTexStorage2D(GL_TEXTURE_2D, 1, format, W, H);
   glGenFramebuffers(1, &fb);
   glBindFramebuffer(GL_FRAMEBUFFER, fb);
   GLenum attachment = format == GL_DEPTH24_STENCIL8 ? GL_DEPTH_STENCIL_ATTACHMENT : GL_DEPTH_ATTACHMENT;
   glFramebufferTexture2D(GL_FRAMEBUFFER, attachment, GL_TEXTURE_2D, tex, 0);
   glDrawBuffer(GL_NONE);
   glReadBuffer(GL_NONE);
   if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
      fprintf(stderr, "framebuffer for %#x incomplete\n", format);
      exit(1);
   }
   return fb;
}

static void read_depth(GLuint fb, float *out)
{
   glBindFramebuffer(GL_READ_FRAMEBUFFER, fb);
   glReadPixels(0, 0, W, H, GL_DEPTH_COMPONENT, GL_FLOAT, out);
}

/* Copies src rect (sx, sy, w, h) into dst at (dx, dy) upside down, then checks
 * every dst texel against the flipped source or the clear value. */
static int test(const char *name, GLenum format, int sx, int sy, int w, int h, int dx, int dy)
{
   static float src[W * H], got[W * H];
   GLuint a = depth_fb(format), b = depth_fb(format);

   glBindFramebuffer(GL_FRAMEBUFFER, a);
   glViewport(0, 0, W, H);
   glDrawArrays(GL_TRIANGLES, 0, 3);
   glBindFramebuffer(GL_FRAMEBUFFER, b);
   glClearDepth(0.25);
   glClear(GL_DEPTH_BUFFER_BIT);

   glBindFramebuffer(GL_READ_FRAMEBUFFER, a);
   glBindFramebuffer(GL_DRAW_FRAMEBUFFER, b);
   glBlitFramebuffer(sx, sy, sx + w, sy + h, dx, dy + h, dx + w, dy, GL_DEPTH_BUFFER_BIT, GL_NEAREST);

   read_depth(a, src);
   read_depth(b, got);
   for (int y = 1; y < H; y++)
      if (src[y * W] == src[(y - 1) * W]) {
         printf("%s: FAIL, source rows %d and %d match\n", name, y - 1, y);
         return 1;
      }
   for (int y = 0; y < H; y++)
      for (int x = 0; x < W; x++) {
         bool inside = x >= dx && x < dx + w && y >= dy && y < dy + h;
         float want = inside ? src[(sy + h - 1 - (y - dy)) * W + sx + x - dx] : 0.25f;
         if (got[y * W + x] != want) {
            printf("%s: FAIL at %d,%d: got %.9g want %.9g\n", name, x, y, got[y * W + x], want);
            return 1;
         }
      }
   printf("%s: ok\n", name);
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
   eglBindAPI(EGL_OPENGL_API);
   static const EGLint ctx_attrs[] = {EGL_CONTEXT_MAJOR_VERSION, 3, EGL_CONTEXT_MINOR_VERSION, 3,
                                      EGL_CONTEXT_OPENGL_PROFILE_MASK, EGL_CONTEXT_OPENGL_CORE_PROFILE_BIT, EGL_NONE};
   EGLContext ctx = eglCreateContext(dpy, EGL_NO_CONFIG_KHR, EGL_NO_CONTEXT, ctx_attrs);
   if (!ctx || !eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, ctx)) {
      fprintf(stderr, "cannot create context: %#x\n", eglGetError());
      return 1;
   }
   printf("GL_RENDERER: %s\n", glGetString(GL_RENDERER));

   /* A full screen triangle writing a depth that differs per row and column. */
   GLuint prog = glCreateProgram();
   glAttachShader(prog, shader(GL_VERTEX_SHADER,
                               "#version 330\n"
                               "void main() { gl_Position = vec4(gl_VertexID == 1 ? 3.0 : -1.0, gl_VertexID == 2 ? 3.0 : -1.0, 0.0, 1.0); }\n"));
   glAttachShader(prog, shader(GL_FRAGMENT_SHADER,
                               "#version 330\n"
                               "void main() {\n"
                               "   uint x = uint(gl_FragCoord.x), y = uint(gl_FragCoord.y);\n"
                               "   gl_FragDepth = float(y * 509u + (x * 37u) % 509u) / 65536.0;\n"
                               "}\n"));
   glLinkProgram(prog);
   glUseProgram(prog);
   GLuint vao;
   glGenVertexArrays(1, &vao);
   glBindVertexArray(vao);
   glEnable(GL_DEPTH_TEST);
   glDepthFunc(GL_ALWAYS);

   int fails = 0;
   fails += test("D32F full", GL_DEPTH_COMPONENT32F, 0, 0, W, H, 0, 0);
   fails += test("D32F box", GL_DEPTH_COMPONENT32F, 24, 10, 100, 70, 8, 30);
   fails += test("D16 full", GL_DEPTH_COMPONENT16, 0, 0, W, H, 0, 0);
   fails += test("D24S8 full", GL_DEPTH24_STENCIL8, 0, 0, W, H, 0, 0);
   fails += test("D32F 5 rows", GL_DEPTH_COMPONENT32F, 0, 50, W, 5, 0, 7);

   printf(fails ? "FAILED\n" : "PASSED\n");
   eglTerminate(dpy);
   return fails != 0;
}
