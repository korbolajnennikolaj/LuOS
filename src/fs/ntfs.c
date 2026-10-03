#include "fs/ntfs.h"

#include "components/logger.h"
#include "components/Memory/heap.h"

#include <string.h>

#define NTFS_BOUNCE_SIZE 65536u
#define NTFS_UPCASE_ENTRIES 65536u

static uint8_t *g_bounce;

static inline uint16_t rd16(const uint8_t *p) { uint16_t v; memcpy(&v, p, 2); return v; }
static inline uint32_t rd32(const uint8_t *p) { uint32_t v; memcpy(&v, p, 4); return v; }
static inline uint64_t rd64(const uint8_t *p) { uint64_t v; memcpy(&v, p, 8); return v; }

static const uint16_t g_i30[4] = { '$', 'I', '3', '0' };

static int dev_read(ntfs_fs_t *fs, uint64_t offset, void *buf, uint64_t len) {
    uint32_t ss = fs->dev->sector_size ? fs->dev->sector_size : 512;
    uint8_t *out = (uint8_t *)buf;
    while (len > 0) {
        uint64_t lba = offset / ss;
        uint32_t skip = (uint32_t)(offset % ss);
        uint64_t want = skip + len;
        uint32_t sectors = (uint32_t)((want + ss - 1) / ss);
        if (sectors > NTFS_BOUNCE_SIZE / ss) sectors = NTFS_BOUNCE_SIZE / ss;
        if (fs->dev->read_sectors(fs->dev, lba, sectors, g_bounce) != 0) {
            LOG_ERROR("%s: read failed at lba %llu", fs->dev->name, (unsigned long long)lba);
            return FS_ERR_IO;
        }
        uint64_t got = (uint64_t)sectors * ss - skip;
        if (got > len) got = len;
        memcpy(out, g_bounce + skip, (size_t)got);
        out += got;
        offset += got;
        len -= got;
    }
    return FS_OK;
}

static int apply_fixup(uint8_t *buf, uint32_t size) {
    uint16_t usa_off = rd16(buf + 4);
    uint16_t usa_count = rd16(buf + 6);
    if (usa_count == 0 || (uint32_t)usa_off + (uint32_t)usa_count * 2 > size) return FS_ERR_CORRUPT;
    uint16_t usn = rd16(buf + usa_off);
    for (uint32_t i = 1; i < usa_count; i++) {
        uint32_t pos = i * NTFS_FIXUP_STRIDE - 2;
        if (pos + 2 > size) return FS_ERR_CORRUPT;
        if (rd16(buf + pos) != usn) return FS_ERR_CORRUPT;
        memcpy(buf + pos, buf + usa_off + i * 2, 2);
    }
    return FS_OK;
}

static void attr_free(ntfs_attr_t *a) {
    if (a->value) kfree(a->value);
    if (a->runs) kfree(a->runs);
    memset(a, 0, sizeof(*a));
}

static int run_append(ntfs_attr_t *a, uint64_t vcn, int64_t lcn, uint64_t length) {
    if (a->run_count && a->runs[a->run_count - 1].lcn >= 0 && lcn >= 0 &&
        a->runs[a->run_count - 1].vcn + a->runs[a->run_count - 1].length == vcn &&
        a->runs[a->run_count - 1].lcn + (int64_t)a->runs[a->run_count - 1].length == lcn) {
        a->runs[a->run_count - 1].length += length;
        return FS_OK;
    }
    if ((a->run_count & 31u) == 0) {
        ntfs_run_t *n = krealloc(a->runs, sizeof(ntfs_run_t) * (a->run_count + 32));
        if (!n) return FS_ERR_NOMEM;
        a->runs = n;
    }
    a->runs[a->run_count].vcn = vcn;
    a->runs[a->run_count].lcn = lcn;
    a->runs[a->run_count].length = length;
    a->run_count++;
    return FS_OK;
}

static int decode_runs(const uint8_t *p, const uint8_t *end, uint64_t start_vcn, ntfs_attr_t *a) {
    uint64_t vcn = start_vcn;
    int64_t lcn = 0;
    while (p < end && *p) {
        uint8_t header = *p++;
        uint32_t len_size = header & 0x0Fu;
        uint32_t off_size = header >> 4;
        if (len_size == 0 || len_size > 8 || off_size > 8 || p + len_size + off_size > end) return FS_ERR_CORRUPT;

        uint64_t length = 0;
        for (uint32_t i = 0; i < len_size; i++) length |= (uint64_t)p[i] << (8 * i);
        p += len_size;

        int64_t run_lcn = -1;
        if (off_size) {
            uint64_t raw = 0;
            for (uint32_t i = 0; i < off_size; i++) raw |= (uint64_t)p[i] << (8 * i);
            if (off_size < 8 && (p[off_size - 1] & 0x80)) raw |= ~0ull << (8 * off_size);
            p += off_size;
            lcn += (int64_t)raw;
            run_lcn = lcn;
        }
        int r = run_append(a, vcn, run_lcn, length);
        if (r != FS_OK) return r;
        vcn += length;
    }
    return FS_OK;
}

static bool attr_name_is(const uint8_t *attr, const uint16_t *name, uint8_t name_len) {
    uint8_t len = attr[9];
    if (len != name_len) return false;
    if (len == 0) return true;
    const uint8_t *n = attr + rd16(attr + 10);
    for (uint8_t i = 0; i < len; i++)
        if (rd16(n + i * 2) != name[i]) return false;
    return true;
}

static int segment_load(const uint8_t *attr, uint32_t attr_len, ntfs_attr_t *out, bool first) {
    if (!attr[8]) {
        if (!first) return FS_OK;
        uint32_t vlen = rd32(attr + 16);
        uint16_t voff = rd16(attr + 20);
        if ((uint32_t)voff + vlen > attr_len) return FS_ERR_CORRUPT;
        out->resident = true;
        out->flags = rd16(attr + 12);
        out->value = kmalloc(vlen ? vlen : 1);
        if (!out->value) return FS_ERR_NOMEM;
        memcpy(out->value, attr + voff, vlen);
        out->value_length = vlen;
        out->data_size = vlen;
        out->init_size = vlen;
        return FS_OK;
    }

    uint64_t start_vcn = rd64(attr + 16);
    uint16_t run_off = rd16(attr + 32);
    if (run_off >= attr_len) return FS_ERR_CORRUPT;
    if (start_vcn == 0) {
        out->resident = false;
        out->flags = rd16(attr + 12);
        out->data_size = rd64(attr + 48);
        out->init_size = rd64(attr + 56);
    }
    return decode_runs(attr + run_off, attr + attr_len, start_vcn, out);
}

static int attr_read(ntfs_fs_t *fs, const ntfs_attr_t *a, uint64_t pos, void *buf, uint64_t len) {
    uint8_t *out = (uint8_t *)buf;
    if (a->resident) {
        if (pos >= a->value_length) return 0;
        if (len > a->value_length - pos) len = a->value_length - pos;
        memcpy(out, a->value + pos, (size_t)len);
        return (int)len;
    }

    if (pos >= a->data_size) return 0;
    if (len > a->data_size - pos) len = a->data_size - pos;

    uint64_t done = 0;
    uint32_t hint = 0;
    while (done < len) {
        uint64_t p = pos + done;
        if (p >= a->init_size) {
            memset(out + done, 0, (size_t)(len - done));
            done = len;
            break;
        }
        uint64_t vcn = p / fs->cluster_size;
        uint32_t off = (uint32_t)(p % fs->cluster_size);

        if (hint >= a->run_count || vcn < a->runs[hint].vcn) hint = 0;
        while (hint < a->run_count && vcn >= a->runs[hint].vcn + a->runs[hint].length) hint++;
        if (hint >= a->run_count || vcn < a->runs[hint].vcn) return FS_ERR_CORRUPT;

        const ntfs_run_t *run = &a->runs[hint];
        uint64_t run_left = (run->vcn + run->length - vcn) * fs->cluster_size - off;
        uint64_t chunk = len - done;
        if (chunk > run_left) chunk = run_left;
        if (p + chunk > a->init_size) chunk = a->init_size - p;

        if (run->lcn < 0) {
            memset(out + done, 0, (size_t)chunk);
        } else {
            uint64_t byte = ((uint64_t)run->lcn + (vcn - run->vcn)) * fs->cluster_size + off;
            int r = dev_read(fs, byte, out + done, chunk);
            if (r != FS_OK) return r;
        }
        done += chunk;
    }
    return (int)done;
}

static int read_record(ntfs_fs_t *fs, uint64_t recno, uint8_t *buf) {
    int n = attr_read(fs, &fs->mft, recno * fs->record_size, buf, fs->record_size);
    if (n != (int)fs->record_size) return n < 0 ? n : FS_ERR_IO;
    if (memcmp(buf, "FILE", 4) != 0) return FS_ERR_CORRUPT;
    return apply_fixup(buf, fs->record_size);
}

static const uint8_t *record_find(const uint8_t *rec, uint32_t size, uint32_t type,
                                  const uint16_t *name, uint8_t name_len, int instance) {
    uint32_t off = rd16(rec + 20);
    while (off + 8 <= size) {
        const uint8_t *a = rec + off;
        uint32_t t = rd32(a);
        if (t == NTFS_AT_END) break;
        uint32_t len = rd32(a + 4);
        if (len < 16 || off + len > size) break;
        if (t == type && attr_name_is(a, name, name_len) && (instance < 0 || rd16(a + 14) == (uint16_t)instance))
            return a;
        off += len;
    }
    return NULL;
}

static int find_attr(ntfs_fs_t *fs, uint64_t recno, const uint8_t *rec, uint32_t type,
                     const uint16_t *name, uint8_t name_len, ntfs_attr_t *out) {
    memset(out, 0, sizeof(*out));

    const uint8_t *list_attr = record_find(rec, fs->record_size, NTFS_AT_ATTRIBUTE_LIST, NULL, 0, -1);
    if (!list_attr || type == NTFS_AT_ATTRIBUTE_LIST) {
        const uint8_t *a = record_find(rec, fs->record_size, type, name, name_len, -1);
        if (!a) return FS_ERR_NOENT;
        return segment_load(a, rd32(a + 4), out, true);
    }

    ntfs_attr_t list;
    memset(&list, 0, sizeof(list));
    int r = segment_load(list_attr, rd32(list_attr + 4), &list, true);
    if (r != FS_OK) { attr_free(&list); return r; }
    uint64_t list_len = list.resident ? list.value_length : list.data_size;
    uint8_t *lbuf = kmalloc(list_len ? list_len : 1);
    if (!lbuf) { attr_free(&list); return FS_ERR_NOMEM; }
    if (attr_read(fs, &list, 0, lbuf, list_len) != (int)list_len) { kfree(lbuf); attr_free(&list); return FS_ERR_IO; }
    attr_free(&list);

    uint8_t *ext = kmalloc(fs->record_size);
    if (!ext) { kfree(lbuf); return FS_ERR_NOMEM; }

    bool found = false;
    r = FS_OK;
    for (uint64_t off = 0; off + 26 <= list_len; ) {
        const uint8_t *e = lbuf + off;
        uint16_t elen = rd16(e + 4);
        if (elen < 26 || off + elen > list_len) break;
        off += elen;

        if (rd32(e) != type) continue;
        uint8_t nlen = e[6];
        if (nlen != name_len) continue;
        bool same = true;
        for (uint8_t i = 0; i < nlen && same; i++)
            if (rd16(e + e[7] + i * 2) != name[i]) same = false;
        if (!same) continue;

        uint64_t ref = rd64(e + 16) & NTFS_REF_MASK;
        uint16_t instance = rd16(e + 24);
        const uint8_t *src = rec;
        if (ref != recno) {
            r = read_record(fs, ref, ext);
            if (r != FS_OK) break;
            src = ext;
        }
        const uint8_t *a = record_find(src, fs->record_size, type, name, name_len, instance);
        if (!a) { r = FS_ERR_CORRUPT; break; }
        r = segment_load(a, rd32(a + 4), out, !found);
        if (r != FS_OK) break;
        found = true;
    }

    kfree(ext);
    kfree(lbuf);
    if (r != FS_OK) { attr_free(out); return r; }
    return found ? FS_OK : FS_ERR_NOENT;
}

static inline uint16_t upcase_char(ntfs_fs_t *fs, uint16_t c) {
    if (fs->upcase) return fs->upcase[c];
    return (c >= 'a' && c <= 'z') ? (uint16_t)(c - 32) : c;
}

static int utf8_to_utf16(const char *src, uint16_t *out, uint32_t max) {
    const uint8_t *p = (const uint8_t *)src;
    uint32_t n = 0;
    while (*p) {
        uint32_t cp;
        if (p[0] < 0x80) { cp = p[0]; p += 1; }
        else if ((p[0] & 0xE0) == 0xC0 && (p[1] & 0xC0) == 0x80) { cp = ((uint32_t)(p[0] & 0x1F) << 6) | (p[1] & 0x3F); p += 2; }
        else if ((p[0] & 0xF0) == 0xE0 && (p[1] & 0xC0) == 0x80 && (p[2] & 0xC0) == 0x80) {
            cp = ((uint32_t)(p[0] & 0x0F) << 12) | ((uint32_t)(p[1] & 0x3F) << 6) | (p[2] & 0x3F); p += 3;
        } else if ((p[0] & 0xF8) == 0xF0 && (p[1] & 0xC0) == 0x80 && (p[2] & 0xC0) == 0x80 && (p[3] & 0xC0) == 0x80) {
            cp = ((uint32_t)(p[0] & 0x07) << 18) | ((uint32_t)(p[1] & 0x3F) << 12) |
                 ((uint32_t)(p[2] & 0x3F) << 6) | (p[3] & 0x3F); p += 4;
        } else return -1;
        if (cp >= 0x10000) {
            if (n + 2 > max) return -1;
            cp -= 0x10000;
            out[n++] = (uint16_t)(0xD800 + (cp >> 10));
            out[n++] = (uint16_t)(0xDC00 + (cp & 0x3FF));
        } else {
            if (n + 1 > max) return -1;
            out[n++] = (uint16_t)cp;
        }
    }
    return (int)n;
}

static void utf16_to_utf8(const uint8_t *src, uint32_t len, char *dst, size_t cap) {
    size_t o = 0;
    for (uint32_t i = 0; i < len && o + 4 < cap; i++) {
        uint32_t c = rd16(src + i * 2);
        if (c >= 0xD800 && c <= 0xDBFF && i + 1 < len) {
            uint32_t lo = rd16(src + (i + 1) * 2);
            if (lo >= 0xDC00 && lo <= 0xDFFF) { c = 0x10000u + ((c - 0xD800u) << 10) + (lo - 0xDC00u); i++; }
        }
        if (c < 0x80) dst[o++] = (char)c;
        else if (c < 0x800) { dst[o++] = (char)(0xC0 | (c >> 6)); dst[o++] = (char)(0x80 | (c & 0x3F)); }
        else if (c < 0x10000) {
            dst[o++] = (char)(0xE0 | (c >> 12)); dst[o++] = (char)(0x80 | ((c >> 6) & 0x3F)); dst[o++] = (char)(0x80 | (c & 0x3F));
        } else {
            dst[o++] = (char)(0xF0 | (c >> 18)); dst[o++] = (char)(0x80 | ((c >> 12) & 0x3F));
            dst[o++] = (char)(0x80 | ((c >> 6) & 0x3F)); dst[o++] = (char)(0x80 | (c & 0x3F));
        }
    }
    dst[o] = '\0';
}

typedef int (*ntfs_index_cb)(ntfs_fs_t *fs, void *ctx, uint64_t ref, const uint8_t *fn, uint16_t fn_len);

static int scan_entries(ntfs_fs_t *fs, const uint8_t *p, const uint8_t *end, ntfs_index_cb cb, void *ctx) {
    while (p + 16 <= end) {
        uint16_t elen = rd16(p + 8);
        uint16_t klen = rd16(p + 10);
        uint16_t flags = rd16(p + 12);
        if (flags & NTFS_INDEX_ENTRY_LAST) break;
        if (elen < 16 || p + elen > end) return FS_ERR_CORRUPT;
        if (klen >= 66 && 16u + klen <= elen) {
            int stop = cb(fs, ctx, rd64(p), p + 16, klen);
            if (stop) return stop;
        }
        p += elen;
    }
    return FS_OK;
}

static int index_scan(ntfs_fs_t *fs, uint64_t recno, ntfs_index_cb cb, void *ctx) {
    uint8_t *rec = kmalloc(fs->record_size);
    if (!rec) return FS_ERR_NOMEM;
    int r = read_record(fs, recno, rec);
    if (r != FS_OK) { kfree(rec); return r; }
    if (!(rd16(rec + 22) & NTFS_MFT_FLAG_DIRECTORY)) { kfree(rec); return FS_ERR_NOTDIR; }

    ntfs_attr_t root, alloc, bitmap;
    r = find_attr(fs, recno, rec, NTFS_AT_INDEX_ROOT, g_i30, 4, &root);
    if (r != FS_OK) { kfree(rec); return r; }
    bool have_alloc = find_attr(fs, recno, rec, NTFS_AT_INDEX_ALLOCATION, g_i30, 4, &alloc) == FS_OK;
    bool have_bitmap = find_attr(fs, recno, rec, NTFS_AT_BITMAP, g_i30, 4, &bitmap) == FS_OK;
    kfree(rec);

    int stop = 0;
    if (root.resident && root.value_length >= 32) {
        const uint8_t *h = root.value + 16;
        uint32_t eoff = rd32(h);
        uint32_t ilen = rd32(h + 4);
        if (16u + ilen <= root.value_length && eoff < ilen)
            stop = scan_entries(fs, h + eoff, h + ilen, cb, ctx);
    }
    attr_free(&root);

    uint8_t *bm = NULL;
    uint64_t bm_len = 0;
    if (have_bitmap) {
        bm_len = bitmap.resident ? bitmap.value_length : bitmap.data_size;
        bm = kmalloc(bm_len ? bm_len : 1);
        if (bm && attr_read(fs, &bitmap, 0, bm, bm_len) != (int)bm_len) { kfree(bm); bm = NULL; }
        attr_free(&bitmap);
    }

    if (have_alloc && stop == 0) {
        uint8_t *block = kmalloc(fs->index_record_size);
        uint64_t total = alloc.resident ? alloc.value_length : alloc.data_size;
        uint64_t blocks = total / fs->index_record_size;
        for (uint64_t b = 0; block && b < blocks && stop == 0; b++) {
            if (bm && ((b >> 3) >= bm_len || !(bm[b >> 3] & (1u << (b & 7))))) continue;
            if (attr_read(fs, &alloc, b * fs->index_record_size, block, fs->index_record_size) != (int)fs->index_record_size)
                continue;
            if (memcmp(block, "INDX", 4) != 0 || apply_fixup(block, fs->index_record_size) != FS_OK) continue;
            uint32_t eoff = rd32(block + 24);
            uint32_t ilen = rd32(block + 28);
            if (24u + ilen > fs->index_record_size || eoff >= ilen) continue;
            stop = scan_entries(fs, block + 24 + eoff, block + 24 + ilen, cb, ctx);
        }
        if (block) kfree(block);
    }
    if (have_alloc) attr_free(&alloc);
    if (bm) kfree(bm);
    return stop < 0 ? stop : FS_OK;
}

typedef struct {
    const uint16_t *name;
    uint32_t len;
    uint64_t ref;
    bool found;
} ntfs_lookup_ctx_t;

static int lookup_cb(ntfs_fs_t *fs, void *ctx, uint64_t ref, const uint8_t *fn, uint16_t fn_len) {
    ntfs_lookup_ctx_t *c = (ntfs_lookup_ctx_t *)ctx;
    uint8_t nlen = fn[64];
    if (66u + (uint32_t)nlen * 2 > fn_len || nlen != c->len) return 0;
    for (uint32_t i = 0; i < nlen; i++)
        if (upcase_char(fs, rd16(fn + 66 + i * 2)) != upcase_char(fs, c->name[i])) return 0;
    c->ref = ref & NTFS_REF_MASK;
    c->found = true;
    return 1;
}

static int resolve(ntfs_fs_t *fs, const char *path, uint64_t *out_rec) {
    fs_path_parts_t pp;
    int r = fs_split_path(path, &pp);
    if (r != FS_OK) return r;

    uint64_t cur = NTFS_REC_ROOT;
    for (int i = 0; i < pp.count; i++) {
        uint16_t name16[256];
        int n = utf8_to_utf16(pp.comps[i], name16, 255);
        if (n <= 0) return FS_ERR_PARAM;
        ntfs_lookup_ctx_t ctx = { name16, (uint32_t)n, 0, false };
        r = index_scan(fs, cur, lookup_cb, &ctx);
        if (r != FS_OK) return r;
        if (!ctx.found) return FS_ERR_NOENT;
        cur = ctx.ref;
    }
    *out_rec = cur;
    return FS_OK;
}

static int record_info(ntfs_fs_t *fs, uint64_t recno, bool *is_dir, ntfs_attr_t *data) {
    uint8_t *rec = kmalloc(fs->record_size);
    if (!rec) return FS_ERR_NOMEM;
    int r = read_record(fs, recno, rec);
    if (r != FS_OK) { kfree(rec); return r; }
    if (!(rd16(rec + 22) & NTFS_MFT_FLAG_IN_USE)) { kfree(rec); return FS_ERR_NOENT; }
    *is_dir = (rd16(rec + 22) & NTFS_MFT_FLAG_DIRECTORY) != 0;
    if (data) {
        r = find_attr(fs, recno, rec, NTFS_AT_DATA, NULL, 0, data);
        if (r == FS_ERR_NOENT) { memset(data, 0, sizeof(*data)); data->resident = true; r = FS_OK; }
    }
    kfree(rec);
    return r;
}

static int ntfs_op_open(fs_t *fsroot, const char *path, bool create, bool truncate_flag, fs_file_t *out) {
    ntfs_fs_t *fs = (ntfs_fs_t *)fsroot->priv;
    uint64_t recno;
    int r = resolve(fs, path, &recno);
    if (r == FS_ERR_NOENT && create) return FS_ERR_NOSUPP;
    if (r != FS_OK) return r;
    if (truncate_flag) return FS_ERR_NOSUPP;

    ntfs_file_t *nf = kmalloc(sizeof(ntfs_file_t));
    if (!nf) return FS_ERR_NOMEM;
    memset(nf, 0, sizeof(*nf));
    nf->fs = fs;

    bool is_dir;
    r = record_info(fs, recno, &is_dir, &nf->data);
    if (r == FS_OK && is_dir) r = FS_ERR_ISDIR;
    if (r == FS_OK && (nf->data.flags & (NTFS_ATTR_FLAG_COMPRESSED | NTFS_ATTR_FLAG_ENCRYPTED))) {
        LOG_WARNING("%s: compressed or encrypted file is not supported", fs->dev->name);
        r = FS_ERR_NOSUPP;
    }
    if (r != FS_OK) { attr_free(&nf->data); kfree(nf); return r; }

    out->priv = nf;
    out->size = nf->data.resident ? nf->data.value_length : nf->data.data_size;
    out->pos = 0;
    out->writable = false;
    return FS_OK;
}

static int ntfs_op_close(fs_file_t *f) {
    ntfs_file_t *nf = (ntfs_file_t *)f->priv;
    if (!nf) return FS_OK;
    attr_free(&nf->data);
    kfree(nf);
    f->priv = NULL;
    return FS_OK;
}

static int64_t ntfs_op_read(fs_file_t *f, void *buf, uint64_t size) {
    ntfs_file_t *nf = (ntfs_file_t *)f->priv;
    if (f->pos >= f->size) return 0;
    if (size > f->size - f->pos) size = f->size - f->pos;
    if (size > 0x7FFFFFFFu) size = 0x7FFFFFFFu;
    int n = attr_read(nf->fs, &nf->data, f->pos, buf, size);
    if (n > 0) f->pos += (uint64_t)n;
    return n;
}

static int64_t ntfs_op_write(fs_file_t *f, const void *buf, uint64_t size) {
    (void)f; (void)buf; (void)size;
    return FS_ERR_NOSUPP;
}

static int ntfs_op_seek(fs_file_t *f, uint64_t pos) { f->pos = pos; return FS_OK; }

static int ntfs_op_truncate(fs_file_t *f, uint64_t size) { (void)f; (void)size; return FS_ERR_NOSUPP; }

static int collect_cb(ntfs_fs_t *fs, void *ctx, uint64_t ref, const uint8_t *fn, uint16_t fn_len) {
    (void)fs;
    ntfs_diriter_t *it = (ntfs_diriter_t *)ctx;
    uint8_t nlen = fn[64];
    uint8_t ns = fn[65];
    if (66u + (uint32_t)nlen * 2 > fn_len) return 0;
    if (ns == NTFS_NAMESPACE_DOS) return 0;
    if ((ref & NTFS_REF_MASK) < NTFS_REC_FIRST_USER) return 0;

    if (it->count == it->capacity) {
        uint32_t cap = it->capacity ? it->capacity * 2 : 32;
        ntfs_dirent_cache_t *n = krealloc(it->entries, sizeof(ntfs_dirent_cache_t) * cap);
        if (!n) return FS_ERR_NOMEM;
        it->entries = n;
        it->capacity = cap;
    }
    ntfs_dirent_cache_t *e = &it->entries[it->count++];
    utf16_to_utf8(fn + 66, nlen, e->name, sizeof(e->name));
    e->is_dir = (rd32(fn + 56) & 0x10000000u) != 0;
    e->size = rd64(fn + 48);
    return 0;
}

static int ntfs_op_opendir(fs_t *fsroot, const char *path, fs_dir_t *out) {
    ntfs_fs_t *fs = (ntfs_fs_t *)fsroot->priv;
    uint64_t recno;
    int r = resolve(fs, path, &recno);
    if (r != FS_OK) return r;

    ntfs_diriter_t *it = kmalloc(sizeof(ntfs_diriter_t));
    if (!it) return FS_ERR_NOMEM;
    memset(it, 0, sizeof(*it));
    r = index_scan(fs, recno, collect_cb, it);
    if (r != FS_OK) {
        if (it->entries) kfree(it->entries);
        kfree(it);
        return r;
    }
    out->priv = it;
    return FS_OK;
}

static int ntfs_op_readdir(fs_dir_t *dir, fs_dirent_t *out) {
    ntfs_diriter_t *it = (ntfs_diriter_t *)dir->priv;
    if (it->index >= it->count) return FS_ERR_EOF;
    ntfs_dirent_cache_t *e = &it->entries[it->index++];
    strncpy(out->name, e->name, FS_MAX_NAME);
    out->name[FS_MAX_NAME] = '\0';
    out->type = e->is_dir ? FS_ENTRY_DIR : FS_ENTRY_FILE;
    out->size = e->is_dir ? 0 : e->size;
    return FS_OK;
}

static int ntfs_op_closedir(fs_dir_t *dir) {
    ntfs_diriter_t *it = (ntfs_diriter_t *)dir->priv;
    if (it) {
        if (it->entries) kfree(it->entries);
        kfree(it);
    }
    dir->priv = NULL;
    return FS_OK;
}

static int ntfs_op_stat(fs_t *fsroot, const char *path, fs_dirent_t *out) {
    ntfs_fs_t *fs = (ntfs_fs_t *)fsroot->priv;
    fs_path_parts_t pp;
    int r = fs_split_path(path, &pp);
    if (r != FS_OK) return r;

    uint64_t recno;
    r = resolve(fs, path, &recno);
    if (r != FS_OK) return r;

    bool is_dir;
    ntfs_attr_t data;
    r = record_info(fs, recno, &is_dir, &data);
    if (r != FS_OK) return r;

    const char *name = pp.count ? pp.comps[pp.count - 1] : "/";
    strncpy(out->name, name, FS_MAX_NAME);
    out->name[FS_MAX_NAME] = '\0';
    out->type = is_dir ? FS_ENTRY_DIR : FS_ENTRY_FILE;
    out->size = is_dir ? 0 : (data.resident ? data.value_length : data.data_size);
    attr_free(&data);
    return FS_OK;
}

static int ntfs_op_readonly_path(fs_t *fsroot, const char *path) {
    (void)fsroot; (void)path;
    return FS_ERR_NOSUPP;
}

static int ntfs_op_unmount(fs_t *fsroot) {
    ntfs_fs_t *fs = (ntfs_fs_t *)fsroot->priv;
    if (fs) {
        attr_free(&fs->mft);
        if (fs->upcase) kfree(fs->upcase);
        kfree(fs);
    }
    fsroot->priv = NULL;
    return FS_OK;
}

const fs_ops_t ntfs_ops = {
    .open = ntfs_op_open,
    .close = ntfs_op_close,
    .read = ntfs_op_read,
    .write = ntfs_op_write,
    .seek = ntfs_op_seek,
    .truncate = ntfs_op_truncate,
    .opendir = ntfs_op_opendir,
    .readdir = ntfs_op_readdir,
    .closedir = ntfs_op_closedir,
    .mkdir = ntfs_op_readonly_path,
    .unlink = ntfs_op_readonly_path,
    .rmdir = ntfs_op_readonly_path,
    .stat = ntfs_op_stat,
    .unmount = ntfs_op_unmount,
};

static int load_bootsector(struct block_device *dev, ntfs_bootsector_t *bs) {
    uint32_t ss = dev->sector_size ? dev->sector_size : 512;
    uint8_t *buf = kmalloc(ss);
    if (!buf) return FS_ERR_NOMEM;
    if (dev->read_sectors(dev, 0, 1, buf) != 0) { kfree(buf); return FS_ERR_IO; }
    memcpy(bs, buf, sizeof(*bs));
    bool sig = buf[510] == 0x55 && buf[511] == 0xAA;
    kfree(buf);
    if (!sig || memcmp(bs->oem_id, NTFS_OEM_ID, 8) != 0) return FS_ERR_CORRUPT;
    uint16_t bps = bs->bytes_per_sector;
    if (bps < 256 || bps > 4096 || (bps & (bps - 1)) != 0 || bs->sectors_per_cluster == 0) return FS_ERR_CORRUPT;
    return FS_OK;
}

int ntfs_probe(struct block_device *dev) {
    if (!dev) return FS_ERR_PARAM;
    ntfs_bootsector_t bs;
    return load_bootsector(dev, &bs);
}

static uint32_t ntfs_record_bytes(int8_t value, uint32_t cluster_size) {
    if (value > 0) return (uint32_t)value * cluster_size;
    if (value < -31) return 0;
    return 1u << (uint32_t)(-value);
}

static void load_upcase(ntfs_fs_t *fs) {
    uint8_t *rec = kmalloc(fs->record_size);
    if (!rec) return;
    ntfs_attr_t data;
    if (read_record(fs, NTFS_REC_UPCASE, rec) == FS_OK &&
        find_attr(fs, NTFS_REC_UPCASE, rec, NTFS_AT_DATA, NULL, 0, &data) == FS_OK) {
        uint16_t *table = kmalloc(NTFS_UPCASE_ENTRIES * 2);
        if (table && attr_read(fs, &data, 0, table, NTFS_UPCASE_ENTRIES * 2) == (int)(NTFS_UPCASE_ENTRIES * 2))
            fs->upcase = table;
        else if (table)
            kfree(table);
        attr_free(&data);
    }
    kfree(rec);
}

static void load_label(ntfs_fs_t *fs) {
    fs->label[0] = '\0';
    uint8_t *rec = kmalloc(fs->record_size);
    if (!rec) return;
    if (read_record(fs, NTFS_REC_VOLUME, rec) == FS_OK) {
        const uint8_t *a = record_find(rec, fs->record_size, NTFS_AT_VOLUME_NAME, NULL, 0, -1);
        if (a && !a[8]) utf16_to_utf8(a + rd16(a + 20), rd32(a + 16) / 2, fs->label, sizeof(fs->label));
    }
    kfree(rec);
}

int ntfs_mount(struct block_device *dev, fs_t *out) {
    if (!dev || !out) return FS_ERR_PARAM;

    ntfs_bootsector_t bs;
    int r = load_bootsector(dev, &bs);
    if (r != FS_OK) return r;

    if (!g_bounce) {
        g_bounce = kmalloc(NTFS_BOUNCE_SIZE);
        if (!g_bounce) return FS_ERR_NOMEM;
    }

    ntfs_fs_t *fs = kmalloc(sizeof(ntfs_fs_t));
    if (!fs) return FS_ERR_NOMEM;
    memset(fs, 0, sizeof(*fs));
    fs->dev = dev;
    fs->bytes_per_sector = bs.bytes_per_sector;
    uint32_t spc = bs.sectors_per_cluster;
    if (spc > 0x80) spc = 1u << (256u - spc);
    fs->cluster_size = fs->bytes_per_sector * spc;
    fs->mft_lcn = bs.mft_lcn;
    fs->record_size = ntfs_record_bytes(bs.clusters_per_mft_record, fs->cluster_size);
    fs->index_record_size = ntfs_record_bytes(bs.clusters_per_index_record, fs->cluster_size);

    if (fs->cluster_size == 0 || fs->record_size < 1024 || fs->record_size > 65536 ||
        fs->index_record_size < 512 || fs->index_record_size > 65536) {
        LOG_ERROR("%s: unsupported NTFS geometry", dev->name);
        kfree(fs);
        return FS_ERR_CORRUPT;
    }

    uint8_t *rec = kmalloc(fs->record_size);
    if (!rec) { kfree(fs); return FS_ERR_NOMEM; }
    r = dev_read(fs, fs->mft_lcn * fs->cluster_size, rec, fs->record_size);
    if (r == FS_OK && memcmp(rec, "FILE", 4) != 0) r = FS_ERR_CORRUPT;
    if (r == FS_OK) r = apply_fixup(rec, fs->record_size);
    if (r == FS_OK) {
        const uint8_t *a = record_find(rec, fs->record_size, NTFS_AT_DATA, NULL, 0, -1);
        r = a ? segment_load(a, rd32(a + 4), &fs->mft, true) : FS_ERR_CORRUPT;
    }
    if (r == FS_OK && record_find(rec, fs->record_size, NTFS_AT_ATTRIBUTE_LIST, NULL, 0, -1)) {
        ntfs_attr_t full;
        if (find_attr(fs, 0, rec, NTFS_AT_DATA, NULL, 0, &full) == FS_OK) {
            attr_free(&fs->mft);
            fs->mft = full;
        }
    }
    kfree(rec);
    if (r != FS_OK || fs->mft.resident) {
        LOG_ERROR("%s: cannot load $MFT (%d)", dev->name, r);
        attr_free(&fs->mft);
        kfree(fs);
        return r != FS_OK ? r : FS_ERR_CORRUPT;
    }

    load_upcase(fs);
    load_label(fs);

    out->type = FS_TYPE_NTFS;
    out->dev = dev;
    out->ops = &ntfs_ops;
    out->priv = fs;
    strncpy(out->label, fs->label, sizeof(out->label) - 1);
    out->label[sizeof(out->label) - 1] = '\0';

    LOG_DEBUG("mounted NTFS volume '%s', cluster=%u, MFT record=%u, index record=%u, read-only",
              fs->label[0] ? fs->label : "(no label)", (unsigned)fs->cluster_size,
              (unsigned)fs->record_size, (unsigned)fs->index_record_size);
    return FS_OK;
}
