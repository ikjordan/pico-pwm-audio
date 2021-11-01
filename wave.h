// WAVE file header format
struct HEADER {
	uint16_t channels;						// no.of channels
	uint32_t sample_rate;					// sampling rate (blocks per second)
	uint16_t bits_per_sample;				// bits per sample, 8- 8bits, 16- 16 bits etc
	uint32_t data_size;						// NumSamples * NumChannels * BitsPerSample/8 - size of the next chunk that will be read
};