#include <assert.h>
#include <stddef.h>
#include <stdio.h>
#include <stdbool.h>
#include <stdlib.h>
#include <inttypes.h>
#include <unistd.h>
#include <osmo-fl2k.h>
#include <math.h>
#include <errno.h>
#include <error.h>
#include <locale.h>
#include <limits.h>
#include <string.h>

enum mode_e {
	AMPLIPHASE_MODE,
	IQ_MODE,
};

static fl2k_dev_t *dev = NULL;
static uint32_t samp_rate = 100000000;
static bool do_exit = false;
static uint8_t *txbuf_r = NULL;
static uint8_t *txbuf_g = NULL;
//static double target_frequency = 1000000;
//static double period_samples;
//static unsigned channel = offsetof(fl2k_data_info_t, r_buf);
static double carrier_frequency = 1000000;
static unsigned samples_per_carrier_period;
static unsigned samples_per_carrier_halfperiod;
static unsigned input_sample_rate = 48000;
static enum mode_e modulation_mode;
static int16_t *audio_samples = NULL;
static FILE *input_file;

// generates square signal starting from 1, returns carrier offset to continue at
static unsigned generate_carrier(uint8_t *target_buf, size_t len)
{
	size_t offset = 0;
	while (1) {
		unsigned samples_ones;
		if (offset + samples_per_carrier_halfperiod >= len) {
			samples_ones = len - offset;
		} else {
			samples_ones = samples_per_carrier_halfperiod;
		}
		memset(target_buf, 0xff, samples_ones);
		if (samples_ones < samples_per_carrier_halfperiod) {
			return samples_ones;
		}
		offset += samples_ones;

		unsigned samples_zeroes;
		if (offset + samples_per_carrier_halfperiod >= len) {
			samples_zeroes = len - offset;
		} else {
			samples_zeroes = samples_per_carrier_halfperiod;
		}
		memset(target_buf, 0, samples_zeroes);
		if (samples_zeroes < samples_per_carrier_halfperiod) {
			return samples_zeroes;
		}
		offset += samples_zeroes;
	}
	return offset % samples_per_carrier_period;
}

// static_shift is specified in 45°'s
// sample produces a 45° shift at 2^15
static void generate_shifted_carrier(uint8_t *target_buf, unsigned carrier_period_samples, double static_shift, int32_t sample, unsigned *carrier_offset)
{
	double phase_shift = (double)sample / 0x8000; // 1 = 45°, so sample ranging from -2^15 to 2^15 gives 90° phase shift range
	double samples_shift = (samples_per_carrier_period / 8) * (static_shift + phase_shift) + *carrier_offset;
	unsigned buf_offset = 0;
	if (samples_shift > 0) {
		// Generate 0's of the previous period
		unsigned samples_zeroes_prev = lround(samples_shift);
		memset(target_buf, 0xff, samples_zeroes_prev);
		buf_offset += samples_zeroes_prev;
	} else {
		// Generate the remaining 1's and 0's of the current period
		unsigned samples_ones = lround(-samples_shift);
		memset(target_buf, 0, samples_ones);
		buf_offset += samples_ones;
		memset(target_buf + buf_offset, 0xff, samples_per_carrier_halfperiod);
		buf_offset += samples_per_carrier_halfperiod;
	}
	unsigned remaining_carrier_period_samples = carrier_period_samples - buf_offset;
	*carrier_offset = generate_carrier(target_buf + buf_offset, remaining_carrier_period_samples);
}

// How many samples of the carrier do we need to produce before it's time to phase-shift for a new input sample
// Must be called once per input sample
static unsigned samples_until_next_input()
{
	static unsigned accumulator = 0;
	unsigned samples_per_input_sample_int = samp_rate / input_sample_rate;
	accumulator += samp_rate % input_sample_rate;
	samples_per_input_sample_int += accumulator / input_sample_rate;
	accumulator %= input_sample_rate;
	return samples_per_input_sample_int;
}

static unsigned input_samples_filling_buffer(unsigned len)

{
	return ceil((double)len / samp_rate * input_sample_rate);
}

static void fl2k_callback(fl2k_data_info_t *data_info)
{
	if (data_info->device_error) {
		fprintf(stderr, "Device error, exiting.\n");
		do_exit = 1;
		return;
	}

	data_info->sampletype_signed = 0;

	data_info->r_buf = (char *)txbuf_r;
	data_info->g_buf = (char *)txbuf_g;
	data_info->b_buf = NULL;
	static unsigned buffer_phase_shift = 0;	// number of samples remaining to be put for the stored input after FL2K_BUF_LEN
	size_t buf_offset;

	// To handle input overlapping buffer boundaries:
	// Produce up to one input sample more than would fit in one FL2K_BUF_LEN
	// At the next iteration copy the tail of the buffer (starting at FL2K_BUF_LEN) to the beginning of the new buffer
	// And start the next input sample number calculation and buffer filling at an offset

	unsigned samples = input_samples_filling_buffer(FL2K_BUF_LEN - buffer_phase_shift);
	size_t read_samples = fread(audio_samples, modulation_mode == IQ_MODE ? 4 : 2, samples, input_file);
	if (read_samples == 0) {
		if (ferror(input_file)) {
			fprintf(stderr, "Couldn't read samples");
		} else {
			puts("Out of input samples, exiting...");
		}
		do_exit = true;
		fl2k_stop_tx(dev);
		return;
	}
	memcpy(txbuf_r, txbuf_r + FL2K_BUF_LEN, buffer_phase_shift);
	memcpy(txbuf_g, txbuf_g + FL2K_BUF_LEN, buffer_phase_shift);
	buf_offset = buffer_phase_shift;
	for (int i = 0; i < read_samples; ++i) {
		static unsigned carrier_offset[2] = { 0 };
		unsigned samples_per_input_sample = samples_until_next_input();
		switch (modulation_mode) {
		case AMPLIPHASE_MODE:
			int sample = audio_samples[i];
			generate_shifted_carrier(txbuf_r + buf_offset, samples_per_input_sample, 1, sample, &carrier_offset[0]);
			generate_shifted_carrier(txbuf_g + buf_offset, samples_per_input_sample, -1, -sample, &carrier_offset[1]);
			break;
		case IQ_MODE:
			// stereo input, every sample encodes phase shift of the corresponding channel in -180° - 180° range
			generate_shifted_carrier(txbuf_r + buf_offset, samples_per_input_sample, 0, 4 * audio_samples[2 * i], &carrier_offset[0]);
			generate_shifted_carrier(txbuf_g + buf_offset, samples_per_input_sample, 0, 4 * audio_samples[2 * i + 1], &carrier_offset[1]);
			break;
		}
		buf_offset += samples_per_input_sample;
	}
	buffer_phase_shift = buf_offset - FL2K_BUF_LEN;

	if (do_exit) {
		fl2k_stop_tx(dev);
	}
}

int main(int argc, char *argv[])
{
	setlocale(LC_NUMERIC, "en_US");	// force some grouping characters

	// allocate more RAM than needed to allow for trailing samples
	txbuf_r = malloc(2 * FL2K_BUF_LEN);
	if (!txbuf_r) {
		fprintf(stderr, "malloc error!\n");
		goto out;
	}
	txbuf_g = malloc(2 * FL2K_BUF_LEN);
	if (!txbuf_g) {
		fprintf(stderr, "malloc error!\n");
		goto out;
	}

	uint32_t dev_index = 0;
	fl2k_open(&dev, dev_index);
	if (NULL == dev) {
		fprintf(stderr, "Failed to open fl2k device #%d.\n", dev_index);
		goto out;
	}

	// future-proof
	//int r = fl2k_set_mode(dev, FL2K_MODE_MULTICHAN);
	//if (r < 0) {
	//	fprintf(stderr, "WARNING: Failed to set multichannel mode\n");
	//	return 0;
	//}
	//fl2k_set_enabled_channels(dev, R_EN | G_EN);

	input_file = stdin;
	int c;
	while ((c = getopt(argc, argv, "f:hiS:s:")) != -1) {
		switch (c) {
		case 'f':
			if (optarg) {
				input_file = fopen(optarg, "rb");
				if (!input_file) {
					perror("Couldn't open the input file");
					return 1;
				}
			}
			break;
		case '?':
		case 'h':
			fprintf(stderr, "Usage: %s [-f <file name>] [-i] [-s <fl2k sample rate>] [-S <input sample rate>]\n", argv[0]);
			goto out;
			break;
		case 'i':
			modulation_mode = IQ_MODE;
			break;
		case 's':
			if (optarg) {
				if (sscanf(optarg, "%u", &samp_rate) < 1) {
					fprintf(stderr, "Couldn't read the desired fl2k sample rate!\n");
					goto out;
				}
			}
			break;
		case 'S':
			if (optarg) {
				if (sscanf(optarg, "%u", &input_sample_rate) < 1) {
					fprintf(stderr, "Couldn't read the desired input sample rate!\n");
					goto out;
				}
			}
			break;
		}
	}

	/* Set the sample rate */
	int r = fl2k_set_sample_rate(dev, samp_rate);
	if (r < 0) {
		fprintf(stderr, "WARNING: Failed to set sample rate %d.\n", samp_rate);
	}
	samp_rate = fl2k_get_sample_rate(dev); // we might get offered a different sample rate due to PLL limitations
	double samples_per_carrier_period_wanted = (double)samp_rate / carrier_frequency;
	samples_per_carrier_halfperiod = lround(samples_per_carrier_period_wanted / 2); // every half-period must have an equal integer length
	samples_per_carrier_period = samples_per_carrier_halfperiod * 2;

	if (samples_per_carrier_period_wanted != samples_per_carrier_period) {
		// inexact freuqency match
		double carrier_frequency_new = (double)samp_rate / samples_per_carrier_period;
		fprintf(stderr, "WARNING: Failed to obtain exact carrier frequency: reuested %lfHz (%lf samples), obtained: %lfHz (%u samples)\n", carrier_frequency, samples_per_carrier_period_wanted, carrier_frequency_new, samples_per_carrier_period);
		carrier_frequency = carrier_frequency_new;
	}

	audio_samples = malloc(2 * ((double)FL2K_BUF_LEN / samp_rate) * input_sample_rate + 1);
	if (!audio_samples) {
		fprintf(stderr, "malloc error!\n");
		goto out;
	}


	r = fl2k_start_tx(dev, fl2k_callback, NULL, 0);
	if (r < 0) {
		fprintf(stderr, "Couldn't start the transmission.\n");
	}

	while (!do_exit) {
		sleep(1);
	}
	fl2k_close(dev);

out:
	// assume no need to free() anything

	return 0;
}
