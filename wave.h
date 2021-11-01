#pragma once

// WAVE file header format
typedef struct wave_file 
{
	FIL*     fil;							// FatFS file block
	uint32_t sample_rate;					// sampling rate (blocks per second)
	uint32_t data_size;						// NumSamples * NumChannels * BitsPerSample/8 - number of bytes of data
	uint32_t data_offset;                   // Offset of start of data
	uint32_t current_pos;					// Current read position in data, offest from start of data chunk
	uint16_t channels;						// no.of channels
	uint16_t bits_per_sample;				// bits per sample, 8- 8bits, 16- 16 bits etc

} wave_file;

// Function decalarations
extern bool waveFileCreate(wave_file* wf, FIL* fil, const char* filename);
extern void waveFileClose(wave_file* wf);

// Populate destination from the circular buffer
extern bool waveFileRead(wave_file* wf, uint16_t* dest, uint len);

