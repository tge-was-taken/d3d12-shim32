#define _GNU_SOURCE
#include <errno.h>
#include <poll.h>
#include <pthread.h>
#include <spawn.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "cache.h"
#include "shim.h"

#ifndef SERVER_PATH
#define SERVER_PATH "d3d12-shim-server"
#endif

#define FLUSH_SIZE (256 * 1024)
#define MAX_FDS 16

extern char **environ;

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static int sock = -1;
static bool dead;

char *wire_out;
size_t wire_len, wire_cap;
static size_t msg_start;
static int fds[MAX_FDS];
static int nfds;

static char *in;
static size_t in_len, in_pos, in_cap;

void shim_warn(const char *fmt, ...)
{
   va_list ap;
   va_start(ap, fmt);
   fputs("d3d12-shim: ", stderr);
   vfprintf(stderr, fmt, ap);
   fputc('\n', stderr);
   va_end(ap);
}

static void reset_after_fork(void)
{
   /* The child must not share the parent's server. */
   if (sock >= 0)
      close(sock);
   sock = -1;
   dead = false;
   wire_len = 0;
   nfds = 0;
   pthread_mutex_init(&lock, NULL);
}

static bool connect_server(void)
{
   static bool atfork;
   if (!atfork) {
      pthread_atfork(NULL, NULL, reset_after_fork);
      atfork = true;
   }

   int sv[2];
   if (socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sv))
      return false;

   const char *path = getenv("D3D12_SHIM_SERVER");
   if (!path)
      path = SERVER_PATH;

   posix_spawn_file_actions_t fa;
   posix_spawn_file_actions_init(&fa);
   posix_spawn_file_actions_adddup2(&fa, sv[1], SHIM_SERVER_FD);
   char *argv[] = {(char *)path, NULL};
   pid_t pid;
   int err = posix_spawn(&pid, path, &fa, NULL, argv, environ);
   posix_spawn_file_actions_destroy(&fa);
   close(sv[1]);
   if (err) {
      shim_warn("cannot start %s: %s", path, strerror(err));
      close(sv[0]);
      return false;
   }
   sock = sv[0];
   return true;
}

static void grow(char **buf, size_t *cap, size_t need)
{
   if (need <= *cap)
      return;
   size_t n = *cap ? *cap : 65536;
   while (n < need)
      n *= 2;
   *buf = realloc(*buf, n);
   *cap = n;
}

bool server_lost(void)
{
   return __atomic_load_n(&dead, __ATOMIC_RELAXED);
}

static void fail(const char *what)
{
   if (!dead)
      shim_warn("lost connection to server (%s)", what);
   dead = true;
}

static void flush(void)
{
   size_t pos = 0;
   while (!dead && (pos < wire_len || nfds)) {
      struct iovec iov = {wire_out + pos, wire_len - pos};
      union {
         char buf[CMSG_SPACE(sizeof(fds))];
         struct cmsghdr align;
      } ctrl;
      struct msghdr mh = {.msg_iov = &iov, .msg_iovlen = 1};
      if (nfds) {
         mh.msg_control = ctrl.buf;
         mh.msg_controllen = CMSG_SPACE(nfds * sizeof(int));
         struct cmsghdr *c = CMSG_FIRSTHDR(&mh);
         c->cmsg_level = SOL_SOCKET;
         c->cmsg_type = SCM_RIGHTS;
         c->cmsg_len = CMSG_LEN(nfds * sizeof(int));
         memcpy(CMSG_DATA(c), fds, nfds * sizeof(int));
      }
      ssize_t n = sendmsg(sock, &mh, MSG_NOSIGNAL);
      if (n < 0 && errno == EINTR)
         continue;
      if (n <= 0) {
         fail("send");
         break;
      }
      pos += n;
      nfds = 0;
   }
   wire_len = 0;
   nfds = 0;
}

void msg_begin(uint32_t op)
{
   pthread_mutex_lock(&lock);
   if (sock < 0 && !dead && !connect_server())
      dead = true;
   grow(&wire_out, &wire_cap, wire_len + 8);
   msg_start = wire_len;
   memcpy(wire_out + wire_len, &op, 4);
   wire_len += 8;
}

static void msg_finish(void)
{
   uint32_t size = wire_len - msg_start - 8;
   memcpy(wire_out + msg_start + 4, &size, 4);
   w_align(8);
}

/* Requests that carry fds are sent at once, so at most one fd is ever pending. */
void msg_fd(int fd)
{
   if (nfds < MAX_FDS)
      fds[nfds++] = fd;
}

void msg_send(void)
{
   msg_finish();
   if (wire_len >= FLUSH_SIZE || dead)
      flush();
   pthread_mutex_unlock(&lock);
}

/* Sends the queued requests now, so the server starts on them while the
 * caller goes on. */
void msg_send_flush(void)
{
   msg_finish();
   flush();
   pthread_mutex_unlock(&lock);
}

/* Waits until fd is readable or the server is gone. */
void msg_wait_fd(int fd)
{
   struct pollfd p[2] = {{fd, POLLIN, 0}, {sock, POLLRDHUP, 0}};
   while (!dead && !p[0].revents && !(p[1].revents & (POLLRDHUP | POLLHUP | POLLERR)))
      if (poll(p, 2, -1) < 0 && errno != EINTR)
         break;
}

static bool read_full(void *buf, size_t size)
{
   size_t pos = 0;
   while (pos < size) {
      ssize_t n = read(sock, (char *)buf + pos, size - pos);
      if (n < 0 && errno == EINTR)
         continue;
      if (n <= 0)
         return false;
      pos += n;
   }
   return true;
}

static bool call(void)
{
   flush();
   in_len = in_pos = 0;
   if (dead)
      return false;
   uint32_t hdr[2];
   if (!read_full(hdr, sizeof(hdr))) {
      fail("receive");
      return false;
   }
   grow(&in, &in_cap, hdr[0]);
   if (!read_full(in, (hdr[0] + 7) & ~7u)) {
      fail("receive");
      return false;
   }
   in_len = hdr[0];
   return true;
}

bool msg_call(void)
{
   msg_finish();
   return call();
}

bool msg_call_cached(unsigned ttl_ms)
{
   msg_finish();
   const char *request = wire_out + msg_start;
   size_t size = wire_len - msg_start, reply_size;
   const void *reply = cache_find(request, size, &reply_size);
   if (reply) {
      wire_len = msg_start;
      grow(&in, &in_cap, reply_size);
      memcpy(in, reply, reply_size);
      in_len = reply_size;
      in_pos = 0;
      return true;
   }
   /* flush() leaves the sent bytes in place, so `request` stays valid. */
   if (!call())
      return false;
   cache_store(request, size, in, in_len, ttl_ms);
   return true;
}

void msg_end(void)
{
   pthread_mutex_unlock(&lock);
}

void wire_grow(size_t need)
{
   grow(&wire_out, &wire_cap, need);
}

void w_blob(const void *p, size_t size)
{
   w_u32(size);
   w_raw(p, size, 8);
}

void r_align(unsigned align)
{
   in_pos = (in_pos + align - 1) / align * align;
}

static void r_get(void *p, size_t size, unsigned align)
{
   r_align(align);
   if (in_pos + size > in_len) {
      memset(p, 0, size);
      in_pos = in_len;
      return;
   }
   memcpy(p, in + in_pos, size);
   in_pos += size;
}

uint32_t r_u32(void) { uint32_t v; r_get(&v, 4, 4); return v; }
uint64_t r_u64(void) { uint64_t v; r_get(&v, 8, 8); return v; }
void r_raw(void *p, size_t size, unsigned align) { r_get(p, size, align); }

HRESULT r_hr(void)
{
   if (in_len == 0)
      return DXGI_ERROR_DEVICE_REMOVED;
   return (HRESULT)r_u32();
}

const void *r_blob_ptr(size_t *size)
{
   *size = r_u32();
   r_align(8);
   if (in_pos + *size > in_len)
      *size = 0;
   const void *p = in + in_pos;
   in_pos += *size;
   return p;
}

size_t r_blob(void *p, size_t max)
{
   size_t size;
   const void *src = r_blob_ptr(&size);
   memcpy(p, src, size < max ? size : max);
   return size;
}
