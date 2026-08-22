#ifndef T113_LIBJPEG_JCONFIG_H
#define T113_LIBJPEG_JCONFIG_H

/* Minimal portable configuration for applications using the libjpeg API.
 * The library itself supplies its ABI-specific configuration at build time. */
#define JPEG_LIB_VERSION 90
#define BITS_IN_JSAMPLE 8
#define HAVE_STDDEF_H 1
#define HAVE_STDLIB_H 1
#define HAVE_UNSIGNED_CHAR 1
#define HAVE_UNSIGNED_SHORT 1
#define MEM_SRCDST_SUPPORTED 1

#endif
