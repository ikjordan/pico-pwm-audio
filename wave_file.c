/**
 * Read and parse a wave file
 *
 **/
#include <stdio.h>
#include <string.h>
#include "f_util.h"
#include "ff.h"
#include "pico/stdlib.h"
#include <stdbool.h>
#include "wave_file.h"

#define CACHE_BUFFER 4096

unsigned char cache_buffer[CACHE_BUFFER];
int16_t* cache_buffer_16 = (int16_t*)cache_buffer;
int32_t* cache_buffer_32 = (int32_t*)cache_buffer;

// Function declarations
static bool file_read(FIL* fil, void* buffer, uint size, const char* msg);
static bool waveFileCheck(wave_file* wf);

#define DEBUG_STATUS

// Extra debug
#ifdef DEBUG_STATUS
  #define STATUS(a) printf a
#else
  #define STATUS(a) (void)0
#endif

bool waveFileCreate(wave_file* wf, FIL* fil, const char* filename)
{
    // Clear the file pointer
    wf->fil = NULL;

    // open file
    STATUS(("\nOpening  file: %s\n", filename));
    FRESULT fr = f_open(fil, filename, FA_OPEN_EXISTING | FA_READ);

    if (FR_OK != fr)
    {
        printf("Error opening file\n");
        return false;
    }
 
    // Store the file handle
    wf->fil = fil;

    // Complete thr rest of the validation
    return waveFileCheck(wf);
}

void waveFileClose(wave_file* wf)
{
    //Close the file, if have a valid handle
    if (wf->fil != NULL)
    {
        STATUS(("Closing file..\n"));
        FRESULT fr = f_close(wf->fil);

        if (FR_OK != fr) 
        {
            printf("f_close error: %s (%d)\n", FRESULT_str(fr), fr);
        }
        wf->fil = NULL;
    }
}

static bool waveFileCheck(wave_file* wf)
{
    uint32_t val32;
    uint16_t val16;
    int read;
    
    // http://soundfile.sapp.org/doc/WaveFormat/

    if (wf->fil == NULL)
    {
        printf("Invalid file handle\n");
        return false;
    }

    // ChunkID
    if (!file_read(wf->fil, &val32, 4, "ChunkID")) return false;
    STATUS(("(0-3)   Chunk ID: %c%c%c%c\n", ((char *)&val32)[0], ((char *)&val32)[1], ((char *)&val32)[2], ((char *)&val32)[3]));
    
    // Check for "RIFF"
    if (val32 != 0x46464952)
    {
        printf("Not RIFF file: %c%c%c%c\n", ((char *)&val32)[0], ((char *)&val32)[1], ((char *)&val32)[2], ((char *)&val32)[3]);
        return false;
    }

    // ChunkSize
    if (!file_read(wf->fil, &val32, sizeof(val32), "ChunkSize")) return false;
    STATUS(("(4-7)   ChunkSize: bytes: %u, Kb: %u\n", val32, val32/1024));

    // Format
    if (!file_read(wf->fil, &val32, 4, "Format")) return false;    
    STATUS(("(8-11)  Format: %c%c%c%c\n", ((char *)&val32)[0], ((char *)&val32)[1], ((char *)&val32)[2], ((char *)&val32)[3]));
    
    // Check for "WAVE"
    if (val32 != 0x45564157)
    {
        printf("Not WAV file: %c%c%c%c\n", ((char *)&val32)[0], ((char *)&val32)[1], ((char *)&val32)[2], ((char *)&val32)[3]);
        return false;
    }

    // Subchunk1ID
    if (!file_read(wf->fil, &val32, 4, "Subchunk1ID")) return false;    
    STATUS(("(12-15) Fmt marker: %c%c%c%c\n", ((char *)&val32)[0], ((char *)&val32)[1], ((char *)&val32)[2], ((char *)&val32)[3]));
    
    // Check for "fmt "
    if (val32 != 0x20746d66)
    {
        printf("Unknown Subchunk1 format: %c%c%c%c\n", ((char *)&val32)[0], ((char *)&val32)[1], ((char *)&val32)[2], ((char *)&val32)[3]);
        return false;
    }

    // Subchunk1Size
    if (!file_read(wf->fil, &val32, sizeof(val32), "Subchunk1Size")) return false;    
    STATUS(("(16-19) Subchunk1Size: %u\n", val32));
    
    if (val32 != 16)
    {
        printf("Unexpected Subchunk1Size: %u\n", val32);
        return false;
    }

    // AudioFormat
    if (!file_read(wf->fil, &val16, sizeof(val16),"AudioFormat")) return false;    

#ifdef DEBUG_STATUS   
    char format_name[10] = "";

    if (val16 == 1)
        strcpy(format_name,"PCM"); 
    else if (val16 == 6)
        strcpy(format_name, "A-law");
    else if (val16 == 7)
        strcpy(format_name, "Mu-law");

    printf("(20-21) Format type: %u %s\n", val16, format_name);
#endif    
    if (val16 != 1)
    {
        printf("Unsupported audio format: %u\n", val16);
        return false;
    }

    // NumChannels
    if (!file_read(wf->fil, &wf->channels, sizeof(wf->channels), "NumChannels")) return false;    
    STATUS(("(22-23) Channels: %u\n", wf->channels));
    
    if (wf->channels > 2 || wf->channels < 1)
    {
        printf("Unsupported number of channels: %u\n", wf->channels);
        return false;
    }

    // SampleRate
    if (!file_read(wf->fil, &wf->sample_rate, sizeof(wf->sample_rate), "SampleRate")) return false;    
    STATUS(("(24-27) Sample rate: %u\n", wf->sample_rate));
    
    if (wf->sample_rate < 8000 || wf->sample_rate > 44100)
    {
        printf("Unsupported sample rate: %u\n", wf->channels);
        return false;
    }

    // Byte rate
    if (!file_read(wf->fil, &val32, sizeof(val32), "Byte rate")) return false;    
    STATUS(("(28-31) Byte Rate: %u\n", val32));

    // Block align
    if (!file_read(wf->fil, &val16, sizeof(val16), "Block align")) return false;    
    STATUS(("(32-33) Block Alignment: %u\n", val16));

    // Bits per sample
    if (!file_read(wf->fil, &wf->bits_per_sample, sizeof(wf->bits_per_sample), "Bits per sample")) return false;    
    STATUS(("(34-35) Bits per sample: %u\n", wf->bits_per_sample));

    if (wf->bits_per_sample != 8 && wf->bits_per_sample != 16 && wf->bits_per_sample != 32)
    {
        printf("unsupported bits per sample: %u\n", wf->bits_per_sample);
        return false;
    }

    // Subchunk2ID
    file_read(wf->fil, &val32, 4, "Subchunk2ID");    
    STATUS(("(36-39) Data marker: %c%c%c%c\n", ((char *)&val32)[0], ((char *)&val32)[1], ((char *)&val32)[2], ((char *)&val32)[3]));
    
    // Check for "data"
    if (val32 != 0x61746164)
    {
        printf("Unknown subchunk2 format: %c%c%c%c\n", ((char *)&val32)[0], ((char *)&val32)[1], ((char *)&val32)[2], ((char *)&val32)[3]);
        return false;
    }

    // Subchunk2Size
    if (!file_read(wf->fil, &wf->data_size, sizeof(wf->data_size), "Subchunk2Size")) return false;    
    STATUS(("(40-43) Subchunk2Size: %u\n", wf->data_size));
    
    wf->data_offset = 44;
    wf->current_pos = 0;
    wf->sample_size = wf->channels * wf->bits_per_sample / 8;

#ifdef DEBUG_STATUS    
    // calculate no.of samples
    uint num_samples = wf->data_size / (wf->channels * wf->bits_per_sample / 8);
    printf("Number of samples: %u\n", num_samples);

    // calculate duration of file
    float duration_in_seconds = (float) wf->data_size / (wf->channels * wf->sample_rate * wf->bits_per_sample / 8);
    printf("Duration in seconds = %f\n", duration_in_seconds);
#endif    

    return true;
}

// Fill the destination buffer - with left and right interleaved samples
// If the wave file is mono, the duplicate left and right channels
// Len is the number of 16 bit samples to copy to buffer
bool waveFileRead(wave_file* wf, uint16_t* dest, uint len)
{

    // Read data into holding buffer, then copy to destination buffer
    // until destination is full
    // When calculating the size of the read to issue it should be the smallest of:
    // 1) Remaining space in destination buffer
    // 2) Size of the cache buffer
    // 3) Data in file, before wrap
    // Sample = data for each supported channel (so 4 bytes for 16 bit stereo)

    uint32_t samples_left = len >> 1;         // Number of samples left to write to destination buffer
                                              // Note always write two samples (for l and r channels)
    uint32_t cache_samples_size = CACHE_BUFFER / wf->sample_size; // Number of samples that fit in read cache
    uint32_t data_index = 0;                           // Index into destination buffer
    uint32_t samples_to_read;                          // Number of samples to read from file next read instance
    uint32_t samples_to_wrap;                          // Samples left to read from file before reaching EOF
    uint read;
    
    // Take the smaller of the read buffer size and the destination size
    while (samples_left)
    {
        // Calculate the number of samples that can be read before a file wrap
        samples_to_wrap = (wf->data_size - wf->current_pos) / wf->sample_size;

        // The smaller of the number to fill the destination, or the holding buffer
        samples_to_read = (samples_left > cache_samples_size) ? cache_samples_size : samples_left;
        samples_to_read = (samples_to_read > samples_to_wrap) ? samples_to_wrap : samples_to_read;

        FRESULT fr = f_read(wf->fil, cache_buffer, samples_to_read * wf->sample_size, &read);
        if (FR_OK != fr || read != (wf->sample_size * samples_to_read))
        {
            printf("Read: %i Expected: %i\n", read, wf->sample_size * samples_to_read);
            printf("Error in f_read of sample %i \n", read);
            return false;
        }
        // Write the samples - store as 16 bit unsigned values - will also bound here
        switch (wf->bits_per_sample)
        {
            case 8:
                if (wf->channels == 2)
                {
                    for (int i = 0; i < samples_to_read; ++i)
                    {
                        dest[data_index++] = ((uint16_t)(cache_buffer[i<<1])) << 4;
                        dest[data_index++] = ((uint16_t)(cache_buffer[(i<<1)+1])) << 4;
                    }
                }
                else
                {
                    // Mono, so duplicate sample
                    for (int i = 0; i < samples_to_read; ++i)
                    {
                        dest[data_index] = ((uint16_t)(cache_buffer[i])) << 4;
                        dest[data_index+1] = dest[data_index];
                        data_index += 2;
                    }
                }
            break;

            case 16:
                if (wf->channels == 2)
                {
                    for (int i = 0; i < samples_to_read; ++i)
                    {
                        dest[data_index++] = (cache_buffer_16[i<<1] + 0x8000) >> 4;
                        dest[data_index++] =((cache_buffer_16[(i<<1)+1] + 0x8000) >> 4);
                    }
                }
                else
                {
                    // Mono, so duplicate sample
                    for (int i = 0; i < samples_to_read; ++i)
                    {
                        dest[data_index] = (cache_buffer_16[i] + 0x8000) >> 4;
                        dest[data_index+1] = dest[data_index];
                        data_index += 2;
                    }
                }
            break;

            case 32:
            {
                uint32_t temp_l;
                uint32_t temp_r;
                if (wf->channels == 2)
                {
                    for (int i = 0; i < samples_to_read; ++i)
                    {
                        temp_l = (cache_buffer_32[i<<1] + 0x80000000) >> 20;
                        temp_r = (cache_buffer_32[(i<<1)+1] + 0x80000000) >> 20;
                        dest[data_index++] = (uint16_t)temp_l;
                        dest[data_index++] = (uint16_t)temp_r;
                    }
                }
                else
                {
                    // Mono, so duplicate sample
                    for (int i = 0; i < samples_to_read; ++i)
                    {
                        temp_l = (cache_buffer_32[i] + 0x80000000) >> 20;;
                        dest[data_index++] = (uint16_t)temp_l;
                        dest[data_index++] = (uint16_t)temp_l;
                    }
                }
            }
            break;
        }
        // Update the current position in the file, and wrap if needed
        wf->current_pos += read;

        if (wf->current_pos == wf->data_size)
        {
            STATUS(("file wrap\n"));

            // seek back to start of data in file
            f_lseek(wf->fil, wf->data_offset);
            wf->current_pos = 0;
        }
        samples_left -= samples_to_read;
    }
}

static bool file_read(FIL* fil, void* buffer, uint size, const char* msg)
{
    uint read;
    bool ret = true;

    FRESULT fr = f_read(fil, buffer, size, &read);
    if (FR_OK != fr || read != size)
    {
        printf("Error in f_read %s \n", msg);
        ret = false;
    }
    return ret;
}