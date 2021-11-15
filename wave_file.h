#pragma once

// WAVE file header format
typedef struct wave_file 
{
	FIL      fil;							// FatFS file block
	bool     init;							// True if initialised
	char*    working;						// Pointer to working buffer
	uint32_t working_len;					// Size of workimng buffer in bytes
	uint32_t sample_rate;					// sampling rate (blocks per second)
	uint32_t data_size;						// NumSamples * NumChannels * BitsPerSample/8 - number of bytes of data
	uint32_t sample_size;                   // wf->channels * wf->bits_per_sample / 8
	uint32_t data_offset;                   // Offset of start of data
	uint32_t current_pos;					// Current read position in data, offest from start of data chunk
	uint16_t channels;						// no.of channels
	uint16_t bits_per_sample;				// bits per sample, 8- 8bits, 16- 16 bits etc

} wave_file;

// Function decalarations
extern bool waveFileCreate(wave_file* wf, const char* filename, unsigned char* working, uint32_t len);
extern void waveFileClose(wave_file* wf);

// Populate destination from the circular buffer
extern bool waveFileRead(wave_file* wf, uint16_t* dest, uint32_t len);

// inline functions
inline uint32_t getSampleRate(wave_file* wf){return wf->sample_rate;}
inline bool isStereo(wave_file* wf){return (wf->channels == 2);}

