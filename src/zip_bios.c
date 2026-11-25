#include "zip_bios.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Minimal self-contained RFC 1951 DEFLATE (inflate) + ZIP reader (pure C99, zero deps) */

typedef struct {
    const uint8_t *src;
    size_t src_len;
    size_t byte_pos;
    uint32_t bit_buf;
    int bit_cnt;
} BitReader;

static inline uint32_t br_bits(BitReader *br, int n) {
    while (br->bit_cnt < n) {
        uint8_t b = (br->byte_pos < br->src_len) ? br->src[br->byte_pos++] : 0u;
        br->bit_buf |= (uint32_t)b << br->bit_cnt;
        br->bit_cnt += 8;
    }
    uint32_t v = br->bit_buf & ((1u << n) - 1u);
    br->bit_buf >>= n;
    br->bit_cnt -= n;
    return v;
}

static inline void br_align(BitReader *br) {
    int drop = br->bit_cnt & 7;
    br->bit_buf >>= drop;
    br->bit_cnt -= drop;
}

typedef struct {
    uint16_t count[16];
    uint16_t sym[288];
} HuffTree;

static bool huff_build(HuffTree *t, const uint8_t *lengths, int num_syms) {
    uint16_t offs[16];
    memset(t->count, 0, sizeof(t->count));
    for (int i = 0; i < num_syms; i++) {
        if (lengths[i] > 15) return false;
        t->count[lengths[i]]++;
    }
    t->count[0] = 0;
    uint16_t sum = 0;
    for (int i = 1; i <= 15; i++) {
        offs[i] = sum;
        sum = (uint16_t)(sum + t->count[i]);
    }
    for (int i = 0; i < num_syms; i++) {
        if (lengths[i] != 0) {
            t->sym[offs[lengths[i]]++] = (uint16_t)i;
        }
    }
    return true;
}

static int huff_decode(BitReader *br, const HuffTree *t) {
    int code = 0, first = 0, index = 0;
    for (int len = 1; len <= 15; len++) {
        code |= (int)br_bits(br, 1);
        int cnt = t->count[len];
        if (code - cnt < first) {
            return t->sym[index + (code - first)];
        }
        index += cnt;
        first = (first + cnt) << 1;
        code <<= 1;
    }
    return -1;
}

static bool inflate_raw(const uint8_t *src, size_t src_len, uint8_t *dst, size_t dst_len) {
    static const uint16_t len_base[29] = {
        3,4,5,6,7,8,9,10,11,13,15,17,19,23,27,31,
        35,43,51,59,67,83,99,115,131,163,195,227,258
    };
    static const uint8_t len_extra[29] = {
        0,0,0,0,0,0,0,0,1,1,1,1,2,2,2,2,
        3,3,3,3,4,4,4,4,5,5,5,5,0
    };
    static const uint16_t dist_base[30] = {
        1,2,3,4,5,7,9,13,17,25,33,49,65,97,129,193,
        257,385,513,769,1025,1537,2049,3073,4097,6145,8193,12289,16385,24577
    };
    static const uint8_t dist_extra[30] = {
        0,0,0,0,1,1,2,2,3,3,4,4,5,5,6,6,
        7,7,8,8,9,9,10,10,11,11,12,12,13,13
    };
    static const uint8_t cl_order[19] = {
        16,17,18,0,8,7,9,6,10,5,11,4,12,3,13,2,14,1,15
    };

    BitReader br = { src, src_len, 0, 0, 0 };
    size_t out_pos = 0;
    bool bfinal = false;

    while (!bfinal) {
        bfinal = br_bits(&br, 1) != 0;
        uint32_t btype = br_bits(&br, 2);

        if (btype == 0) {
            br_align(&br);
            uint16_t len = (uint16_t)br_bits(&br, 16);
            uint16_t nlen = (uint16_t)br_bits(&br, 16);
            if ((uint16_t)(len ^ nlen) != 0xFFFFu) return false;
            if (out_pos + len > dst_len) return false;
            for (uint16_t i = 0; i < len; i++) {
                dst[out_pos++] = (uint8_t)br_bits(&br, 8);
            }
        } else if (btype == 1 || btype == 2) {
            HuffTree lit_tree, dist_tree;
            if (btype == 1) {
                uint8_t lit_lens[288], dist_lens[32];
                for (int i = 0; i <= 143; i++) lit_lens[i] = 8;
                for (int i = 144; i <= 255; i++) lit_lens[i] = 9;
                for (int i = 256; i <= 279; i++) lit_lens[i] = 7;
                for (int i = 280; i <= 287; i++) lit_lens[i] = 8;
                for (int i = 0; i < 32; i++) dist_lens[i] = 5;
                huff_build(&lit_tree, lit_lens, 288);
                huff_build(&dist_tree, dist_lens, 32);
            } else {
                int hlit = (int)br_bits(&br, 5) + 257;
                int hdist = (int)br_bits(&br, 5) + 1;
                int hclen = (int)br_bits(&br, 4) + 4;
                if (hlit > 286 || hdist > 30) return false;

                uint8_t cl_lens[19];
                memset(cl_lens, 0, sizeof(cl_lens));
                for (int i = 0; i < hclen; i++) {
                    cl_lens[cl_order[i]] = (uint8_t)br_bits(&br, 3);
                }
                HuffTree cl_tree;
                if (!huff_build(&cl_tree, cl_lens, 19)) return false;

                uint8_t all_lens[286 + 30];
                int total = hlit + hdist;
                int idx = 0;
                while (idx < total) {
                    int sym = huff_decode(&br, &cl_tree);
                    if (sym < 0) return false;
                    if (sym < 16) {
                        all_lens[idx++] = (uint8_t)sym;
                    } else if (sym == 16) {
                        if (idx == 0) return false;
                        int rep = (int)br_bits(&br, 2) + 3;
                        uint8_t prev = all_lens[idx - 1];
                        while (rep-- > 0 && idx < total) all_lens[idx++] = prev;
                    } else if (sym == 17) {
                        int rep = (int)br_bits(&br, 3) + 3;
                        while (rep-- > 0 && idx < total) all_lens[idx++] = 0;
                    } else if (sym == 18) {
                        int rep = (int)br_bits(&br, 7) + 11;
                        while (rep-- > 0 && idx < total) all_lens[idx++] = 0;
                    } else {
                        return false;
                    }
                }
                if (!huff_build(&lit_tree, all_lens, hlit)) return false;
                if (!huff_build(&dist_tree, all_lens + hlit, hdist)) return false;
            }

            for (;;) {
                int sym = huff_decode(&br, &lit_tree);
                if (sym < 0) return false;
                if (sym < 256) {
                    if (out_pos >= dst_len) return false;
                    dst[out_pos++] = (uint8_t)sym;
                } else if (sym == 256) {
                    break;
                } else {
                    int lidx = sym - 257;
                    if (lidx < 0 || lidx >= 29) return false;
                    uint32_t length = len_base[lidx] + br_bits(&br, len_extra[lidx]);
                    int dsym = huff_decode(&br, &dist_tree);
                    if (dsym < 0 || dsym >= 30) return false;
                    uint32_t dist = dist_base[dsym] + br_bits(&br, dist_extra[dsym]);
                    if (dist == 0 || dist > out_pos || out_pos + length > dst_len) return false;
                    for (uint32_t k = 0; k < length; k++) {
                        dst[out_pos] = dst[out_pos - dist];
                        out_pos++;
                    }
                }
            }
        } else {
            return false;
        }
    }

    return out_pos == dst_len;
}

static inline uint16_t rd16(const uint8_t *p) {
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

static inline uint32_t rd32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

bool zip_load_gamecom_bios(const char *zip_path,
                           uint8_t *out_internal_4k,
                           uint8_t *out_external_256k) {
    FILE *f = fopen(zip_path, "rb");
    if (!f) return false;
    fseek(f, 0, SEEK_END);
    long fsz = ftell(f);
    if (fsz < 22 || fsz > 16 * 1024 * 1024) {
        fclose(f);
        return false;
    }
    rewind(f);
    uint8_t *buf = (uint8_t *)malloc((size_t)fsz);
    if (!buf) { fclose(f); return false; }
    size_t sz = fread(buf, 1, (size_t)fsz, f);
    fclose(f);
    if (sz != (size_t)fsz) { free(buf); return false; }

    /* Locate End of Central Directory (EOCD) signature 0x06054b50 */
    size_t eocd_pos = 0;
    bool found_eocd = false;
    size_t min_pos = (sz > 65557u) ? (sz - 65557u) : 0u;
    for (size_t p = sz - 22u; ; p--) {
        if (rd32(buf + p) == 0x06054B50u) {
            eocd_pos = p;
            found_eocd = true;
            break;
        }
        if (p == min_pos) break;
    }
    if (!found_eocd) { free(buf); return false; }

    uint16_t total_entries = rd16(buf + eocd_pos + 10);
    uint32_t cd_offset = rd32(buf + eocd_pos + 16);

    bool got_int = false;
    bool got_ext = false;
    size_t pos = cd_offset;

    for (uint16_t i = 0; i < total_entries && pos + 46u <= sz; i++) {
        if (rd32(buf + pos) != 0x02014B50u) break;
        uint16_t method    = rd16(buf + pos + 10);
        uint32_t comp_sz   = rd32(buf + pos + 20);
        uint32_t uncomp_sz = rd32(buf + pos + 24);
        uint16_t name_len  = rd16(buf + pos + 28);
        uint16_t extra_len = rd16(buf + pos + 30);
        uint16_t comm_len  = rd16(buf + pos + 32);
        uint32_t lfh_off   = rd32(buf + pos + 42);

        pos += 46u + name_len + extra_len + comm_len;

        uint8_t *target = NULL;
        if (uncomp_sz == 0x1000u && !got_int) {
            target = out_internal_4k;
        } else if (uncomp_sz == 0x40000u && !got_ext) {
            target = out_external_256k;
        } else {
            continue;
        }

        if (lfh_off + 30u > sz || rd32(buf + lfh_off) != 0x04034B50u) continue;
        uint16_t lfh_name_len  = rd16(buf + lfh_off + 26);
        uint16_t lfh_extra_len = rd16(buf + lfh_off + 28);
        size_t data_off = (size_t)lfh_off + 30u + lfh_name_len + lfh_extra_len;
        if (data_off + comp_sz > sz) continue;

        bool ok = false;
        if (method == 0 && comp_sz == uncomp_sz) {
            memcpy(target, buf + data_off, uncomp_sz);
            ok = true;
        } else if (method == 8) {
            ok = inflate_raw(buf + data_off, comp_sz, target, uncomp_sz);
        }

        if (ok) {
            if (uncomp_sz == 0x1000u) got_int = true;
            else if (uncomp_sz == 0x40000u) got_ext = true;
        }
    }

    free(buf);
    return got_int && got_ext;
}
