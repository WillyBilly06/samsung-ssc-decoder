/* Decodes a harness .ssc file ([u16 LE length][frame] ...) to raw interleaved s16le stereo.
 * Usage: ssc_dec_tool in.ssc out.s16le [sample_rate=44100] */
#include <stdio.h>
#include <stdlib.h>

#include "ssc_decoder.h"
extern int ssc_debug_tell, ssc_debug_total_bits, ssc_debug_storage_bits;

int main(int argc, char **argv) {
    if (argc < 3) {
        fprintf(stderr, "usage: %s in.ssc out.s16le [sample_rate]\n", argv[0]);
        return 1;
    }
    int fs = argc > 3 ? atoi(argv[3]) : 44100;
    FILE *fi = fopen(argv[1], "rb"), *fo = fopen(argv[2], "wb");
    if (!fi || !fo) { perror("fopen"); return 1; }
    ssc_decoder *dec = ssc_decoder_create(fs);
    if (!dec) { fprintf(stderr, "create failed\n"); return 1; }
    static unsigned char frame[4096];
    static int16_t pcm[2 * SSC_FRAME_SAMPLES];
    unsigned char lb[2];
    int n = 0, errors = 0, size_mismatch = 0;
    while (fread(lb, 1, 2, fi) == 2) {
        int len = lb[0] | lb[1] << 8;
        if (len > (int)sizeof frame || fread(frame, 1, len, fi) != (size_t)len) break;
        if (ssc_frame_bytes(frame, len, fs) != len) size_mismatch++;
        int r = ssc_decode(dec, frame, len, pcm);
        if (r < 0) {
            errors++;
            if (errors < 5) fprintf(stderr, "frame %d: error %d\n", n, r);
        }
        if (getenv("SSC_TELL"))
            fprintf(stderr, "frame %d: tell %d budget %d storage %d\n", n, ssc_debug_tell,
                    ssc_debug_total_bits, ssc_debug_storage_bits);
        fwrite(pcm, sizeof(int16_t), 2 * SSC_FRAME_SAMPLES, fo);
        n++;
    }
    fprintf(stderr, "decoded %d frames, %d errors, %d size mismatches (state %d bytes)\n", n, errors,
            size_mismatch, ssc_decoder_size());
    ssc_decoder_destroy(dec);
    fclose(fi);
    fclose(fo);
    return 0;
}
