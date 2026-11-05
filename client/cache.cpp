// Replies to requests whose answer does not change, keyed by the request
// bytes. Callers hold the wire lock.
#include <chrono>
#include <string>
#include <unordered_map>

#include "cache.h"

using Clock = std::chrono::steady_clock;

struct Entry {
   std::string reply;
   Clock::time_point expires;
};

static std::unordered_map<std::string, Entry> entries;

const void *cache_find(const void *request, size_t size, size_t *reply_size)
{
   auto it = entries.find(std::string(static_cast<const char *>(request), size));
   if (it == entries.end())
      return nullptr;
   if (it->second.expires != Clock::time_point() && Clock::now() > it->second.expires) {
      entries.erase(it);
      return nullptr;
   }
   *reply_size = it->second.reply.size();
   return it->second.reply.data();
}

void cache_store(const void *request, size_t size, const void *reply, size_t reply_size, unsigned ttl_ms)
{
   // Requests that embed sizes or formats can be unbounded in number.
   if (entries.size() >= 4096)
      entries.clear();
   Entry &e = entries[std::string(static_cast<const char *>(request), size)];
   e.reply.assign(static_cast<const char *>(reply), reply_size);
   e.expires = ttl_ms ? Clock::now() + std::chrono::milliseconds(ttl_ms) : Clock::time_point();
}
