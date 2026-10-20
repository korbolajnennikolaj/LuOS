// SPDX-License-Identifier: MIT
// Copyright (c) 2026 korbolajnennikolaj


#include "components/fwzip.h"

#include "components/logger.h"
#include "components/Memory/heap.h"

#include <stdio.h>
#include <string.h>


static uint16_t rd16(const uint8_t *p) {
    uint16_t v;
    memcpy(&v, p, sizeof(v));
    return v;
}

static uint32_t rd32(const uint8_t *p) {
    uint32_t v;
    memcpy(&v, p, sizeof(v));
    return v;
}

static uint64_t rd64(const uint8_t *p) {
    uint64_t v;
    memcpy(&v, p, sizeof(v));
    return v;
}


static const uint32_t fwzip_crc_table[256] = {
    0x00000000u, 0x77073096u, 0xee0e612cu, 0x990951bau, 0x076dc419u, 0x706af48fu, 0xe963a535u, 0x9e6495a3u,
    0x0edb8832u, 0x79dcb8a4u, 0xe0d5e91eu, 0x97d2d988u, 0x09b64c2bu, 0x7eb17cbdu, 0xe7b82d07u, 0x90bf1d91u,
    0x1db71064u, 0x6ab020f2u, 0xf3b97148u, 0x84be41deu, 0x1adad47du, 0x6ddde4ebu, 0xf4d4b551u, 0x83d385c7u,
    0x136c9856u, 0x646ba8c0u, 0xfd62f97au, 0x8a65c9ecu, 0x14015c4fu, 0x63066cd9u, 0xfa0f3d63u, 0x8d080df5u,
    0x3b6e20c8u, 0x4c69105eu, 0xd56041e4u, 0xa2677172u, 0x3c03e4d1u, 0x4b04d447u, 0xd20d85fdu, 0xa50ab56bu,
    0x35b5a8fau, 0x42b2986cu, 0xdbbbc9d6u, 0xacbcf940u, 0x32d86ce3u, 0x45df5c75u, 0xdcd60dcfu, 0xabd13d59u,
    0x26d930acu, 0x51de003au, 0xc8d75180u, 0xbfd06116u, 0x21b4f4b5u, 0x56b3c423u, 0xcfba9599u, 0xb8bda50fu,
    0x2802b89eu, 0x5f058808u, 0xc60cd9b2u, 0xb10be924u, 0x2f6f7c87u, 0x58684c11u, 0xc1611dabu, 0xb6662d3du,
    0x76dc4190u, 0x01db7106u, 0x98d220bcu, 0xefd5102au, 0x71b18589u, 0x06b6b51fu, 0x9fbfe4a5u, 0xe8b8d433u,
    0x7807c9a2u, 0x0f00f934u, 0x9609a88eu, 0xe10e9818u, 0x7f6a0dbbu, 0x086d3d2du, 0x91646c97u, 0xe6635c01u,
    0x6b6b51f4u, 0x1c6c6162u, 0x856530d8u, 0xf262004eu, 0x6c0695edu, 0x1b01a57bu, 0x8208f4c1u, 0xf50fc457u,
    0x65b0d9c6u, 0x12b7e950u, 0x8bbeb8eau, 0xfcb9887cu, 0x62dd1ddfu, 0x15da2d49u, 0x8cd37cf3u, 0xfbd44c65u,
    0x4db26158u, 0x3ab551ceu, 0xa3bc0074u, 0xd4bb30e2u, 0x4adfa541u, 0x3dd895d7u, 0xa4d1c46du, 0xd3d6f4fbu,
    0x4369e96au, 0x346ed9fcu, 0xad678846u, 0xda60b8d0u, 0x44042d73u, 0x33031de5u, 0xaa0a4c5fu, 0xdd0d7cc9u,
    0x5005713cu, 0x270241aau, 0xbe0b1010u, 0xc90c2086u, 0x5768b525u, 0x206f85b3u, 0xb966d409u, 0xce61e49fu,
    0x5edef90eu, 0x29d9c998u, 0xb0d09822u, 0xc7d7a8b4u, 0x59b33d17u, 0x2eb40d81u, 0xb7bd5c3bu, 0xc0ba6cadu,
    0xedb88320u, 0x9abfb3b6u, 0x03b6e20cu, 0x74b1d29au, 0xead54739u, 0x9dd277afu, 0x04db2615u, 0x73dc1683u,
    0xe3630b12u, 0x94643b84u, 0x0d6d6a3eu, 0x7a6a5aa8u, 0xe40ecf0bu, 0x9309ff9du, 0x0a00ae27u, 0x7d079eb1u,
    0xf00f9344u, 0x8708a3d2u, 0x1e01f268u, 0x6906c2feu, 0xf762575du, 0x806567cbu, 0x196c3671u, 0x6e6b06e7u,
    0xfed41b76u, 0x89d32be0u, 0x10da7a5au, 0x67dd4accu, 0xf9b9df6fu, 0x8ebeeff9u, 0x17b7be43u, 0x60b08ed5u,
    0xd6d6a3e8u, 0xa1d1937eu, 0x38d8c2c4u, 0x4fdff252u, 0xd1bb67f1u, 0xa6bc5767u, 0x3fb506ddu, 0x48b2364bu,
    0xd80d2bdau, 0xaf0a1b4cu, 0x36034af6u, 0x41047a60u, 0xdf60efc3u, 0xa867df55u, 0x316e8eefu, 0x4669be79u,
    0xcb61b38cu, 0xbc66831au, 0x256fd2a0u, 0x5268e236u, 0xcc0c7795u, 0xbb0b4703u, 0x220216b9u, 0x5505262fu,
    0xc5ba3bbeu, 0xb2bd0b28u, 0x2bb45a92u, 0x5cb36a04u, 0xc2d7ffa7u, 0xb5d0cf31u, 0x2cd99e8bu, 0x5bdeae1du,
    0x9b64c2b0u, 0xec63f226u, 0x756aa39cu, 0x026d930au, 0x9c0906a9u, 0xeb0e363fu, 0x72076785u, 0x05005713u,
    0x95bf4a82u, 0xe2b87a14u, 0x7bb12baeu, 0x0cb61b38u, 0x92d28e9bu, 0xe5d5be0du, 0x7cdcefb7u, 0x0bdbdf21u,
    0x86d3d2d4u, 0xf1d4e242u, 0x68ddb3f8u, 0x1fda836eu, 0x81be16cdu, 0xf6b9265bu, 0x6fb077e1u, 0x18b74777u,
    0x88085ae6u, 0xff0f6a70u, 0x66063bcau, 0x11010b5cu, 0x8f659effu, 0xf862ae69u, 0x616bffd3u, 0x166ccf45u,
    0xa00ae278u, 0xd70dd2eeu, 0x4e048354u, 0x3903b3c2u, 0xa7672661u, 0xd06016f7u, 0x4969474du, 0x3e6e77dbu,
    0xaed16a4au, 0xd9d65adcu, 0x40df0b66u, 0x37d83bf0u, 0xa9bcae53u, 0xdebb9ec5u, 0x47b2cf7fu, 0x30b5ffe9u,
    0xbdbdf21cu, 0xcabac28au, 0x53b39330u, 0x24b4a3a6u, 0xbad03605u, 0xcdd70693u, 0x54de5729u, 0x23d967bfu,
    0xb3667a2eu, 0xc4614ab8u, 0x5d681b02u, 0x2a6f2b94u, 0xb40bbe37u, 0xc30c8ea1u, 0x5a05df1bu, 0x2d02ef8du,
};

uint32_t fwzip_crc32(uint32_t crc, const uint8_t *p, size_t len) {
    crc ^= 0xFFFFFFFFu;
    while (len--) crc = fwzip_crc_table[(crc ^ *p++) & 0xff] ^ (crc >> 8);
    return crc ^ 0xFFFFFFFFu;
}


#define INFL_MAX_BITS 15
#define INFL_MAX_SYMS 288

struct infl_state {
    const uint8_t *in;
    size_t in_len;
    size_t in_pos;
    uint32_t bitbuf;
    int bitcnt;
    uint8_t *out;
    size_t out_cap;
    size_t out_pos;
    int bad;
};

struct infl_huff {
    uint16_t counts[INFL_MAX_BITS + 1];
    uint16_t symbols[INFL_MAX_SYMS];
};

static int infl_bit(struct infl_state *s, uint32_t *out) {
    if (s->bitcnt == 0) {
        if (s->in_pos >= s->in_len) return FWZIP_ERR_CORRUPT;
        s->bitbuf = s->in[s->in_pos++];
        s->bitcnt = 8;
    }
    *out = s->bitbuf & 1;
    s->bitbuf >>= 1;
    s->bitcnt--;
    return FWZIP_OK;
}

static int infl_bits(struct infl_state *s, int need, uint32_t *out) {
    uint32_t val = 0;
    for (int i = 0; i < need; i++) {
        uint32_t b;
        if (infl_bit(s, &b)) return FWZIP_ERR_CORRUPT;
        val |= b << i;
    }
    *out = val;
    return FWZIP_OK;
}

static int infl_build(struct infl_huff *h, const uint8_t *lengths, int n) {
    memset(h->counts, 0, sizeof(h->counts));
    for (int i = 0; i < n; i++) h->counts[lengths[i]]++;
    if (h->counts[0] == n) return FWZIP_OK;

    int left = 1;
    for (int len = 1; len <= INFL_MAX_BITS; len++) {
        left <<= 1;
        left -= h->counts[len];
        if (left < 0) return FWZIP_ERR_CORRUPT;
    }

    uint16_t offs[INFL_MAX_BITS + 1];
    offs[1] = 0;
    for (int len = 1; len < INFL_MAX_BITS; len++) offs[len + 1] = offs[len] + h->counts[len];
    for (int i = 0; i < n; i++)
        if (lengths[i]) h->symbols[offs[lengths[i]]++] = (uint16_t)i;
    return FWZIP_OK;
}

static int infl_decode(struct infl_state *s, const struct infl_huff *h, uint16_t *out) {
    int code = 0, first = 0, index = 0;
    for (int len = 1; len <= INFL_MAX_BITS; len++) {
        uint32_t b;
        if (infl_bit(s, &b)) return FWZIP_ERR_CORRUPT;
        code |= (int)b;
        int count = h->counts[len];
        if (code - first < count) {
            *out = h->symbols[index + (code - first)];
            return FWZIP_OK;
        }
        index += count;
        first = (first + count) << 1;
        code <<= 1;
    }
    return FWZIP_ERR_CORRUPT;
}

static int infl_emit(struct infl_state *s, uint8_t byte) {
    if (s->out_pos >= s->out_cap) return FWZIP_ERR_CORRUPT;
    s->out[s->out_pos++] = byte;
    return FWZIP_OK;
}

static int infl_copy(struct infl_state *s, uint32_t dist, uint32_t len) {
    if (dist == 0 || dist > s->out_pos) return FWZIP_ERR_CORRUPT;
    size_t from = s->out_pos - dist;
    for (uint32_t i = 0; i < len; i++) {
        if (s->out_pos >= s->out_cap) return FWZIP_ERR_CORRUPT;
        s->out[s->out_pos++] = s->out[from++];
    }
    return FWZIP_OK;
}

static const uint16_t infl_len_base[29] = {
    3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258
};
static const uint8_t infl_len_extra[29] = {
    0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0
};
static const uint16_t infl_dist_base[30] = {
    1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193, 257, 385, 513, 769, 1025,
    1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577
};
static const uint8_t infl_dist_extra[30] = {
    0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13
};

static int infl_block(struct infl_state *s, const struct infl_huff *lit, const struct infl_huff *dist) {
    for (;;) {
        uint16_t sym;
        if (infl_decode(s, lit, &sym)) return FWZIP_ERR_CORRUPT;
        if (sym < 256) {
            if (infl_emit(s, (uint8_t)sym)) return FWZIP_ERR_CORRUPT;
        } else if (sym == 256) {
            return FWZIP_OK;
        } else {
            if (sym > 285) return FWZIP_ERR_CORRUPT;
            sym -= 257;
            uint32_t extra;
            if (infl_bits(s, infl_len_extra[sym], &extra)) return FWZIP_ERR_CORRUPT;
            uint32_t len = infl_len_base[sym] + extra;

            uint16_t dsym;
            if (infl_decode(s, dist, &dsym)) return FWZIP_ERR_CORRUPT;
            if (dsym > 29) return FWZIP_ERR_CORRUPT;
            if (infl_bits(s, infl_dist_extra[dsym], &extra)) return FWZIP_ERR_CORRUPT;
            uint32_t d = infl_dist_base[dsym] + extra;
            if (infl_copy(s, d, len)) return FWZIP_ERR_CORRUPT;
        }
    }
}

static int infl_stored(struct infl_state *s) {
    s->bitbuf = 0;
    s->bitcnt = 0;
    if (s->in_pos + 4 > s->in_len) return FWZIP_ERR_CORRUPT;
    uint16_t len = rd16(s->in + s->in_pos);
    uint16_t nlen = rd16(s->in + s->in_pos + 2);
    s->in_pos += 4;
    if ((uint16_t)~nlen != len) return FWZIP_ERR_CORRUPT;
    if (s->in_pos + len > s->in_len) return FWZIP_ERR_CORRUPT;
    if (s->out_pos + len > s->out_cap) return FWZIP_ERR_CORRUPT;
    memcpy(s->out + s->out_pos, s->in + s->in_pos, len);
    s->out_pos += len;
    s->in_pos += len;
    return FWZIP_OK;
}

static const uint8_t infl_clorder[19] = { 16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15 };

static int infl_dynamic(struct infl_state *s, struct infl_huff *lit, struct infl_huff *dist) {
    uint8_t lengths[INFL_MAX_SYMS + 32];
    uint32_t hlit, hdist, hclen;

    if (infl_bits(s, 5, &hlit) || infl_bits(s, 5, &hdist) || infl_bits(s, 4, &hclen)) return FWZIP_ERR_CORRUPT;
    hlit += 257;
    hdist += 1;
    hclen += 4;
    if (hlit > 286 || hdist > 30) return FWZIP_ERR_CORRUPT;

    uint8_t cl_lengths[19];
    memset(cl_lengths, 0, sizeof(cl_lengths));
    for (uint32_t i = 0; i < hclen; i++) {
        uint32_t v;
        if (infl_bits(s, 3, &v)) return FWZIP_ERR_CORRUPT;
        cl_lengths[infl_clorder[i]] = (uint8_t)v;
    }
    struct infl_huff clh;
    if (infl_build(&clh, cl_lengths, 19)) return FWZIP_ERR_CORRUPT;

    uint32_t n = 0;
    memset(lengths, 0, hlit + hdist);
    while (n < hlit + hdist) {
        uint16_t sym;
        if (infl_decode(s, &clh, &sym)) return FWZIP_ERR_CORRUPT;
        if (sym < 16) {
            lengths[n++] = (uint8_t)sym;
        } else {
            uint8_t value = 0;
            uint32_t rep = 0;
            if (sym == 16) {
                if (n == 0) return FWZIP_ERR_CORRUPT;
                value = lengths[n - 1];
                if (infl_bits(s, 2, &rep)) return FWZIP_ERR_CORRUPT;
                rep += 3;
            } else if (sym == 17) {
                if (infl_bits(s, 3, &rep)) return FWZIP_ERR_CORRUPT;
                rep += 3;
            } else {
                if (infl_bits(s, 7, &rep)) return FWZIP_ERR_CORRUPT;
                rep += 11;
            }
            if (n + rep > hlit + hdist) return FWZIP_ERR_CORRUPT;
            while (rep--) lengths[n++] = value;
        }
    }
    if (lengths[256] == 0) return FWZIP_ERR_CORRUPT;

    if (infl_build(lit, lengths, (int)hlit)) return FWZIP_ERR_CORRUPT;
    if (infl_build(dist, lengths + hlit, (int)hdist)) return FWZIP_ERR_CORRUPT;
    return infl_block(s, lit, dist);
}

static int infl_fixed(struct infl_state *s, struct infl_huff *lit, struct infl_huff *dist) {
    uint8_t lengths[288];
    int i = 0;
    for (; i < 144; i++) lengths[i] = 8;
    for (; i < 256; i++) lengths[i] = 9;
    for (; i < 280; i++) lengths[i] = 7;
    for (; i < 288; i++) lengths[i] = 8;
    if (infl_build(lit, lengths, 288)) return FWZIP_ERR_CORRUPT;

    uint8_t dlengths[30];
    memset(dlengths, 5, sizeof(dlengths));
    if (infl_build(dist, dlengths, 30)) return FWZIP_ERR_CORRUPT;
    return infl_block(s, lit, dist);
}

int fwzip_inflate(const uint8_t *in, size_t in_len, uint8_t *out, size_t out_cap, size_t *out_len) {
    if (!in || !out || !out_len) return FWZIP_ERR_PARAM;
    struct infl_state s = {
        .in = in, .in_len = in_len, .in_pos = 0, .bitbuf = 0, .bitcnt = 0,
        .out = out, .out_cap = out_cap, .out_pos = 0, .bad = 0,
    };
    struct infl_huff lit, dist;

    for (;;) {
        uint32_t final, type;
        if (infl_bit(&s, &final) || infl_bits(&s, 2, &type)) return FWZIP_ERR_CORRUPT;
        int r;
        if (type == 0) r = infl_stored(&s);
        else if (type == 1) r = infl_fixed(&s, &lit, &dist);
        else if (type == 2) r = infl_dynamic(&s, &lit, &dist);
        else return FWZIP_ERR_CORRUPT;
        if (r) return r;
        if (final) break;
    }
    *out_len = s.out_pos;
    return FWZIP_OK;
}


static int zread_exact(fs_file_t *f, void *buf, uint64_t len) {
    uint8_t *p = buf;
    uint64_t done = 0;
    while (done < len) {
        int64_t r = fs_read(f, p + done, len - done);
        if (r <= 0) return FWZIP_ERR_IO;
        done += (uint64_t)r;
    }
    return FWZIP_OK;
}

static int zfind_eocd(fs_t *fs, const char *rel, uint64_t file_size, uint64_t *cd_offset, uint32_t *cd_count,
                      uint64_t *cd_size, bool *zip64) {
    uint64_t tail_len = file_size < 65536 + 22 + 512 ? file_size : 65536 + 22 + 512;
    uint8_t *tail = kmalloc((size_t)tail_len);
    if (!tail) return FWZIP_ERR_NOMEM;

    fs_file_t f;
    if (fs_open(fs, rel, false, false, &f) != FS_OK) {
        kfree(tail);
        return FWZIP_ERR_IO;
    }
    if (fs_seek(&f, file_size - tail_len) != FS_OK || zread_exact(&f, tail, tail_len) != FWZIP_OK) {
        fs_close(&f);
        kfree(tail);
        return FWZIP_ERR_IO;
    }
    fs_close(&f);

    uint64_t eocd = 0;
    bool found = false;
    for (uint64_t i = tail_len; i >= 22; i--) {
        uint64_t off = i - 22;
        if (rd32(tail + off) == 0x06054b50u) {
            eocd = off;
            found = true;
            break;
        }
    }
    if (!found) {
        kfree(tail);
        return FWZIP_ERR_CORRUPT;
    }

    uint16_t disk = rd16(tail + eocd + 4);
    uint16_t cd_disk = rd16(tail + eocd + 6);
    uint16_t entries_disk = rd16(tail + eocd + 8);
    uint16_t entries_total = rd16(tail + eocd + 10);
    uint32_t size32 = rd32(tail + eocd + 12);
    uint32_t offset32 = rd32(tail + eocd + 16);

    if (disk != 0 || cd_disk != 0) {
        kfree(tail);
        LOG_WARNING("fwzip: multi-disk archives are not supported");
        return FWZIP_ERR_UNSUPP;
    }

    *zip64 = false;
    *cd_count = entries_total;
    *cd_size = size32;
    *cd_offset = offset32;

    if (entries_total == 0xFFFF || size32 == 0xFFFFFFFFu || offset32 == 0xFFFFFFFFu) {
        *zip64 = true;
        if (eocd >= 20 && rd32(tail + eocd - 20) == 0x07064b50u) {
            uint64_t z64_eocd = rd64(tail + eocd - 20 + 8);
            uint8_t hdr[56];
            fs_file_t zf;
            bool opened = fs_open(fs, rel, false, false, &zf) == FS_OK;
            bool got = opened && fs_seek(&zf, z64_eocd) == FS_OK && zread_exact(&zf, hdr, sizeof(hdr)) == FWZIP_OK;
            if (opened) fs_close(&zf);
            if (got && rd32(hdr) == 0x06064b50u) {
                uint64_t total = rd64(hdr + 32);
                uint64_t zsize = rd64(hdr + 40);
                uint64_t zoffset = rd64(hdr + 48);
                kfree(tail);
                if (total > FWZIP_MAX_ENTRIES) {
                    LOG_WARNING("fwzip: archive has %llu entries, more than the %d supported",
                                (unsigned long long)total, FWZIP_MAX_ENTRIES);
                    return FWZIP_ERR_UNSUPP;
                }
                *cd_count = (uint32_t)total;
                *cd_size = zsize;
                *cd_offset = zoffset;
                return FWZIP_OK;
            }
        }
        kfree(tail);
        LOG_WARNING("fwzip: Zip64 end of central directory not found");
        return FWZIP_ERR_UNSUPP;
    }

    kfree(tail);
    if (entries_total != entries_disk) return FWZIP_ERR_CORRUPT;
    if (*cd_count > FWZIP_MAX_ENTRIES) {
        LOG_WARNING("fwzip: archive has %u entries, more than the %d supported", *cd_count, FWZIP_MAX_ENTRIES);
        return FWZIP_ERR_UNSUPP;
    }
    return FWZIP_OK;
}

static bool zindex_name(const uint8_t *src, size_t name_len, char *out, size_t cap) {
    size_t n = name_len;
    while (n > 0 && (src[n - 1] == '\n' || src[n - 1] == '\r')) n--;
    if (n == 0 || n >= cap) return false;
    if (src[n - 1] == '/') return false;

    size_t skip = 0;
    static const char prefixes[][15] = { "lib/firmware/", "firmware/" };
    for (size_t i = 0; i < sizeof(prefixes) / sizeof(prefixes[0]); i++) {
        size_t pl = strlen(prefixes[i]);
        if (n > pl && memcmp(src, prefixes[i], pl) == 0) {
            skip = pl;
            break;
        }
    }
    const uint8_t *p = src + skip;
    n -= skip;
    if (n == 0 || n >= cap) return false;

    for (size_t i = 0; i < n; i++) {
        char c = (char)p[i];
        if (c == '\\') c = '/';
        if (c == '.' && i + 2 < n && p[i + 1] == '.' && p[i + 2] == '/') return false;
        out[i] = c;
    }
    out[n] = '\0';
    if (out[0] == '/' || strstr(out, "//")) return false;
    return true;
}

static struct fwzip *zindex_alloc(uint32_t cd_count);
static int zindex_parse_cd(const uint8_t *cd, uint64_t cd_size, uint32_t cd_count, struct fwzip *z,
                           const char *label);

int fwzip_open(fs_t *fs, const char *rel, struct fwzip **out) {
    if (!fs || !rel || !out) return FWZIP_ERR_PARAM;
    *out = NULL;

    fs_file_t f;
    int open_r = fs_open(fs, rel, false, false, &f);
    if (open_r != FS_OK) return open_r == FS_ERR_NOENT ? FWZIP_ERR_NOENT : FWZIP_ERR_IO;
    uint64_t file_size = f.size;
    uint8_t magic[4];
    if (zread_exact(&f, magic, 4) != FWZIP_OK) {
        fs_close(&f);
        return FWZIP_ERR_IO;
    }
    fs_close(&f);
    if (memcmp(magic, "PK", 2) != 0) return FWZIP_ERR_CORRUPT;

    uint64_t cd_offset, cd_size;
    uint32_t cd_count;
    bool zip64 = false;
    int r = zfind_eocd(fs, rel, file_size, &cd_offset, &cd_count, &cd_size, &zip64);
    if (r != FWZIP_OK) return r;
    if (cd_size > (64u << 20)) {
        LOG_WARNING("fwzip: central directory of %llu bytes is implausible", (unsigned long long)cd_size);
        return FWZIP_ERR_CORRUPT;
    }
    if (cd_offset + cd_size > file_size) return FWZIP_ERR_CORRUPT;

    uint8_t *cd = kmalloc((size_t)cd_size ? (size_t)cd_size : 1);
    if (!cd) return FWZIP_ERR_NOMEM;
    if (fs_open(fs, rel, false, false, &f) != FS_OK) {
        kfree(cd);
        return FWZIP_ERR_IO;
    }
    if (fs_seek(&f, cd_offset) != FS_OK || zread_exact(&f, cd, cd_size) != FWZIP_OK) {
        fs_close(&f);
        kfree(cd);
        return FWZIP_ERR_IO;
    }
    fs_close(&f);

    struct fwzip *z = zindex_alloc(cd_count);
    if (!z) {
        kfree(cd);
        return FWZIP_ERR_NOMEM;
    }
    int pr = zindex_parse_cd(cd, cd_size, cd_count, z, rel);
    kfree(cd);
    if (pr != FWZIP_OK) {
        fwzip_close(z);
        return pr;
    }

    *out = z;
    return FWZIP_OK;
}


static struct fwzip *zindex_alloc(uint32_t cd_count) {
    struct fwzip *z = kmalloc(sizeof(*z));
    if (!z) return NULL;
    z->entries = kmalloc((size_t)cd_count * sizeof(struct fwzip_entry));
    if (!z->entries) {
        kfree(z);
        return NULL;
    }
    memset(z->entries, 0, (size_t)cd_count * sizeof(struct fwzip_entry));
    z->count = 0;
    return z;
}

static int zindex_parse_cd(const uint8_t *cd, uint64_t cd_size, uint32_t cd_count, struct fwzip *z,
                           const char *label) {
    uint64_t pos = 0;
    uint32_t unsupported = 0, skipped = 0;
    for (uint32_t i = 0; i < cd_count; i++) {
        if (pos + 46 > cd_size) break;
        const uint8_t *e = cd + pos;
        if (rd32(e) != 0x02014b50u) return FWZIP_ERR_CORRUPT;
        uint16_t method = rd16(e + 10);
        uint16_t flags = rd16(e + 8);
        uint16_t disk_start = rd16(e + 34);
        uint32_t crc = rd32(e + 16);
        uint32_t csize = rd32(e + 20);
        uint32_t usize = rd32(e + 24);
        uint16_t name_len = rd16(e + 28);
        uint16_t extra_len = rd16(e + 30);
        uint16_t comment_len = rd16(e + 32);
        uint64_t header_offset = rd32(e + 42);
        if (pos + 46 + name_len + extra_len + comment_len > cd_size) return FWZIP_ERR_CORRUPT;
        const uint8_t *name = e + 46;
        const uint8_t *extra = name + name_len;
        pos += 46 + name_len + extra_len + comment_len;

        if (method != 0 && method != 8) {
            unsupported++;
            continue;
        }
        if (flags & 1) {
            unsupported++;
            continue;
        }
        if (disk_start != 0) {
            unsupported++;
            continue;
        }

        if (csize == 0xFFFFFFFFu || usize == 0xFFFFFFFFu || header_offset == 0xFFFFFFFFu) {
            bool ok = false;
            uint64_t p = 0;
            while (p + 4 <= extra_len) {
                uint16_t id = rd16(extra + p);
                uint16_t sz = rd16(extra + p + 2);
                if (p + 4 + sz > extra_len) break;
                if (id == 0x0001) {
                    uint64_t q = p + 4;
                    if (usize == 0xFFFFFFFFu && q + 8 <= p + 4 + sz) {
                        uint64_t v = rd64(extra + q);
                        if (v > 0xFFFFFFFFull) {
                            unsupported++;
                            ok = true;
                            break;
                        }
                        usize = (uint32_t)v;
                        q += 8;
                    }
                    if (csize == 0xFFFFFFFFu && q + 8 <= p + 4 + sz) {
                        uint64_t v = rd64(extra + q);
                        if (v > 0xFFFFFFFFull) {
                            unsupported++;
                            ok = true;
                            break;
                        }
                        csize = (uint32_t)v;
                        q += 8;
                    }
                    if (header_offset == 0xFFFFFFFFu && q + 8 <= p + 4 + sz) {
                        uint64_t v = rd64(extra + q);
                        if (v > 0xFFFFFFFFull) {
                            unsupported++;
                            ok = true;
                            break;
                        }
                        header_offset = (uint32_t)v;
                    }
                    ok = true;
                }
                p += 4 + (uint64_t)sz;
            }
            if (!ok) {
                unsupported++;
                continue;
            }
        }

        char key[FWZIP_NAME_MAX];
        if (!zindex_name(name, name_len, key, sizeof(key))) {
            skipped++;
            continue;
        }

        struct fwzip_entry slot;
        memset(&slot, 0, sizeof(slot));
        size_t key_len = strlen(key);
        memcpy(slot.name, key, key_len);
        slot.header_offset = header_offset;
        slot.comp_size = csize;
        slot.file_size = usize;
        slot.crc32 = crc;
        slot.method = method;

        bool replaced = false;
        for (uint32_t k = 0; k < z->count; k++) {
            if (strcmp(z->entries[k].name, key) == 0) {
                z->entries[k] = slot;
                replaced = true;
                break;
            }
        }
        if (!replaced) z->entries[z->count++] = slot;
    }
    if (z->count == 0)
        LOG_WARNING("fwzip: '%s' has no usable firmware entries (%u unsupported, %u skipped)", label, unsupported,
                    skipped);
    else if (unsupported)
        LOG_INFO("fwzip: '%s': %d entr%s indexed (%d unsupported, %d skipped)", label, z->count,
                 z->count == 1 ? "y" : "ies", unsupported, skipped);
    else
        LOG_INFO("fwzip: '%s': %d entr%s indexed", label, z->count, z->count == 1 ? "y" : "ies");
    return FWZIP_OK;
}


static int zfind_eocd_mem(const uint8_t *mem, uint64_t mem_len, uint64_t *cd_offset, uint32_t *cd_count,
                          uint64_t *cd_size, bool *zip64) {
    uint64_t tail_len = mem_len < 65536 + 22 + 512 ? mem_len : 65536 + 22 + 512;
    const uint8_t *tail = mem + mem_len - tail_len;

    uint64_t eocd = 0;
    bool found = false;
    for (uint64_t i = tail_len; i >= 22; i--) {
        uint64_t off = i - 22;
        if (rd32(tail + off) == 0x06054b50u) {
            eocd = off;
            found = true;
            break;
        }
    }
    if (!found) return FWZIP_ERR_CORRUPT;

    uint16_t disk = rd16(tail + eocd + 4);
    uint16_t cd_disk = rd16(tail + eocd + 6);
    uint16_t entries_disk = rd16(tail + eocd + 8);
    uint16_t entries_total = rd16(tail + eocd + 10);
    uint32_t size32 = rd32(tail + eocd + 12);
    uint32_t offset32 = rd32(tail + eocd + 16);

    if (disk != 0 || cd_disk != 0) {
        LOG_WARNING("fwzip: multi-disk archives are not supported");
        return FWZIP_ERR_UNSUPP;
    }

    *zip64 = false;
    *cd_count = entries_total;
    *cd_size = size32;
    *cd_offset = offset32;

    if (entries_total == 0xFFFF || size32 == 0xFFFFFFFFu || offset32 == 0xFFFFFFFFu) {
        *zip64 = true;
        if (eocd >= 20 && rd32(tail + eocd - 20) == 0x07064b50u) {
            uint64_t z64_eocd = rd64(tail + eocd - 20 + 8);
            if (z64_eocd + 56 <= mem_len && rd32(mem + z64_eocd) == 0x06064b50u) {
                const uint8_t *hdr = mem + z64_eocd;
                uint64_t total = rd64(hdr + 32);
                uint64_t zsize = rd64(hdr + 40);
                uint64_t zoffset = rd64(hdr + 48);
                if (total > FWZIP_MAX_ENTRIES) {
                    LOG_WARNING("fwzip: archive has %llu entries, more than the %d supported",
                                (unsigned long long)total, FWZIP_MAX_ENTRIES);
                    return FWZIP_ERR_UNSUPP;
                }
                *cd_count = (uint32_t)total;
                *cd_size = zsize;
                *cd_offset = zoffset;
                return FWZIP_OK;
            }
        }
        LOG_WARNING("fwzip: Zip64 end of central directory not found");
        return FWZIP_ERR_UNSUPP;
    }

    if (entries_total != entries_disk) return FWZIP_ERR_CORRUPT;
    if (*cd_count > FWZIP_MAX_ENTRIES) {
        LOG_WARNING("fwzip: archive has %u entries, more than the %d supported", *cd_count, FWZIP_MAX_ENTRIES);
        return FWZIP_ERR_UNSUPP;
    }
    return FWZIP_OK;
}

int fwzip_open_mem(const uint8_t *mem, uint64_t mem_len, const char *label, struct fwzip **out) {
    if (!mem || !out) return FWZIP_ERR_PARAM;
    *out = NULL;
    if (mem_len < 2 || memcmp(mem, "PK", 2) != 0) return FWZIP_ERR_CORRUPT;

    uint64_t cd_offset, cd_size;
    uint32_t cd_count;
    bool zip64 = false;
    int r = zfind_eocd_mem(mem, mem_len, &cd_offset, &cd_count, &cd_size, &zip64);
    if (r != FWZIP_OK) return r;
    if (cd_size > (64u << 20)) {
        LOG_WARNING("fwzip: central directory of %llu bytes is implausible", (unsigned long long)cd_size);
        return FWZIP_ERR_CORRUPT;
    }
    if (cd_offset + cd_size > mem_len) return FWZIP_ERR_CORRUPT;

    struct fwzip *z = zindex_alloc(cd_count);
    if (!z) return FWZIP_ERR_NOMEM;

    r = zindex_parse_cd(mem + cd_offset, cd_size, cd_count, z, label);
    if (r != FWZIP_OK) {
        fwzip_close(z);
        return r;
    }

    *out = z;
    return FWZIP_OK;
}

void fwzip_close(struct fwzip *z) {
    if (!z) return;
    if (z->entries) kfree(z->entries);
    kfree(z);
}

const struct fwzip_entry *fwzip_find(const struct fwzip *z, const char *name) {
    if (!z || !name) return NULL;
    for (uint32_t i = 0; i < z->count; i++)
        if (strcmp(z->entries[i].name, name) == 0) return &z->entries[i];
    return NULL;
}

int fwzip_read(fs_t *fs, const char *rel, const struct fwzip *z, const struct fwzip_entry *e, uint8_t **data,
               size_t *size, char *out_desc, size_t desc_cap) {
    if (!fs || !rel || !z || !e || !data || !size) return FWZIP_ERR_PARAM;
    *data = NULL;
    *size = 0;

    fs_file_t f;
    if (fs_open(fs, rel, false, false, &f) != FS_OK) return FWZIP_ERR_IO;

    uint8_t lfh[30];
    if (fs_seek(&f, e->header_offset) != FS_OK || zread_exact(&f, lfh, sizeof(lfh)) != FWZIP_OK) {
        fs_close(&f);
        return FWZIP_ERR_IO;
    }
    if (rd32(lfh) != 0x04034b50u) {
        fs_close(&f);
        return FWZIP_ERR_CORRUPT;
    }
    uint16_t l_method = rd16(lfh + 8);
    uint16_t l_flags = rd16(lfh + 6);
    uint16_t name_len = rd16(lfh + 26);
    uint16_t extra_len = rd16(lfh + 28);
    if (l_method != e->method || (l_flags & 1)) {
        fs_close(&f);
        return FWZIP_ERR_CORRUPT;
    }
    uint64_t data_off = e->header_offset + 30 + name_len + extra_len;

    uint8_t *comp = kmalloc(e->comp_size ? e->comp_size : 1);
    if (!comp) {
        fs_close(&f);
        return FWZIP_ERR_NOMEM;
    }
    if (fs_seek(&f, data_off) != FS_OK || zread_exact(&f, comp, e->comp_size) != FWZIP_OK) {
        fs_close(&f);
        kfree(comp);
        return FWZIP_ERR_IO;
    }
    fs_close(&f);

    uint8_t *out = NULL;
    size_t out_len = 0;
    int r = FWZIP_OK;
    if (e->method == 0) {
        if (e->comp_size != e->file_size) r = FWZIP_ERR_CORRUPT;
        else {
            out = comp;
            comp = NULL;
            out_len = e->file_size;
            if (out_desc) snprintf(out_desc, desc_cap, "stored, %u bytes", (unsigned)e->file_size);
        }
    } else {
        out = kmalloc(e->file_size ? e->file_size : 1);
        if (!out) r = FWZIP_ERR_NOMEM;
        else {
            r = fwzip_inflate(comp, e->comp_size, out, e->file_size, &out_len);
            if (r == FWZIP_OK && out_len != e->file_size) r = FWZIP_ERR_CORRUPT;
            if (r == FWZIP_OK)
                if (out_desc)
                    snprintf(out_desc, desc_cap, "deflate, %u -> %u bytes", (unsigned)e->comp_size, (unsigned)e->file_size);
        }
    }
    if (comp) kfree(comp);

    if (r != FWZIP_OK) {
        if (out) kfree(out);
        return r;
    }
    if (fwzip_crc32(0, out, out_len) != e->crc32) {
        kfree(out);
        LOG_WARNING("fwzip: CRC mismatch for '%s' in '%s'", e->name, rel);
        return FWZIP_ERR_CORRUPT;
    }

    *data = out;
    *size = out_len;
    return FWZIP_OK;
}

int fwzip_read_mem(const uint8_t *mem, uint64_t mem_len, const struct fwzip *z, const struct fwzip_entry *e,
                   uint8_t **data, size_t *size, char *out_desc, size_t desc_cap) {
    if (!mem || !z || !e || !data || !size) return FWZIP_ERR_PARAM;
    *data = NULL;
    *size = 0;

    if (e->header_offset + 30 > mem_len) return FWZIP_ERR_CORRUPT;
    const uint8_t *lfh = mem + e->header_offset;
    if (rd32(lfh) != 0x04034b50u) return FWZIP_ERR_CORRUPT;
    uint16_t l_method = rd16(lfh + 8);
    uint16_t l_flags = rd16(lfh + 6);
    uint16_t name_len = rd16(lfh + 26);
    uint16_t extra_len = rd16(lfh + 28);
    if (l_method != e->method || (l_flags & 1)) return FWZIP_ERR_CORRUPT;
    uint64_t data_off = e->header_offset + 30 + name_len + extra_len;
    if (data_off + e->comp_size > mem_len) return FWZIP_ERR_CORRUPT;
    const uint8_t *comp = mem + data_off;

    uint8_t *out = NULL;
    size_t out_len = 0;
    int r = FWZIP_OK;
    if (e->method == 0) {
        if (e->comp_size != e->file_size) r = FWZIP_ERR_CORRUPT;
        else {
            out = kmalloc(e->file_size ? e->file_size : 1);
            if (!out) r = FWZIP_ERR_NOMEM;
            else {
                memcpy(out, comp, e->file_size);
                out_len = e->file_size;
                if (out_desc) snprintf(out_desc, desc_cap, "stored, %u bytes", (unsigned)e->file_size);
            }
        }
    } else {
        out = kmalloc(e->file_size ? e->file_size : 1);
        if (!out) r = FWZIP_ERR_NOMEM;
        else {
            r = fwzip_inflate(comp, e->comp_size, out, e->file_size, &out_len);
            if (r == FWZIP_OK && out_len != e->file_size) r = FWZIP_ERR_CORRUPT;
            if (r == FWZIP_OK && out_desc)
                snprintf(out_desc, desc_cap, "deflate, %u -> %u bytes", (unsigned)e->comp_size,
                         (unsigned)e->file_size);
        }
    }

    if (r != FWZIP_OK) {
        if (out) kfree(out);
        return r;
    }
    if (fwzip_crc32(0, out, out_len) != e->crc32) {
        kfree(out);
        LOG_WARNING("fwzip: CRC mismatch for '%s' in the image archive", e->name);
        return FWZIP_ERR_CORRUPT;
    }

    *data = out;
    *size = out_len;
    return FWZIP_OK;
}
