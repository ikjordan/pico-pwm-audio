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
#include "wave.h"

#define READ_BUFFER 4096
#define DATA_BUFFER 2000

unsigned char read_buffer[READ_BUFFER];
int16_t* read_buffer_16 = (int16_t*)read_buffer;
int32_t* read_buffer_32 = (int32_t*)read_buffer;

uint16_t data_buffer[2][DATA_BUFFER];

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

    // calculate no.of samples
    uint num_samples = wf->data_size / (wf->channels * wf->bits_per_sample / 8);
    STATUS(("Number of samples: %u\n", num_samples));

    // calculate duration of file
#ifdef DEBUG_STATUS    
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
    // 1) Size of the holding buffer
    // 2) Remaining space in destimnation buffer
    // 3) Data in file, before wrap

    int size_of_each_sample = wf->channels * wf->bits_per_sample / 8;
    STATUS(("Size of each sample: %d bytes\n", size_of_each_sample));

    uint32_t samples_left = len / wf->channels;
    uint32_t samples_to_read;
    uint32_t max_samples_to_read = READ_BUFFER / size_of_each_sample;
    uint32_t total_read = 0;
    uint32_t data_index = 0;
    uint read;

    // How many samples will fill in the read buffer?
    while (samples_left)
    {
        samples_to_read = (max_samples_to_read < samples_left) ? max_samples_to_read : samples_left;

        FRESULT fr = f_read(wf->fil, read_buffer, samples_to_read * size_of_each_sample, &read);
        if (FR_OK != fr || read != (size_of_each_sample * samples_to_read))
        {
            printf("Read: %i Expected: %i\n", read, size_of_each_sample * samples_to_read);
            printf("Error in f_read of sample %i \n", read);
            return false;
        }
        // Write the samples - store as 16 bit unsigned values - will also bound here
        switch (wf->bits_per_sample)
        {
            case 8:
                for (int i = 0; i < samples_to_read; ++i)
                {
                    data_buffer[0][data_index] = ((uint16_t)(read_buffer[i])) << 4;
                    data_buffer[1][data_index] = (wf->channels == 2) ? ((uint16_t)(read_buffer[i])) << 4: data_buffer[0][data_index];
                    data_index = (data_index < DATA_BUFFER - 1) ? data_index + 1 : 0;
                }
            break;

            case 16:
                for (int i = 0; i < samples_to_read; ++i)
                {
                    data_buffer[0][data_index] = (read_buffer_16[i] + 0x8000) >> 4;
                    data_buffer[1][data_index] = (wf->channels == 2) ? ((read_buffer_16[i] + 0x8000) >> 4): data_buffer[0][data_index];
                    data_index = (data_index < DATA_BUFFER - 1) ? data_index + 1 : 0;
                }
            break;

            case 32:
            {
                uint32_t temp_l;
                uint32_t temp_r;
                for (int i = 0; i < samples_to_read; ++i)
                {
                    temp_l = (read_buffer_32[i] + 0x80000000) >> 20;
                    temp_r = (wf->channels == 2) ? ((read_buffer_32[i] + 0x80000000) >> 20): temp_l;
                    data_buffer[0][data_index] = (uint16_t)temp_l;
                    data_buffer[1][data_index] = (uint16_t)temp_r;
                    data_index = (data_index < DATA_BUFFER - 1) ? data_index + 1 : 0;
                }
            }
            break;
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