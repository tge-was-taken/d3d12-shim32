/* CPU-visible GPU memory lives in the server's address space, so the client
 * maps a shared memfd instead and the server copies between the two. Pages the
 * client writes are found with userfaultfd write protection and PAGEMAP_SCAN,
 * and are sent with each ExecuteCommandLists. */
#define _GNU_SOURCE
#include <fcntl.h>
#include <linux/fs.h>
#include <linux/userfaultfd.h>
#include <pthread.h>
#include <stdlib.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <unistd.h>

#include "shim.h"

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static struct shadow *shadows;
static int uffd = -1, pagemap = -1;
static bool tracking_checked;

/* Shadows are placed in one memfd mapped once when they fit, so a single
 * PAGEMAP_SCAN over one mapping finds the written pages of all of them. The
 * scan costs about 0.2 us per mapping, which adds up with hundreds of buffers. */
#define PAGE 4096u
#define ARENA_PAGES ((256u << 20) / PAGE)
static char *arena;
static int arena_fd = -1;
static struct shadow **arena_owner;  /* per page, set once attached */
struct extent {
   uint32_t start, len;
};
static struct extent *free_ext;  /* sorted by start */
static size_t nfree_ext;

static bool register_wp(void *p, size_t size)
{
   struct uffdio_register reg = {
      .range = {(uintptr_t)p, size},
      .mode = UFFDIO_REGISTER_MODE_WP,
   };
   return !ioctl(uffd, UFFDIO_REGISTER, &reg);
}

static void init_arena(void)
{
   size_t size = (size_t)ARENA_PAGES * PAGE;
   int fd = memfd_create("d3d12-shadows", MFD_CLOEXEC);
   void *p = MAP_FAILED;
   if (fd >= 0 && !ftruncate(fd, size))
      p = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
   if (p == MAP_FAILED || !register_wp(p, size)) {
      if (p != MAP_FAILED)
         munmap(p, size);
      if (fd >= 0)
         close(fd);
      return;
   }
   arena = p;
   arena_fd = fd;
   arena_owner = calloc(ARENA_PAGES, sizeof(*arena_owner));
   free_ext = malloc(sizeof(*free_ext));
   free_ext[0] = (struct extent){0, ARENA_PAGES};
   nfree_ext = 1;
}

/* First fit; returns NULL when no free extent is large enough. */
static char *arena_alloc(size_t size)
{
   uint32_t pages = size / PAGE;
   for (size_t i = 0; i < nfree_ext; i++) {
      struct extent *e = &free_ext[i];
      if (e->len < pages)
         continue;
      char *p = arena + (size_t)e->start * PAGE;
      e->start += pages;
      e->len -= pages;
      if (!e->len)
         memmove(e, e + 1, (--nfree_ext - i) * sizeof(*e));
      return p;
   }
   return NULL;
}

static void arena_free(char *p, size_t size)
{
   uint32_t start = (p - arena) / PAGE, len = size / PAGE;
   size_t i = 0;
   while (i < nfree_ext && free_ext[i].start < start)
      i++;
   bool join_prev = i > 0 && free_ext[i - 1].start + free_ext[i - 1].len == start;
   bool join_next = i < nfree_ext && start + len == free_ext[i].start;
   if (join_prev && join_next) {
      free_ext[i - 1].len += len + free_ext[i].len;
      memmove(&free_ext[i], &free_ext[i + 1], (--nfree_ext - i) * sizeof(*free_ext));
   } else if (join_prev) {
      free_ext[i - 1].len += len;
   } else if (join_next) {
      free_ext[i].start = start;
      free_ext[i].len += len;
   } else {
      free_ext = realloc(free_ext, (nfree_ext + 1) * sizeof(*free_ext));
      memmove(&free_ext[i + 1], &free_ext[i], (nfree_ext - i) * sizeof(*free_ext));
      free_ext[i] = (struct extent){start, len};
      nfree_ext++;
   }
}

/* Pages from the arena start up to the last one in use. */
static size_t arena_used(void)
{
   struct extent *last = nfree_ext ? &free_ext[nfree_ext - 1] : NULL;
   uint32_t end = last && last->start + last->len == ARENA_PAGES ? last->start : ARENA_PAGES;
   return (size_t)end * PAGE;
}

static bool in_arena(const struct shadow *s)
{
   return arena && s->ptr >= arena && s->ptr < arena + (size_t)ARENA_PAGES * PAGE;
}

static void init_tracking(void)
{
   tracking_checked = true;
   uffd = syscall(SYS_userfaultfd, O_CLOEXEC | UFFD_USER_MODE_ONLY);
   if (uffd < 0)
      goto fail;
   struct uffdio_api api = {
      .api = UFFD_API,
      .features = UFFD_FEATURE_WP_ASYNC | UFFD_FEATURE_WP_HUGETLBFS_SHMEM | UFFD_FEATURE_WP_UNPOPULATED,
   };
   pagemap = open("/proc/self/pagemap", O_RDONLY | O_CLOEXEC);
   if (ioctl(uffd, UFFDIO_API, &api) || pagemap < 0)
      goto fail;
   init_arena();
   return;
fail:
   shim_warn("no write tracking, CPU-visible memory is copied in full on every submit");
   if (uffd >= 0)
      close(uffd);
   uffd = -1;
}

/* Pages without protection would be reported as written, even if only read. */
static bool protect(struct shadow *s, bool registered)
{
   struct uffdio_writeprotect wp = {
      .range = {(uintptr_t)s->ptr, s->size},
      .mode = UFFDIO_WRITEPROTECT_MODE_WP,
   };
   return (registered || register_wp(s->ptr, s->size)) && !ioctl(uffd, UFFDIO_WRITEPROTECT, &wp);
}

struct shadow *shadow_create(size_t size, enum cpu_access access, int *fd)
{
   size = (size + 4095) & ~(size_t)4095;
   pthread_mutex_lock(&lock);
   if (!tracking_checked)
      init_tracking();
   char *p = arena ? arena_alloc(size) : NULL;
   if (p) {
      *fd = dup(arena_fd);
   } else {
      *fd = memfd_create("d3d12-shadow", MFD_CLOEXEC);
      if (*fd >= 0 && !ftruncate(*fd, size))
         p = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, *fd, 0);
      if (p == MAP_FAILED)
         p = NULL;
   }
   if (!p || *fd < 0) {
      if (p && in_arena(&(struct shadow){.ptr = p}))
         arena_free(p, size);
      else if (p)
         munmap(p, size);
      pthread_mutex_unlock(&lock);
      goto fail;
   }

   struct shadow *s = calloc(1, sizeof(*s));
   s->ptr = p;
   s->size = size;
   s->access = access;
   s->offset = in_arena(s) ? p - arena : 0;
   if (uffd >= 0 && !protect(s, in_arena(s))) {
      shim_warn("cannot write-protect shadow memory");
      close(uffd);
      uffd = -1;
   }
   pthread_mutex_unlock(&lock);
   return s;

fail:
   if (*fd >= 0)
      close(*fd);
   *fd = -1;
   return NULL;
}

/* Starts syncing once the server has accepted the shadow. */
void shadow_attach(struct shadow *s, uint32_t owner)
{
   s->owner = owner;
   pthread_mutex_lock(&lock);
   s->next = shadows;
   shadows = s;
   if (in_arena(s))
      for (size_t i = 0; i < s->size / PAGE; i++)
         arena_owner[(s->ptr - arena) / PAGE + i] = s;
   pthread_mutex_unlock(&lock);
}

void shadow_destroy(struct shadow *s)
{
   if (!s)
      return;
   pthread_mutex_lock(&lock);
   for (struct shadow **p = &shadows; *p; p = &(*p)->next) {
      if (*p == s) {
         *p = s->next;
         break;
      }
   }
   if (in_arena(s)) {
      for (size_t i = 0; i < s->size / PAGE; i++)
         arena_owner[(s->ptr - arena) / PAGE + i] = NULL;
      /* Frees the memory and zeroes it for the next shadow placed here. */
      fallocate(arena_fd, FALLOC_FL_PUNCH_HOLE | FALLOC_FL_KEEP_SIZE, s->offset, s->size);
      arena_free(s->ptr, s->size);
   } else {
      munmap(s->ptr, s->size);
   }
   pthread_mutex_unlock(&lock);
   free(s);
}

struct range {
   uint32_t start, end;
};

/* Calls `found` for each run of written pages in [start, end), or returns
 * false if the scan fails. Written pages are protected again. */
static bool scan_written(char *start, char *end, void (*found)(char *start, char *end, void *ctx), void *ctx)
{
   struct page_region regs[64];
   struct pm_scan_arg arg = {
      .size = sizeof(arg),
      .flags = PM_SCAN_WP_MATCHING | PM_SCAN_CHECK_WPASYNC,
      .start = (uintptr_t)start,
      .end = (uintptr_t)end,
      .vec = (uintptr_t)regs,
      .vec_len = ARRAY_SIZE(regs),
      .category_mask = PAGE_IS_WRITTEN,
      .return_mask = PAGE_IS_WRITTEN,
   };
   while (arg.start < arg.end) {
      long got = ioctl(pagemap, PAGEMAP_SCAN, &arg);
      if (got < 0)
         return false;
      for (long i = 0; i < got; i++)
         found((char *)(uintptr_t)regs[i].start, (char *)(uintptr_t)regs[i].end, ctx);
      arg.start = arg.walk_end;
   }
   return true;
}

/* Writes one shadow's entry of the dirty list: owner, count, ranges. */
struct dirty {
   struct shadow *cur;
   size_t n_at;
   uint32_t n, count;
};

static void dirty_add(struct dirty *d, struct shadow *s, uint32_t start, uint32_t end)
{
   if (s != d->cur) {
      if (d->cur)
         memcpy(wire_out + d->n_at, &d->n, 4);
      w_u32(s->owner);
      w_u32(0);
      d->n_at = wire_len - 4;
      d->cur = s;
      d->n = 0;
      d->count++;
   }
   w_u32(start);
   w_u32(end);
   d->n++;
}

static void dirty_finish(struct dirty *d)
{
   if (d->cur)
      memcpy(wire_out + d->n_at, &d->n, 4);
}

/* Splits a written run in the arena by the shadows that own its pages. */
static void arena_found(char *start, char *end, void *ctx)
{
   while (start < end) {
      struct shadow *s = arena_owner[(start - arena) / PAGE];
      if (!s) {
         start += PAGE;
         continue;
      }
      char *stop = end < s->ptr + s->size ? end : s->ptr + s->size;
      dirty_add(ctx, s, start - s->ptr, stop - s->ptr);
      start = stop;
   }
}

struct shadow_scan {
   struct dirty *d;
   struct shadow *s;
};

static void shadow_found(char *start, char *end, void *ctx)
{
   struct shadow_scan *sc = ctx;
   dirty_add(sc->d, sc->s, start - sc->s->ptr, end - sc->s->ptr);
}

/* Writes the pages written since the last call, as consumed by OP_QUEUE_EXECUTE. */
void shadow_write_dirty(void)
{
   pthread_mutex_lock(&lock);
   w_u32(0);
   size_t count_at = wire_len - 4;
   struct dirty d = {0};
   bool arena_failed = false;
   if (arena && uffd >= 0 && !scan_written(arena, arena + arena_used(), arena_found, &d)) {
      shim_warn("PAGEMAP_SCAN failed, syncing whole shadows");
      arena_failed = true;
   }
   for (struct shadow *s = shadows; s; s = s->next) {
      if (uffd < 0) {
         if (s->access == CPU_WRITE)
            dirty_add(&d, s, 0, s->size);
      } else if (in_arena(s)) {
         if (arena_failed)
            dirty_add(&d, s, 0, s->size);
      } else if (!scan_written(s->ptr, s->ptr + s->size, shadow_found, &(struct shadow_scan){&d, s})) {
         shim_warn("PAGEMAP_SCAN failed, syncing whole shadow");
         dirty_add(&d, s, 0, s->size);
      }
   }
   dirty_finish(&d);
   memcpy(wire_out + count_at, &d.count, 4);
   pthread_mutex_unlock(&lock);
}
