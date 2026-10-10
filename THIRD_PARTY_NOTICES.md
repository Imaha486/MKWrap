# Third-party notices

## unshield (third_party/unshield)

InstallShield cabinet extraction library. https://github.com/twogood/unshield
MIT License, Copyright (c) 2003-2021 David Eriksson and contributors. See `third_party/unshield/LICENSE`.

Local changes: `internal.h` includes `unshield_config.h` from the same directory, and a fixed
`unshield_config.h` is provided instead of the CMake-generated one (uses the bundled MD5).
The bundled `md5/` is the RSA Data Security, Inc. MD5 Message-Digest Algorithm reference implementation.

## blast (third_party/blast)

PKWARE Data Compression Library (DCL) "implode" decompressor, from zlib `contrib/blast`.
Copyright (C) 2003, 2012, 2013 Mark Adler. zlib License (see the header of `blast.c`).

## zlib

MKSetup.exe is statically linked with zlib (used by unshield). Copyright (C) 1995-2024 Jean-loup Gailly and Mark Adler. zlib License.
