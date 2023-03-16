#include <cstdint>
#include <string>

#include "tinyusdz.hh"

static void run(const uint8_t *data, size_t size)
{
  // Assume input is null separated 2 strings.
  if (size < 3) {
    return;
  }

  // disallow too large string
  if (size > (1024*1024*1024)) {
    return;
  }
  
  size_t loc = 0;
  for (; loc < size; loc++) {
    if (data[loc] == '\0') {
      break;
    }
  }

  if (loc == 0) {
    return;
  } else if (loc >= (size-1)) {
    return;
  }

  std::string s1 = std::string(reinterpret_cast<const char *>(data), loc-1);
  std::string s2 = std::string(reinterpret_cast<const char *>(data+loc), size-loc-1);
  
  return;
}

extern "C"
int LLVMFuzzerTestOneInput(std::uint8_t const* data, std::size_t size)
{
    run(data, size);
    return 0;
}
