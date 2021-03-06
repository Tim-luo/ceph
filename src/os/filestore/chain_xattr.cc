// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab

#include "chain_xattr.h"
#include <errno.h>           // for ERANGE, ENODATA, ENOMEM
#include <stdio.h>           // for size_t, snprintf
#include <stdlib.h>          // for free, malloc
#include <string.h>          // for strcpy, strlen
#include "include/ceph_assert.h"  // for assert
#include "include/buffer.h"

#if defined(__linux__)
#include <linux/fs.h>
#endif

#include "include/ceph_assert.h"

using ceph::bufferptr;

/*
 * chaining xattrs
 *
 * In order to support xattrs that are larger than the xattr size limit that some file systems
 * impose, we use multiple xattrs to store the value of a single xattr. The xattrs keys
 * are set as follows:
 * The first xattr in the chain, has a key that holds the original xattr name, with any '@' char
 * being esacped ("@@").
 * The chained keys will have the first xattr's key (with the escaping), and a suffix: "@<id>"
 * where <id> marks the num of xattr in the chain.
 */

void get_raw_xattr_name(const char *name, int i, char *raw_name, int raw_len)
{
  int pos = 0;

  while (*name) {
    switch (*name) {
    case '@': /* escape it */
      pos += 2;
      ceph_assert (pos < raw_len - 1);
      *raw_name = '@';
      raw_name++;
      *raw_name = '@';
      break;
    default:
      pos++;
      ceph_assert(pos < raw_len - 1);
      *raw_name = *name;
      break;
    }
    name++;
    raw_name++;
  }

  if (!i) {
    *raw_name = '\0';
  } else {
    int r = snprintf(raw_name, raw_len - pos, "@%d", i);
    ceph_assert(r < raw_len - pos);
  }
}

static int translate_raw_name(const char *raw_name, char *name, int name_len, bool *is_first)
{
  int pos = 0;

  *is_first = true;
  while (*raw_name) {
    switch (*raw_name) {
    case '@': /* escape it */
      raw_name++;
      if (!*raw_name)
        break;
      if (*raw_name != '@') {
        *is_first = false;
        goto done;
      }

    /* fall through */
    default:
      *name = *raw_name;
      break;
    }
    pos++;
    ceph_assert(pos < name_len);
    name++;
    raw_name++;
  }
done:
  *name = '\0';
  return pos;
}


// setxattr

static int getxattr_len(const char *fn, const char *name)
{
  int i = 0, total = 0;
  char raw_name[CHAIN_XATTR_MAX_NAME_LEN * 2 + 16];
  int r;

  do {
    get_raw_xattr_name(name, i, raw_name, sizeof(raw_name));
    r = sys_getxattr(fn, raw_name, 0, 0);
    if (!i && r < 0)
      return r;
    if (r < 0)
      break;
    total += r;
    i++;
  } while (r == CHAIN_XATTR_MAX_BLOCK_LEN ||
	   r == CHAIN_XATTR_SHORT_BLOCK_LEN);

  return total;
}

int chain_getxattr(const char *fn, const char *name, void *val, size_t size)
{
  int i = 0, pos = 0;
  char raw_name[CHAIN_XATTR_MAX_NAME_LEN * 2 + 16];
  int ret = 0;
  int r;
  size_t chunk_size;

  if (!size)
    return getxattr_len(fn, name);

  do {
    chunk_size = size;
    get_raw_xattr_name(name, i, raw_name, sizeof(raw_name));

    r = sys_getxattr(fn, raw_name, (char *)val + pos, chunk_size);
    if (i && r == -ENODATA) {
      ret = pos;
      break;
    }
    if (r < 0) {
      ret = r;
      break;
    }

    if (r > 0) {
      pos += r;
      size -= r;
    }

    i++;
  } while (size && (r == CHAIN_XATTR_MAX_BLOCK_LEN ||
		    r == CHAIN_XATTR_SHORT_BLOCK_LEN));

  if (r >= 0) {
    ret = pos;
    /* is there another chunk? that can happen if the last read size span over
       exactly one block */
    if (chunk_size == CHAIN_XATTR_MAX_BLOCK_LEN ||
	chunk_size == CHAIN_XATTR_SHORT_BLOCK_LEN) {
      get_raw_xattr_name(name, i, raw_name, sizeof(raw_name));
      r = sys_getxattr(fn, raw_name, 0, 0);
      if (r > 0) { // there's another chunk.. the original buffer was too small
        ret = -ERANGE;
      }
    }
  }
  return ret;
}

int chain_getxattr_buf(const char *fn, const char *name, bufferptr *bp)
{
  size_t size = 1024; // Initial
  while (1) {
    bufferptr buf(size);
    int r = chain_getxattr(
      fn,
      name,
      buf.c_str(),
      size);
    if (r > 0) {
      buf.set_length(r);
      if (bp)
	bp->swap(buf);
      return r;
    } else if (r == 0) {
      return 0;
    } else {
      if (r == -ERANGE) {
	size *= 2;
      } else {
	return r;
      }
    }
  }
  ceph_abort_msg("unreachable");
  return 0;
}

static int chain_fgetxattr_len(int fd, const char *name)
{
  int i = 0, total = 0;
  char raw_name[CHAIN_XATTR_MAX_NAME_LEN * 2 + 16];
  int r;

  do {
    get_raw_xattr_name(name, i, raw_name, sizeof(raw_name));
    r = sys_fgetxattr(fd, raw_name, 0, 0);
    if (!i && r < 0)
      return r;
    if (r < 0)
      break;
    total += r;
    i++;
  } while (r == CHAIN_XATTR_MAX_BLOCK_LEN ||
	   r == CHAIN_XATTR_SHORT_BLOCK_LEN);

  return total;
}

int chain_fgetxattr(int fd, const char *name, void *val, size_t size)
{
  int i = 0, pos = 0;
  char raw_name[CHAIN_XATTR_MAX_NAME_LEN * 2 + 16];
  int ret = 0;
  int r;
  size_t chunk_size;

  if (!size)
    return chain_fgetxattr_len(fd, name);

  do {
    chunk_size = size;
    get_raw_xattr_name(name, i, raw_name, sizeof(raw_name));

    r = sys_fgetxattr(fd, raw_name, (char *)val + pos, chunk_size);
    if (i && r == -ENODATA) {
      ret = pos;
      break;
    }
    if (r < 0) {
      ret = r;
      break;
    }

    if (r > 0) {
      pos += r;
      size -= r;
    }

    i++;
  } while (size && (r == CHAIN_XATTR_MAX_BLOCK_LEN ||
		    r == CHAIN_XATTR_SHORT_BLOCK_LEN));

  if (r >= 0) {
    ret = pos;
    /* is there another chunk? that can happen if the last read size span over
       exactly one block */
    if (chunk_size == CHAIN_XATTR_MAX_BLOCK_LEN ||
	chunk_size == CHAIN_XATTR_SHORT_BLOCK_LEN) {
      get_raw_xattr_name(name, i, raw_name, sizeof(raw_name));
      r = sys_fgetxattr(fd, raw_name, 0, 0);
      if (r > 0) { // there's another chunk.. the original buffer was too small
        ret = -ERANGE;
      }
    }
  }
  return ret;
}


// setxattr

int get_xattr_block_size(size_t size)
{
  if (size <= CHAIN_XATTR_SHORT_LEN_THRESHOLD)
    // this may fit in the inode; stripe over short attrs so that XFS
    // won't kick it out.
    return CHAIN_XATTR_SHORT_BLOCK_LEN;
  return CHAIN_XATTR_MAX_BLOCK_LEN;
}

// removexattr

int chain_removexattr(const char *fn, const char *name)
{
  int i = 0;
  char raw_name[CHAIN_XATTR_MAX_NAME_LEN * 2 + 16];
  int r;

  do {
    get_raw_xattr_name(name, i, raw_name, sizeof(raw_name));
    r = sys_removexattr(fn, raw_name);
    if (!i && r < 0) {
      return r;
    }
    i++;
  } while (r >= 0);
  return 0;
}

int chain_fremovexattr(int fd, const char *name)
{
  int i = 0;
  char raw_name[CHAIN_XATTR_MAX_NAME_LEN * 2 + 16];
  int r;

  do {
    get_raw_xattr_name(name, i, raw_name, sizeof(raw_name));
    r = sys_fremovexattr(fd, raw_name);
    if (!i && r < 0) {
      return r;
    }
    i++;
  } while (r >= 0);
  return 0;
}


// listxattr

int chain_listxattr(const char *fn, char *names, size_t len) {
  int r;

  if (!len)
    return sys_listxattr(fn, names, len) * 2;

  r = sys_listxattr(fn, 0, 0);
  if (r < 0)
    return r;

  size_t total_len = r * 2; // should be enough
  char *full_buf = (char *)malloc(total_len);
  if (!full_buf)
    return -ENOMEM;

  r = sys_listxattr(fn, full_buf, total_len);
  if (r < 0) {
    free(full_buf);
    return r;
  }

  char *p = full_buf;
  const char *end = full_buf + r;
  char *dest = names;
  char *dest_end = names + len;

  while (p < end) {
    char name[CHAIN_XATTR_MAX_NAME_LEN * 2 + 16];
    int attr_len = strlen(p);
    bool is_first;
    int name_len = translate_raw_name(p, name, sizeof(name), &is_first);
    if (is_first)  {
      if (dest + name_len > dest_end) {
        r = -ERANGE;
        goto done;
      }
      strcpy(dest, name);
      dest += name_len + 1;
    }
    p += attr_len + 1;
  }
  r = dest - names;

done:
  free(full_buf);
  return r;
}

int chain_flistxattr(int fd, char *names, size_t len) {
  int r;
  char *p;
  const char * end;
  char *dest;
  char *dest_end;

  if (!len)
    return sys_flistxattr(fd, names, len) * 2;

  r = sys_flistxattr(fd, 0, 0);
  if (r < 0)
    return r;

  size_t total_len = r * 2; // should be enough
  char *full_buf = (char *)malloc(total_len);
  if (!full_buf)
    return -ENOMEM;

  r = sys_flistxattr(fd, full_buf, total_len);
  if (r < 0)
    goto done;

  p = full_buf;
  end = full_buf + r;
  dest = names;
  dest_end = names + len;

  while (p < end) {
    char name[CHAIN_XATTR_MAX_NAME_LEN * 2 + 16];
    int attr_len = strlen(p);
    bool is_first;
    int name_len = translate_raw_name(p, name, sizeof(name), &is_first);
    if (is_first)  {
      if (dest + name_len > dest_end) {
        r = -ERANGE;
        goto done;
      }
      strcpy(dest, name);
      dest += name_len + 1;
    }
    p += attr_len + 1;
  }
  r = dest - names;

done:
  free(full_buf);
  return r;
}
