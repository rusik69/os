/* bunzip2.c — bzip2 decompression */
#include "unistd.h"
#include "stdio.h"
#include "string.h"
#include "stdlib.h"

static void mtf_decode(unsigned char *data, int len, unsigned char *mtf_list, int list_size) {
    (void)list_size;
    for (int i = 0; i < len; i++) {
        int idx = data[i];
        unsigned char val = mtf_list[idx];
        for (int j = idx; j > 0; j--)
            mtf_list[j] = mtf_list[j-1];
        mtf_list[0] = val;
        data[i] = val;
    }
}

static int bwt_inverse(unsigned char *data, int len, int orig_ptr, unsigned char *out) {
    int count[256] = {0};
    for (int i = 0; i < len; i++) count[data[i]]++;
    int cum_count[256];
    int sum = 0;
    for (int i = 0; i < 256; i++) { cum_count[i] = sum; sum += count[i]; }
    int *T = (int *)malloc(len * sizeof(int));
    if (!T) return -1;
    int *next = (int *)calloc(256, sizeof(int));
    if (!next) { free(T); return -1; }
    for (int i = 0; i < 256; i++) next[i] = cum_count[i];
    for (int i = 0; i < len; i++) { int c = data[i]; T[next[c]++] = i; }
    int idx = orig_ptr;
    for (int i = len - 1; i >= 0; i--) { idx = T[idx]; out[i] = data[idx]; }
    free(T); free(next);
    return 0;
}

typedef struct { unsigned char *data; unsigned long len; unsigned long pos; int bit_pos; int error; } br_t;
static int br_read_bit(br_t *br) {
    if (br->pos >= br->len) { br->error = 1; return 0; }
    int bit = (br->data[br->pos] >> (7 - br->bit_pos)) & 1;
    br->bit_pos++;
    if (br->bit_pos >= 8) { br->bit_pos = 0; br->pos++; }
    return bit;
}
static int br_read_bits(br_t *br, int n) {
    int val = 0;
    for (int i = 0; i < n; i++) val = (val << 1) | br_read_bit(br);
    return val;
}

typedef struct hn { int sym; struct hn *left, *right; } hnode_t;
static hnode_t *mk_hn(void) {
    hnode_t *n = (hnode_t *)malloc(sizeof(hnode_t));
    if (n) { n->sym = -1; n->left = n->right = 0; }
    return n;
}
static void free_hn(hnode_t *n) {
    if (!n) return;
    free_hn(n->left);
    free_hn(n->right);
    free(n);
}
static int build_huff(hnode_t *root, const int *lens, int n_syms) {
    int max_len = 0;
    for (int i = 0; i < n_syms; i++) if (lens[i] > max_len) max_len = lens[i];
    if (max_len == 0) { root->sym = 0; return 0; }
    int bl_count[21] = {0};
    for (int i = 0; i < n_syms; i++) if (lens[i] > 0 && lens[i] <= 20) bl_count[lens[i]]++;
    int code = 0, next_code[21] = {0};
    for (int bits = 1; bits <= max_len; bits++) {
        code = (code + bl_count[bits-1]) << 1;
        next_code[bits] = code;
    }
    for (int i = 0; i < n_syms; i++) {
        if (lens[i] == 0) continue;
        int len = lens[i], cod = next_code[len]++;
        hnode_t *node = root;
        for (int b = len - 1; b >= 0; b--) {
            int bit = (cod >> b) & 1;
            if (bit == 0) {
                if (!node->left) node->left = mk_hn();
                if (!node->left) return -1;
                node = node->left;
            } else {
                if (!node->right) node->right = mk_hn();
                if (!node->right) return -1;
                node = node->right;
            }
        }
        node->sym = i;
    }
    return 0;
}
static int huff_decode(br_t *br, hnode_t *root) {
    hnode_t *node = root;
    while (node && node->sym < 0) {
        int bit = br_read_bit(br);
        if (br->error) return -1;
        if (bit == 0) node = node->left; else node = node->right;
        if (!node) return -1;
    }
    return node ? node->sym : -1;
}

static int decompress_block(br_t *br, unsigned char *out, int *out_len, int max_out, int block_size_100k) {
    (void)max_out;
    int bsize = block_size_100k * 100000;
    br_read_bits(br, 32); br_read_bit(br);
    int orig_ptr = br_read_bits(br, 24);
    if (br->error) return -1;

    int in_use[256] = {0}, n_used = 0;
    for (int i = 0; i < 16; i++) {
        int v = br_read_bits(br, 16);
        for (int j = 0; j < 16; j++)
            if (v & (1 << (15 - j))) { in_use[i*16+j] = 1; n_used++; }
    }
    int n_groups = br_read_bits(br, 3);
    if (n_groups < 1 || n_groups > 6) return -1;
    int n_selectors = br_read_bits(br, 15);
    int *selectors = (int *)malloc(n_selectors * sizeof(int));
    if (!selectors) return -1;
    int k = 0;
    while (k < n_selectors) { int val = 0; while (br_read_bit(br)) val++; selectors[k++] = val; }
    hnode_t *trees[6];
    for (int i = 0; i < 6; i++) trees[i] = 0;
    for (int t = 0; t < n_groups; t++) {
        int lens[258]; memset(lens, 0, sizeof(lens));
        int curr = br_read_bits(br, 5);
        for (int s = 0; s < 258; s++) {
            while (br_read_bit(br)) { if (br_read_bit(br)) curr++; else curr--; }
            lens[s] = curr;
        }
        trees[t] = mk_hn();
        if (!trees[t]) { free(selectors); return -1; }
        if (build_huff(trees[t], lens, 258) != 0) {
            free(selectors); for (int i = 0; i <= t; i++) if (trees[i]) free_hn(trees[i]);
            return -1;
        }
    }

    unsigned char *decoded = (unsigned char *)malloc(bsize);
    if (!decoded) { free(selectors); return -1; }
    int nsyms = 0, eob = n_used + 1, run = 0, sel_idx = 0, sc = 0, t_cur = 0;

    while (sel_idx < n_selectors) {
        if (sc >= 50) { sc = 0; sel_idx++; if (sel_idx < n_selectors) t_cur = selectors[sel_idx]; }
        if (sel_idx >= n_selectors) break;
        int sym = huff_decode(br, trees[t_cur]);
        if (sym < 0 || br->error) break;
        sc++;
        if (sym == eob) {
            if (run > 0) { int cnt = (1 << run) - 1; for (int i = 0; i < cnt && nsyms < bsize; i++) decoded[nsyms++] = 0; run = 0; }
            break;
        }
        if (sym == 0 || sym == 1) { run++; continue; }
        if (run > 0) { int cnt = (1 << run) - 1; for (int i = 0; i < cnt && nsyms < bsize; i++) decoded[nsyms++] = 0; run = 0; }
        decoded[nsyms++] = (unsigned char)(sym - 2);
    }

    if (br->error) { free(decoded); free(selectors); for (int i = 0; i < n_groups; i++) if (trees[i]) free_hn(trees[i]); return -1; }
    unsigned char mtf_list[256]; int mtf_sz = 0;
    for (int i = 0; i < 256; i++) if (in_use[i]) mtf_list[mtf_sz++] = (unsigned char)i;
    mtf_decode(decoded, nsyms, mtf_list, mtf_sz);
    if (bwt_inverse(decoded, nsyms, orig_ptr, out) != 0) { free(decoded); free(selectors); for (int i = 0; i < n_groups; i++) if (trees[i]) free_hn(trees[i]); return -1; }
    *out_len = nsyms;
    free(decoded); free(selectors);
    for (int i = 0; i < n_groups; i++) if (trees[i]) free_hn(trees[i]);
    return 0;
}

static int decompress_bzip2(const unsigned char *in, unsigned long in_len, unsigned char *out, unsigned long *out_len, unsigned long max_out) {
    if (in_len < 4 || in[0] != 'B' || in[1] != 'Z' || in[2] != 'h') return -1;
    int blk = in[3] - '0';
    if (blk < 1 || blk > 9) return -1;
    br_t br;
    br.data = (unsigned char *)in + 4; br.len = in_len - 4; br.pos = 0; br.bit_pos = 0; br.error = 0;
    unsigned long total = 0;
    while (br.pos < br.len && !br.error) {
        int m1 = br_read_bits(&br, 24), m2 = br_read_bits(&br, 24);
        if (br.error) break;
        if (m1 == 0x314159 && m2 == 0x265359) {
            int olen = 0;
            if (decompress_block(&br, out + total, &olen, (int)(max_out - total), blk) != 0) return -1;
            total += olen;
        } else if (m1 == 0x177245 && m2 == 0x385090) break;
        else return -1;
    }
    *out_len = total;
    return 0;
}

static int process_file(const char *inpath, int to_stdout, int keep) {
    int fd_in, fd_out;
    char outpath[1024];
    fd_in = open(inpath, O_RDONLY, 0);
    if (fd_in < 0) { printf("bunzip2: %s: No such file\n", inpath); return 1; }
    unsigned char *in = 0; unsigned long in_len = 0, alloc = 65536;
    in = malloc(alloc);
    if (!in) { close(fd_in); return 1; }
    int n;
    while ((n = read(fd_in, in + in_len, alloc - in_len)) > 0) {
        in_len += n;
        if (in_len + 65536 > alloc) { alloc *= 2; unsigned char *tmp = realloc(in, alloc); if (!tmp) { free(in); close(fd_in); return 1; } in = tmp; }
    }
    close(fd_in);
    if (in_len < 4 || in[0] != 'B' || in[1] != 'Z' || in[2] != 'h') { printf("bunzip2: %s: not bzip2\n", inpath); free(in); return 1; }
    int blk = in[3] - '0';
    if (blk < 1 || blk > 9) { printf("bunzip2: %s: invalid block size\n", inpath); free(in); return 1; }
    unsigned long max_out = (unsigned long)blk * 100000 * 4;
    unsigned char *out = malloc(max_out);
    if (!out) { free(in); return 1; }
    unsigned long out_len = 0;
    if (decompress_bzip2(in, in_len, out, &out_len, max_out) != 0) { printf("bunzip2: %s: decompression error\n", inpath); free(out); free(in); return 1; }
    if (to_stdout) { write(STDOUT_FILENO, out, out_len); }
    else {
        unsigned long len = strlen(inpath);
        if (len > 4 && strcmp(inpath + len - 4, ".bz2") == 0) len -= 4;
        if (len > 1020) len = 1020;
        memcpy(outpath, inpath, len); outpath[len] = '\0';
        fd_out = open(outpath, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (fd_out < 0) { printf("bunzip2: cannot create %s\n", outpath); free(out); free(in); return 1; }
        write(fd_out, out, out_len); close(fd_out);
        if (!keep) unlink(inpath);
    }
    free(out); free(in);
    return 0;
}

int main(int argc, char *argv[]) {
    int keep = 0, to_stdout = 0, files_start = 1;
    if (argc < 2) { printf("usage: bunzip2 [-k] [-f] [-c] <file.bz2>...\n"); return 1; }
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-k") == 0) keep = 1;
        else if (strcmp(argv[i], "-f") == 0) {}
        else if (strcmp(argv[i], "-c") == 0) to_stdout = 1;
        else { files_start = i; break; }
    }
    for (int i = files_start; i < argc; i++) { if (process_file(argv[i], to_stdout, keep) != 0) return 1; }
    return 0;
}
