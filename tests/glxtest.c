/* Renders frames to an X11 window with GLX, checks one pixel per frame and reports the frame rate. */
#include <GL/gl.h>
#include <GL/glx.h>
#include <X11/Xlib.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#define W 800
#define H 600
#define FRAMES 300

static double now(void)
{
   struct timespec ts;
   clock_gettime(CLOCK_MONOTONIC, &ts);
   return ts.tv_sec + ts.tv_nsec * 1e-9;
}

int main(void)
{
   Display *dpy = XOpenDisplay(NULL);
   if (!dpy) {
      fprintf(stderr, "cannot open display\n");
      return 1;
   }
   int attrs[] = {GLX_RGBA, GLX_DOUBLEBUFFER, GLX_RED_SIZE, 8, GLX_GREEN_SIZE, 8, GLX_BLUE_SIZE, 8, None};
   XVisualInfo *vi = glXChooseVisual(dpy, DefaultScreen(dpy), attrs);
   if (!vi) {
      fprintf(stderr, "no visual\n");
      return 1;
   }
   Window root = RootWindow(dpy, vi->screen);
   XSetWindowAttributes swa = {.colormap = XCreateColormap(dpy, root, vi->visual, AllocNone)};
   Window win = XCreateWindow(dpy, root, 0, 0, W, H, 0, vi->depth, InputOutput, vi->visual, CWColormap, &swa);
   XStoreName(dpy, win, "glxtest");
   XMapWindow(dpy, win);
   GLXContext ctx = glXCreateContext(dpy, vi, NULL, True);
   glXMakeCurrent(dpy, win, ctx);
   printf("GL_RENDERER: %s\n", glGetString(GL_RENDERER));
   glViewport(0, 0, W, H);

   int fails = 0;
   double start = now();
   for (int i = 0; i < FRAMES; i++) {
      float t = i / (float)FRAMES;
      glClearColor(t, 0.25f, 1 - t, 1);
      glClear(GL_COLOR_BUFFER_BIT);
      glLoadIdentity();
      glRotatef(i * 2.0f, 0, 0, 1);
      glBegin(GL_TRIANGLES);
      glColor3f(1, 0, 0);
      glVertex2f(-0.5f, -0.5f);
      glColor3f(0, 1, 0);
      glVertex2f(0.5f, -0.5f);
      glColor3f(0, 0, 1);
      glVertex2f(0, 0.5f);
      glEnd();
      unsigned char px[4];
      glReadPixels(0, 0, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, px);
      int want_r = (int)(t * 255 + 0.5f), want_b = (int)((1 - t) * 255 + 0.5f);
      if (abs(px[0] - want_r) > 1 || abs(px[1] - 64) > 1 || abs(px[2] - want_b) > 1) {
         if (fails++ < 5)
            printf("frame %d: corner %d,%d,%d want %d,64,%d\n", i, px[0], px[1], px[2], want_r, want_b);
      }
      glXSwapBuffers(dpy, win);
   }
   glFinish();
   double secs = now() - start;
   printf("%d frames at %dx%d in %.2f s: %.1f fps\n", FRAMES, W, H, secs, FRAMES / secs);
   printf(fails ? "FAILED\n" : "PASSED\n");
   glXMakeCurrent(dpy, None, NULL);
   glXDestroyContext(dpy, ctx);
   XCloseDisplay(dpy);
   return fails != 0;
}
