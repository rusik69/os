/* pcmplay.c — PCM audio playback test
 *
 * Generates a 440 Hz sine wave in 16-bit signed stereo PCM format
 * at 44100 Hz sample rate.  Attempts to play via /dev/dsp (OSS).
 *
 * Usage: pcmplay [freq] [duration_ms]
 *   freq:        Hz (20-20000, default 440)
 *   duration_ms: milliseconds (10-5000, default 1000)
 *
 * Task #15 — D142 Build + test audio playback in userspace
 */
#include "unistd.h"
#include "stdio.h"
#include "string.h"
#include "stdlib.h"

#define PCM_FRAME_SIZE  4     /* 16-bit stereo = 4 bytes per frame */
#define PCM_SAMPLE_RATE 44100

/* 64-entry sine lookup (one quadrant, mirrored for full cycle) */
static const short sin_quad[64] = {
	0, 1608, 3212, 4808, 6393, 7962, 9512, 11039,
	12539, 14010, 15446, 16846, 18204, 19519, 20787, 22005,
	23170, 24279, 25329, 26319, 27245, 28105, 28898, 29621,
	30273, 30852, 31356, 31785, 32137, 32412, 32609, 32728,
	32767, 32728, 32609, 32412, 32137, 31785, 31356, 30852,
	30273, 29621, 28898, 28105, 27245, 26319, 25329, 24279,
	23170, 22005, 20787, 19519, 18204, 16846, 15446, 14010,
	12539, 11039, 9512, 7962, 6393, 4808, 3212, 1608,
};

static short sin_lookup(unsigned int phase) {
	/* phase is 0..255 (0 = 0, 64 = 90, 128 = 180, 192 = 270 degrees) */
	unsigned char idx = (unsigned char)(phase & 0x3Fu);
	unsigned char quad = (unsigned char)((phase >> 6) & 3u);
	switch (quad) {
	case 0:  return  sin_quad[idx];
	case 1:  return  sin_quad[63 - idx];
	case 2:  return -sin_quad[idx];
	default: return -sin_quad[63 - idx];
	}
}

int main(int argc, char *argv[]) {
	int freq = 440;
	int duration_ms = 1000;
	short *pcm_buf = 0;

	/* Parse arguments */
	if (argc >= 2) {
		int n = atoi(argv[1]);
		if (n >= 20 && n <= 20000) freq = n;
	}
	if (argc >= 3) {
		int n = atoi(argv[2]);
		if (n >= 10 && n <= 5000) duration_ms = n;
	}

	printf("pcmplay: %d Hz %d ms | %d Hz %d ch 16-bit\n",
	       freq, duration_ms, PCM_SAMPLE_RATE, 2);

	/* Allocate PCM buffer */
	int total_frames = (PCM_SAMPLE_RATE * duration_ms) / 1000;
	int total_bytes = total_frames * PCM_FRAME_SIZE;
	if (total_bytes > 256 * 1024) total_bytes = 256 * 1024;

	pcm_buf = (short *)malloc((unsigned long)total_bytes);
	if (!pcm_buf) {
		printf("pcmplay: out of memory\n");
		return 1;
	}

	/* Fill with sine wave (16-bit signed, interleaved stereo) */
	int frames = total_bytes / PCM_FRAME_SIZE;
	{
		int i;
		for (i = 0; i < frames; i++) {
			unsigned int phase = (unsigned int)(
				(unsigned long long)i * (unsigned long long)freq * 256ULL
				/ (unsigned long long)PCM_SAMPLE_RATE);
			short val = sin_lookup(phase);
			pcm_buf[i * 2]     = val;  /* left */
			pcm_buf[i * 2 + 1] = val;  /* right */
		}
	}

	/* Open /dev/dsp (OSS audio device) */
	int dsp_fd = open("/dev/dsp", O_WRONLY, 0);
	if (dsp_fd < 0) {
		printf("pcmplay: cannot open /dev/dsp (err=%d)\n", dsp_fd);
		printf("pcmplay: (%d frames generated in memory)\n", frames);
		/* Fallback: write bell to stdout */
		write(1, "\x07", 1);
		printf("\n");
		printf("pcmplay: (bell only - /dev/dsp not available)\n");
		free(pcm_buf);
		return 1;
	}

	/* Configure OSS parameters via ioctl */
	{
		int fmt = 16;   /* AFMT_S16_LE */
		int ch  = 2;    /* stereo */
		int rate = PCM_SAMPLE_RATE;
		int frag = 0x000A000A;  /* 2^10 frags of 2^10 bytes */

		ioctl(dsp_fd, 0x00500005, &fmt);   /* SNDCTL_DSP_SETFMT */
		ioctl(dsp_fd, 0x00500006, &ch);    /* SNDCTL_DSP_CHANNELS */
		ioctl(dsp_fd, 0x00500002, &rate);  /* SNDCTL_DSP_SPEED */
		ioctl(dsp_fd, 0x0050000A, &frag);  /* SNDCTL_DSP_SETFRAGMENT */
	}

	printf("pcmplay: writing %d bytes to /dev/dsp ...\n", total_bytes);

	/* Write PCM data to /dev/dsp */
	{
		int written = write(dsp_fd, pcm_buf, (unsigned long)total_bytes);
		if (written < 0) {
			printf("pcmplay: write failed (err=%d)\n", written);
		} else {
			printf("pcmplay: wrote %d bytes\n", written);
		}
	}

	close(dsp_fd);
	free(pcm_buf);
	printf("pcmplay: done\n");
	return 0;
}
