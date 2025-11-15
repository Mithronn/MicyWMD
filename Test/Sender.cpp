/*++
    User-mode application to send audio data to MicyAudio driver

    This program demonstrates how to send audio data to the driver using
    IOCTL_MICYAUDIO_SET_AUDIO_DATA
--*/

// Ensure Unicode is enabled
#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>
#include <winioctl.h>

#include <devguid.h>
#include <initguid.h>

// Include the IOCTL definition - in a real scenario, you'd copy this from
// definitions.h IOCTL_MICYAUDIO_SET_AUDIO_DATA
#define METHOD_BUFFERED 0
#define FILE_ANY_ACCESS 0
#define MICY_IOCTL_TYPE 29

#define IOCTL_MICYAUDIO_SET_AUDIO_DATA                                         \
  CTL_CODE(MICY_IOCTL_TYPE, 0x902, METHOD_BUFFERED, FILE_ANY_ACCESS)

// Structure matching MICYAUDIO_SET_AUDIO_DATA from definitions.h
typedef struct _MICYAUDIO_SET_AUDIO_DATA {
  ULONG StreamId;    // Stream identifier (Pin ID) - typically 1 for capture
  ULONG DataSize;    // Size of audio data in bytes
  BYTE AudioData[1]; // Audio data buffer (variable length)
} MICYAUDIO_SET_AUDIO_DATA, *PMICYAUDIO_SET_AUDIO_DATA;

HANDLE FindAndOpenAudioDevice() {
  HANDLE hDevice =
      CreateFile(L"\\\\.\\MicyAudio", GENERIC_READ | GENERIC_WRITE, 0, NULL,
                 OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);

  if (hDevice == INVALID_HANDLE_VALUE) {
    printf("CreateFile failed for %S: %lu\n", L"\\\\.\\MicyAudio",
           GetLastError());
  }

  return hDevice;
}

// Function to open device handle
HANDLE OpenAudioDevice() {
  HANDLE hDevice = INVALID_HANDLE_VALUE;

  // Try different device paths - adjust based on your device setup
  // You can find the device path using SetupDiGetClassDevs and
  // SetupDiEnumDeviceInterfaces

  // Option 1: If you have a symbolic link, use it directly
  // hDevice = CreateFile(L"\\\\.\\MicyAudio", ...);

  // Option 2: Open through device interface (recommended)
  // You'll need to enumerate audio devices to find the right one
  // For now, we'll use a generic approach

  // Note: In a real application, you should:
  // 1. Enumerate audio devices using SetupDiGetClassDevs
  // 2. Find your specific device (MicyAudio)
  // 3. Get its device interface path
  // 4. Open that path

  hDevice = FindAndOpenAudioDevice();
  printf(
      "Note: You need to determine the correct device path for your setup.\n");
  printf("This is a template - adjust the device path as needed.\n");

  return hDevice;
}

// Function to generate sample audio data (sine wave)
void GenerateSineWave(BYTE *buffer, DWORD bufferSize, DWORD sampleRate,
                      WORD channels, WORD bitsPerSample) {
  static double phase = 0.0;
  double frequency = 440.0; // A4 note
  double amplitude = 0.5;

  if (bitsPerSample == 16 && channels == 2) // 16-bit stereo
  {
    short *samples = (short *)buffer;
    DWORD sampleCount = bufferSize / (channels * sizeof(short));

    for (DWORD i = 0; i < sampleCount; i++) {
      double sample = amplitude * sin(phase * 2.0 * 3.14159265359);
      short sampleValue = (short)(sample * 32767.0);

      // Left channel
      samples[i * 2] = sampleValue;
      // Right channel
      samples[i * 2 + 1] = sampleValue;

      phase += frequency / sampleRate;
      if (phase >= 1.0)
        phase -= 1.0;
    }
  } else if (bitsPerSample == 16 && channels == 1) // 16-bit mono
  {
    short *samples = (short *)buffer;
    DWORD sampleCount = bufferSize / sizeof(short);

    for (DWORD i = 0; i < sampleCount; i++) {
      double sample = amplitude * sin(phase * 2.0 * 3.14159265359);
      samples[i] = (short)(sample * 32767.0);

      phase += frequency / sampleRate;
      if (phase >= 1.0)
        phase -= 1.0;
    }
  }
}

// Function to send audio data to driver
BOOL SendAudioDataToDriver(HANDLE hDevice, ULONG streamId, BYTE *audioData,
                           ULONG dataSize) {
  if (hDevice == INVALID_HANDLE_VALUE || audioData == NULL || dataSize == 0) {
    printf("Invalid parameters\n");
    return FALSE;
  }

  // Allocate buffer for IOCTL (structure + audio data)
  DWORD bufferSize = sizeof(MICYAUDIO_SET_AUDIO_DATA) - 1 + dataSize;
  PMICYAUDIO_SET_AUDIO_DATA pBuffer =
      (PMICYAUDIO_SET_AUDIO_DATA)malloc(bufferSize);

  if (pBuffer == NULL) {
    printf("Failed to allocate buffer\n");
    return FALSE;
  }

  // Fill in the structure
  pBuffer->StreamId = streamId;
  pBuffer->DataSize = dataSize;

  // Copy audio data
  memcpy(pBuffer->AudioData, audioData, dataSize);

  // Send IOCTL
  DWORD bytesReturned = 0;
  BOOL result =
      DeviceIoControl(hDevice, IOCTL_MICYAUDIO_SET_AUDIO_DATA, pBuffer,
                      bufferSize, NULL, 0, &bytesReturned, NULL);

  if (!result) {
    DWORD error = GetLastError();
    printf("DeviceIoControl failed with error: %lu (0x%08lx)\n", error, error);
    free(pBuffer);
    return FALSE;
  }

  printf("Successfully sent %lu bytes of audio data to stream %lu\n", dataSize,
         streamId);

  free(pBuffer);
  return TRUE;
}

// Function to read audio data from a WAV file (simplified - assumes PCM format)
BOOL ReadAudioDataFromFile(const char *filename, BYTE **ppAudioData,
                           DWORD *pDataSize) {
  FILE *file;
  errno_t err;
  err = fopen_s(&file, filename, "rb");

  if (file == NULL || err != 0) {
    printf("Failed to open file: %s\n", filename);
    return FALSE;
  }

  // Skip WAV header (simplified - assumes 44-byte standard header)
  fseek(file, 44, SEEK_SET);

  // Get file size
  fseek(file, 0, SEEK_END);
  long fileSize = ftell(file);
  long audioSize = fileSize - 44; // Subtract header size

  if (audioSize <= 0) {
    printf("Invalid audio file size\n");
    fclose(file);
    return FALSE;
  }

  // Allocate buffer
  *ppAudioData = (BYTE *)malloc(audioSize);
  if (*ppAudioData == NULL) {
    printf("Failed to allocate memory\n");
    fclose(file);
    return FALSE;
  }

  // Read audio data
  fseek(file, 44, SEEK_SET);
  size_t bytesRead = fread(*ppAudioData, 1, audioSize, file);
  fclose(file);

  if (bytesRead != audioSize) {
    printf("Failed to read audio data completely\n");
    free(*ppAudioData);
    return FALSE;
  }

  *pDataSize = (DWORD)audioSize;
  return TRUE;
}

int main(int argc, char *argv[]) {
  printf("MicyAudio - Audio Data Sender\n");
  printf("=====================================\n\n");

  HANDLE hDevice = INVALID_HANDLE_VALUE;
  BYTE *audioData = NULL;
  DWORD audioDataSize = 0;
  ULONG streamId = 1; // Typically 1 for capture/microphone stream

  // Parse command line arguments
  if (argc > 1) {
    if (strcmp(argv[1], "--help") == 0 || strcmp(argv[1], "-h") == 0) {
      printf("Usage: %s [options]\n", argv[0]);
      printf("Options:\n");
      printf("  --stream-id <id>    Stream ID (Pin ID) - default: 1\n");
      printf("  --file <filename>    WAV file to send (PCM format, 44-byte "
             "header)\n");
      printf("  --generate <size>    Generate sine wave of specified size in "
             "bytes\n");
      printf("  --sample-rate <rate> Sample rate for generated audio (default: "
             "44100)\n");
      printf("  --channels <num>     Number of channels (default: 2)\n");
      printf("  --bits <bits>        Bits per sample (default: 16)\n");
      printf("\nExample:\n");
      printf(
          "  %s --generate 44100 --sample-rate 44100 --channels 2 --bits 16\n",
          argv[0]);
      printf("  %s --file audio.wav\n", argv[0]);
      return 0;
    }
  }

  // Open device handle
  hDevice = OpenAudioDevice();

  // Example: Generate sine wave audio
  DWORD sampleRate = 48000;
  WORD channels = 2;
  WORD bitsPerSample = 16;
  DWORD audioSize = 48000; // 1 second of audio

  // Parse command line
  for (int i = 1; i < argc; i++) {
    if (strcmp(argv[i], "--stream-id") == 0 && i + 1 < argc) {
      streamId = (ULONG)atoi(argv[++i]);
    } else if (strcmp(argv[i], "--generate") == 0 && i + 1 < argc) {
      audioSize = (DWORD)atoi(argv[++i]);
    } else if (strcmp(argv[i], "--file") == 0 && i + 1 < argc) {
      if (!ReadAudioDataFromFile(argv[++i], &audioData, &audioDataSize)) {
        printf("Failed to read audio file\n");
        return 1;
      }
      printf("Read %lu bytes from audio file\n", audioDataSize);
    } else if (strcmp(argv[i], "--sample-rate") == 0 && i + 1 < argc) {
      sampleRate = (DWORD)atoi(argv[++i]);
    } else if (strcmp(argv[i], "--channels") == 0 && i + 1 < argc) {
      channels = (WORD)atoi(argv[++i]);
    } else if (strcmp(argv[i], "--bits") == 0 && i + 1 < argc) {
      bitsPerSample = (WORD)atoi(argv[++i]);
    }
  }

  // If no file was loaded, generate sine wave
  if (audioData == NULL) {
    audioDataSize = audioSize;
    audioData = (BYTE *)malloc(audioDataSize);
    if (audioData == NULL) {
      printf("Failed to allocate memory for audio data\n");
      return 1;
    }

    printf("Generating %lu bytes of sine wave audio...\n", audioDataSize);
    printf("  Sample Rate: %lu Hz\n", sampleRate);
    printf("  Channels: %u\n", channels);
    printf("  Bits per Sample: %u\n", bitsPerSample);

    GenerateSineWave(audioData, audioDataSize, sampleRate, channels,
                     bitsPerSample);
  }

  // Send data to driver
  if (hDevice != INVALID_HANDLE_VALUE) {
    printf("\nSending audio data to driver...\n");
    printf("  Stream ID: %lu\n", streamId);
    printf("  Data Size: %lu bytes\n", audioDataSize);

    if (SendAudioDataToDriver(hDevice, streamId, audioData, audioDataSize)) {
      printf("Success!\n");
    } else {
      printf("Failed to send audio data\n");
    }

    CloseHandle(hDevice);
  } else {
    printf("\nCannot send data - device handle not available\n");
    printf("Make sure the MicyAudio driver is installed and running.\n");

    // For demonstration, show what would be sent
    printf("\nWould send:\n");
    printf("  Stream ID: %lu\n", streamId);
    printf("  Data Size: %lu bytes\n", audioDataSize);
    printf("  First 16 bytes of audio data: ");
    for (DWORD i = 0; i < 16 && i < audioDataSize; i++) {
      printf("%02X ", audioData[i]);
    }
    printf("\n");
  }

  // Cleanup
  if (audioData != NULL) {
    free(audioData);
  }

  return 0;
}
