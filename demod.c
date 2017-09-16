#include <stdint.h>
#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <math.h>
#include <string.h>
#include <ctype.h>
#include <getopt.h>
#include <pthread.h>

#include <rtl-sdr.h>

#include "hex.h"
#include "protocol.h"
#include "filter.h"
#include "runningavg.h"

void handlePacket(unsigned char* packet)
{
	if (isValidPacket(packet) == -1)
	{
		fprintf(stderr, "Invalid packet\n");
		return;
	}

	unsigned char hexString[21];

	hexify(hexString, packet, 10);

	fprintf(stderr, "Valid packet received\n");
	fprintf(stderr, " * Code: %s\n", hexString);
	fprintf(stderr, " * Command: %s\n", commandName(getCommand(packet)));
	fprintf(stderr, " * Rolling code: %d\n", getCode(packet));

	FILE* f = fopen("latestcode.txt", "w");
	fprintf(f, "%s\n", hexString);
	fclose(f);

	f = fopen("receivedcodes.txt", "a");
	fprintf(f, "%s\n", hexString);
	fclose(f);
}

#define DEFAULT_BUF_LENGTH              (16 * 16384)

typedef struct
{
	float i;
	float q;
} __attribute__((packed)) ComplexSample;

typedef struct
{
	int fd;
	rtlsdr_dev_t *dev;
} Context;

void rtlsdr_callback(unsigned char *buf, uint32_t len, void *c)
{
	Context* ctx = (Context*) c;

	if (write(ctx->fd, buf, len) == -1)
	{
		perror("rtlsdr_callback write():");
		exit(1);
	}
}

void *startSampler(void *c)
{
	Context* ctx = (Context*) c;

	int ret = rtlsdr_read_async(ctx->dev, rtlsdr_callback, c, 0, DEFAULT_BUF_LENGTH);
	if (ret == -1)
	{
		fprintf(stderr, "rtlsdr_read_async returned with %d\n", ret);
		exit(1);
	}

	return NULL;
}

void printHelp(char *s)
{
	fprintf(stderr, "usage %s [options]\n", s);
	fprintf(stderr, "\t-h, --help                   : this\n");
	fprintf(stderr, "\t-p, --ppm <ppm>              : set the frequency correction\n");
	fprintf(stderr, "\t-a, --agc                    : enable autogain\n");
	fprintf(stderr, "\t-t, --tunergain <gain value> : set rtlsdr_set_tuner_gain() (defaults to 8.7)\n");
	fprintf(stderr, "\t-d, --debug <int>            : debug stuff. Look in the code if you're interested\n");
}

int main(int argc, char **argv)
{
	uint32_t sampleRate = 1000000;
	double freq = 433.92e+6;
	uint16_t buf[DEFAULT_BUF_LENGTH / sizeof(uint16_t)];
        int samplesPerBit = 40; // XXX: this corresponds to 1000 baud, which is a little too high, but works fine since we can re-align at every zero-crossing
	int maxTimeError = 4; // XXX: This needs optimization
	int historyLen = samplesPerBit * 8; // XXX: Shoule be significantly longer than 2 bits, significantly shorted than the pre-amble

	int ppm = 0;
	int agc = 0;
	int tunergain = 87;		// XXX: Pulled from GNURadio source. Dunno what it means
	rtlsdr_dev_t *dev = NULL;
	unsigned int dev_index = 0;
	int sampleAt = 0.0;
	int state = 0;
	int lastCrossing = 0;
	int manchPos = 0;
	int bitpos = 0;
	int n;
	int sampleNum = 0;
	int lastBit = 0;
	int preambleGood = 0;
	int minPreambleBits = 42;
	unsigned char byte;
	unsigned char packet[10];
	int pos = 0;
	int lastSampledBit;
	int decimation = 25;
	int decimator = 0;
	int debug = 0;
	int c;

	/* Parse options */
	while (1) {
		int option_index = 0;
		static struct option long_options[] = {
			{"ppm",        required_argument, 0,  'p' },
			{"tunergain",  required_argument, 0,  't' },
			{"agc",        no_argument,       0,  'a' },
			{"help",       no_argument,       0,  'h' },
			{"debug",      required_argument, 0,  'd' },
			{0,            0,                 0,  0 }
		};

		c = getopt_long(argc, argv, "p:t:ahd:",
				long_options, &option_index);
		if (c == -1)
			break;

		switch (c) {
			default:
			case 'h':
			case 0:
				printHelp(argv[0]);
				return c == 'h' ? 0 : -1;

			case 'p':
				ppm = atoi(optarg);
				break;

			case 't':
				tunergain = (int)(10.0 * atof(optarg));
				break;

			case 'a':
				agc = 1;
				break;

			case 'd':
				debug = atoi(optarg);
				break;
		}
	}

	/* setup filter */
	SampleFilter filteri;
	SampleFilter filterq;
	SampleFilter_init(&filteri);
	SampleFilter_init(&filterq);

	/* Build look-up table for float conversion */
	ComplexSample lut[0x10000];;
	for (unsigned int i = 0; i < 0x10000; i++)
	{
#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
		lut[i].i = (((float)(i & 0xff)) - 127.4f) * (1.0f/128.0f);
		lut[i].q = (((float)(i >> 8)) - 127.4f) * (1.0f/128.0f);
#else // BIG_ENDIAN
		lut[i].i = (((float)(i >> 8)) - 127.4f) * (1.0f/128.0f);
		lut[i].q = (((float)(i & 0xff) - 127.4f) * (1.0f/128.0f);
#endif
	}

	/* set up running average to get ac signal */
	runningAvgContext avgCtx;
	runningAvgInit(&avgCtx, historyLen);

	/* Open the rtlsdr */
	if (rtlsdr_open(&dev, dev_index) == -1)
	{
		fprintf(stderr, "Can't open device");
		return -1;
	}

	/* Set rtl-sdr parameters */
	if (agc)
	{
		rtlsdr_set_tuner_gain_mode(dev, 0);
		rtlsdr_set_agc_mode(dev, 1);
	}
	else
	{
		rtlsdr_set_tuner_gain_mode(dev, 1);
		rtlsdr_set_agc_mode(dev, 0);
		if (rtlsdr_set_tuner_gain(dev, tunergain) == -1)
		{
			fprintf(stderr, "Can not set gain\n");
		}
	}
	rtlsdr_set_sample_rate(dev, sampleRate);
	rtlsdr_set_center_freq(dev, (uint32_t) freq);
	rtlsdr_reset_buffer(dev);
	rtlsdr_set_freq_correction(dev, ppm);

	Context ctx;
	int pipefd[2];
	if (debug != 2)
	{
		/* create our data pipe */
		if (pipe(pipefd) == -1)
		{
			perror("pipe()");
			return -1;
		}
		ctx.fd = pipefd[1];
		ctx.dev = dev;

		pthread_t sampleThread;
		if (pthread_create(&sampleThread, NULL, startSampler, &ctx) != 0)
		{
			perror("pthread_create()");
			return -1;
		}
	}

	if (debug == 2) /* Don't capture, just read from stdin */
	{
		pipefd[0] = 0;
	}

	ComplexSample* debugout = NULL;
	if (debug == 3 || debug == 1)
	{
		debugout = malloc(DEFAULT_BUF_LENGTH / sizeof(uint16_t) * sizeof(ComplexSample));
	}
	int nDebugout = 0;

	int bit = 0;
	while ((n = read(pipefd[0], buf, sizeof(buf))) > 0)
	{
		int nout = n / 2;
		for (int i = 0 ; i < nout; i++)
		{
			ComplexSample* cSample = &lut[buf[i]];

			if (debug == 1)
			{
				debugout[nDebugout].i = cSample->i;
				debugout[nDebugout].q = cSample->q;
				nDebugout++;
			}

			SampleFilter_put(&filteri, cSample->i);
			SampleFilter_put(&filterq, cSample->q);

			if (decimation > ++decimator)
			{
				continue;
			}
			decimator = 0;

			double si = SampleFilter_get(&filteri);
			double sq = SampleFilter_get(&filterq);

			if (debug == 3)
			{
				debugout[nDebugout].i = si;
				debugout[nDebugout].q = sq;
				nDebugout++;
			}

			double sample = si*si + sq*sq; // Proportional to receive power

			/* Get an average value to compare against */
			double avg = runningAvg(&avgCtx, sample);

			/* Hysteresis XXX: These values might need optimizing */
			if (sample > avg * 1.00) bit = 1;
			if (sample < avg * 0.25) bit = 0;

			/* Check for zero-crossing */
			int zeroCrossing = (lastBit != bit);
			int sinceLastCrossing = sampleNum - lastCrossing;

			if (zeroCrossing)
			{
				// Align the sampleAt variable here
				sampleAt = sampleNum + samplesPerBit / 2.0;
			}

			/* Check if this is the end of the preamble */
			if (preambleGood >= minPreambleBits)
			{
				/* Pre-amble is followed by 4 low 'bits' */
				if (abs(sinceLastCrossing / 4 - samplesPerBit) < maxTimeError)
				{
					state = 1;
					preambleGood = 0;

					/* Start sampling halfway during the next bit */
					sampleAt = sampleNum + samplesPerBit / 2.0;
				}
			}

			/* Demodulation state-machine */
			switch(state)
			{
				case 0: // wait for pre-amble
					if (zeroCrossing)
					{ 
						if (!preambleGood) /* See if this is the start of a preamble */
						{
							preambleGood++;
						}
						else
						{
							int err = abs(sinceLastCrossing - samplesPerBit);;
							if (err > maxTimeError)
							{
								preambleGood = 0;
							}
							else
							{
								preambleGood++;
							}
						}
					}
					break;
				case 1: // prepare for bitbang
					fprintf(stderr, "Got a pre-amble!\n");
					manchPos = 0;
					state = 2;
					bitpos = 0;
					pos = 0;
					byte = 0;
					// fall through
				case 2: // bitbang
					if (sampleNum >= sampleAt)
					{
						/* Time the next sample */
						sampleAt += samplesPerBit;

						if (manchPos & 1)
						{
							/* Check for manchester encoding errors */
							if (bit == lastSampledBit)
							{
//								fprintf(stderr, "Manchester encoding error: %d and %d at manchPos %d\n", bit, lastSampledBit, manchPos);
								/* Reset state */
								state = 0;
							}
						}
						else
						{
							/* This is a data bit */
							byte <<= 1;
							byte |= bit;

							bitpos++;

							if (bitpos == 8)
							{
								packet[pos++] = byte;
								byte = 0;
								bitpos = 0;

								if (pos == sizeof(packet)) // We have a full packet! */
								{
									handlePacket(packet);
									state = 0;
								}
							}
						}

						manchPos++;
						lastSampledBit = bit;
					}

					break;
			}

			if (zeroCrossing)
			{
				lastCrossing = sampleNum;
			}

			lastBit = bit;
			sampleNum++;
		}

		if (debug == 3 || debug == 1)
		{
			if (write(1, debugout, nDebugout * sizeof(ComplexSample)) == -1)
			{
				perror("write()");
				return -1;
			}

			nDebugout = 0;
		}
	}

	return 0;
}

