#include "png.h"
#include "util.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <zlib.h>

static void put_u32(unsigned char *p, uint32_t v) {
    p[0] = (unsigned char)(v >> 24);
    p[1] = (unsigned char)(v >> 16);
    p[2] = (unsigned char)(v >> 8);
    p[3] = (unsigned char)v;
}

static int write_chunk(FILE *f, const char type[4], const unsigned char *data, size_t len) {
    unsigned char hdr[8];
    put_u32(hdr, (uint32_t)len);
    memcpy(hdr + 4, type, 4);
    if (fwrite(hdr, 1, sizeof(hdr), f) != sizeof(hdr)) return -1;
    if (len && fwrite(data, 1, len, f) != len) return -1;
    uLong crc = crc32(0L, Z_NULL, 0);
    crc = crc32(crc, (const Bytef *)type, 4);
    if (len) crc = crc32(crc, data, (uInt)len);
    unsigned char crcbe[4];
    put_u32(crcbe, (uint32_t)crc);
    if (fwrite(crcbe, 1, 4, f) != 4) return -1;
    return 0;
}

int png_write_rgb(const char *path, const uint8_t *rgb, unsigned width, unsigned height) {
    if (!path || !rgb || width == 0 || height == 0) return -1;

    size_t stride = (size_t)width * 3;
    size_t rawlen = (stride + 1) * (size_t)height;
    unsigned char *raw = xmalloc(rawlen);
    for (unsigned y = 0; y < height; y++) {
        unsigned char *dst = raw + (stride + 1) * y;
        dst[0] = 0; /* filter: none */
        memcpy(dst + 1, rgb + stride * y, stride);
    }

    uLongf zlen = compressBound((uLong)rawlen);
    unsigned char *z = xmalloc(zlen);
    if (compress2(z, &zlen, raw, (uLong)rawlen, 6) != Z_OK) {
        free(raw);
        free(z);
        return -1;
    }
    free(raw);

    FILE *f = fopen(path, "wb");
    if (!f) {
        free(z);
        return -1;
    }
    static const unsigned char sig[8] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n'};
    int rc = 0;
    if (fwrite(sig, 1, sizeof(sig), f) != sizeof(sig)) rc = -1;

    unsigned char ihdr[13];
    put_u32(ihdr, width);
    put_u32(ihdr + 4, height);
    ihdr[8] = 8;  /* bit depth */
    ihdr[9] = 2;  /* colour type: truecolour */
    ihdr[10] = 0; /* compression */
    ihdr[11] = 0; /* filter */
    ihdr[12] = 0; /* interlace */
    if (!rc && write_chunk(f, "IHDR", ihdr, sizeof(ihdr)) != 0) rc = -1;
    if (!rc && write_chunk(f, "IDAT", z, (size_t)zlen) != 0) rc = -1;
    if (!rc && write_chunk(f, "IEND", NULL, 0) != 0) rc = -1;

    free(z);
    if (fclose(f) != 0) rc = -1;
    if (rc != 0) unlink(path);
    return rc;
}
