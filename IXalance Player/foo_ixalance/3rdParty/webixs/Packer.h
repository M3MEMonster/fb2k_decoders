/*
* Handles zlib based data de-/compression.
*
*   Copyright (C) 2022 Juergen Wothke
*   Copyright (C) original x86 code: Shortcut Software Development BV
*
* LICENSE
*
*   This software is licensed under a CC BY-NC-SA
*   (http://creativecommons.org/licenses/by-nc-sa/4.0/).
*/

// todo: refactor into a regular C++ class

#ifndef IXS_PACKER_H
#define IXS_PACKER_H

#include "basetypes.h"

extern "C" {
#include "../miniz/miniz.h"
};

#ifndef z_const
#define z_const const
#endif

// miniz exposes zlib-compatible API, but not versioned zlib init entrypoints.
#ifndef deflateInit_
static inline int deflateInit_(z_stream* pStream, int level, const char*, int) {
  return deflateInit(pStream, level);
}
#endif

#ifndef inflateInit_
static inline int inflateInit_(z_stream* pStream, const char*, int) {
  return inflateInit(pStream);
}
#endif

namespace IXS {
  typedef struct ByteArray ByteArray;

  /**
   * Simple "byte array" data type.
   */
  struct ByteArray {
    int *bufPtr_0x0;
    uint bufSize_0x4;
  };

  ByteArray *__thiscall IXS__ByteArray__Z_ctor_0x40f050(ByteArray *byteArray);

  void __thiscall IXS__ByteArray__Z_resize_0x40f060(ByteArray *byteArray, size_t size);


  typedef struct IXS__Packer__VFTABLE IXS__Packer__VFTABLE;

  typedef struct Packer Packer;

  struct IXS__Packer__VFTABLE {
    ByteArray *(*fn1_deflate)(struct Packer *, struct ByteArray *, int);

    ByteArray *(*fn2_inflate)(struct Packer *, struct ByteArray *);

    void (*fn3_maybeTraceLog)(uint, uint, uint);

    void (*fn4_unusedFn)(uint);
  };

  /**
   * API used for ZLIB based data compression/decompression.
   */
  struct Packer {
    struct IXS__Packer__VFTABLE *vftptr_0x0;
    z_stream zstream_0x4;
    int zlib_status_0x3c;
    char abortFlag_0x40;
    byte field4_0x41;     // probably above is just an int
    byte field5_0x42;
    byte field6_0x43;
    struct ByteArray *byteArray_0x44;
    struct ByteArray *byteArray_0x48;
    int int_0x4c;
    int lenCount_0x50;
    byte destBuffer4096_0x54[4096];
    byte srcBuffer4096_0x1054[4096];
  };

  // "public methods"
  Packer *IXS__Packer__ctor_0040cea0();

  void __thiscall IXS__Packer__dtor_0x40ca60(Packer *packer);

  ByteArray *IXS__Packer__newByteArray_0040ce40();

}


#endif //IXS_PACKER_H
