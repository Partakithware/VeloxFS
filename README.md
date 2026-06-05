> [!IMPORTANT]
> Coming soon to a repo near you...

Oh hello! This is just like v6 from latest but stripped of all those pesky deps.
Enjoy!

```
/* veloxfs.h - v6.0bm
 *
 * Storage-Optimized Edition: Flash / NAND / HDD
 *
 * STANDALONE DESIGN
 * =================
 * This header has zero hard dependencies on any system library.  All system
 * calls (malloc, memcpy, printf, time …) go through overridable macros.
 * Define your replacements BEFORE including this header:
 *
 *   // Kernel module example:
 *   #include <linux/slab.h>
 *   #include <linux/string.h>
 *   #include <linux/printk.h>
 *   #define veloxfs_MALLOC(sz)         kmalloc(sz, GFP_KERNEL)
 *   #define veloxfs_CALLOC(n, sz)      kzalloc((n)*(sz), GFP_KERNEL)
 *   #define veloxfs_FREE(p)            kfree(p)
 *   #define veloxfs_MEMSET(d,c,n)      memset(d,c,n)
 *   #define veloxfs_MEMCPY(d,s,n)      memcpy(d,s,n)
 *   #define veloxfs_MEMMOVE(d,s,n)     memmove(d,s,n)
 *   #define veloxfs_STRLEN(s)          strlen(s)
 *   #define veloxfs_STRCMP(a,b)        strcmp(a,b)
 *   #define veloxfs_STRNCMP(a,b,n)     strncmp(a,b,n)
 *   #define veloxfs_STRCHR(s,c)        strchr(s,c)
 *   #define veloxfs_STRNCPY(d,s,n)     strncpy(d,s,n)
 *   #define veloxfs_SNPRINTF(d,n,...)  snprintf(d,n,__VA_ARGS__)
 *   #define veloxfs_LOG(fmt,...)       printk(KERN_INFO fmt, ##__VA_ARGS__)
 *   #define veloxfs_TIME()             ktime_get_real_seconds()
 *   #define veloxfs_IMPLEMENTATION
 *   #include "veloxfs.h"
```
