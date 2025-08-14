#include "kernel/types.h"
#include "kernel/fcntl.h"
#include "user/user.h"
#include "kernel/riscv.h"

char*
memmem(char *haystack, int haystack_len, char *needle, int needle_len)
{
  int i, j;
  if (needle_len == 0) return haystack;
  if (haystack_len < needle_len) return 0;

  for (i = 0; i <= haystack_len - needle_len; i++) {
    for (j = 0; j < needle_len; j++) {
      if (haystack[i+j] != needle[j]) {
        break;
      }
    }
    if (j == needle_len) {
      return &haystack[i];
    }
  }
  return 0;
}

int
main(int argc, char *argv[])
{
  char *base = sbrk(PGSIZE*64);

  //cannot rely on string first chars
  char *marker_to_find = "very very secret pw is:";
  
  char *found_marker = memmem(base, PGSIZE*64, marker_to_find, strlen(marker_to_find));

  char *password_start = found_marker + strlen(marker_to_find);

  write(2, password_start+1, 8);
  
  exit(0);
}
