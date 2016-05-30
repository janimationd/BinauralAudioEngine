#include "stdlib.h"
#include "stdio.h"
#include "Windows.h"
#include "math.h"
#include "stdint.h"
#include "Audioclient.h"
#include "Audiopolicy.h"
#include "Mmdeviceapi.h"

#include "SFML\Audio.hpp"
#include "filt.h"
#include "DSPFilters/Legendre.h"

#define M_PI 3.14159265358979323846
#define BOOST_MOD 0.8f
#define SHIFT_CONST 0.0f
#define POW_MOD 1.0f
#define RADIANS_PER_MS 0.0008f
#define DELTA_TIME 0.03333f
#define HEAD_RADIUS 0.0762f // meters
#define SPEED_OF_SOUND 340.29f // meters/second
#define REFTIMES_PER_SEC 10000000
#define REFTIMES_PER_MILLISEC 10000
#define SFML_BYTES_PER_SAMPLE 2

#define EXIT_ON_ERROR(hres) if (hres != 0) { printf("HRESULT FAILED!\n"); exit(hres); }
#define SAFE_RELEASE(punk)  if ((punk) != NULL)  { (punk)->Release(); (punk) = NULL; }

#define CLAMP_SAMPLE(a, x) ((a) < 0 ? \
                        ((a) < -x ? -x : (a)) : \
                        ((a) > x ? x : (a)))

DWORD startTime;

/**
 * A representation of a polar positional intensity diagram
 */
class PolarStorage {
private:
  const float *entries;
  const float right_ear_values[24] = { 0.0f, 2.7f, 5.0f, 7.5f, 8.5f, 9.3f, 9.7f, 7.7f, 4.9f, 2.0f, -1.5f, -3.5f, -5.0f, -6.5f, -9.0f, -12.0f, -13.0f, -13.0f, -12.0f, -11.0f, -10.5f, -8.0f, -5.0f, -2.5f };
  const float left_ear_values[24] = { 0.0f, -2.5f, -5.0f, -8.0f, -10.5f, -11.0f, -12.0f, -13.0f, -13.0f, -12.0f, -9.0f, -6.5f, -5.0f, -3.5f, -1.5f, 2.0f, 4.9f, 7.7f, 9.7f, 9.3f, 8.5f, 7.5f, 5.0f, 2.7f };
  const uint32_t num_entries = 24;

public:
  enum class earSide : uint8_t {
    LEFT = 0,
    RIGHT = 1
  };

  PolarStorage(earSide side) {
    entries = (uint8_t)side ? right_ear_values : left_ear_values;
  }

  /**
   * Calculates an interpolated value from the entries array that should be accurate at the given angle
   *
   * 'angle' is an amount in radians, where 0 is straight ahead, and pi/2 is out to the right
   */
  float get_value(float angle) {
    angle *= 180 / M_PI;
    while (angle >= 360.0f || angle < 0.0f) {
      if (angle > 360.0f)
        angle -= 360.0f;
      else
        angle += 360.0f;
    }

    uint32_t low_index = (uint32_t)((angle / 360.0f) * num_entries) % num_entries;
    uint32_t high_index = (low_index + 1) % num_entries;

    float low_value = entries[low_index];
    float high_value = entries[high_index];

    float interval = 360.0f / num_entries;
    float angle_dif = angle - (low_index * interval);
    float interp_ratio_from_low = angle_dif / interval;

    // 10^(x/20)
    float temp = (low_value * (1 - interp_ratio_from_low)) + (high_value * interp_ratio_from_low);
    return pow(10, temp / 20 * POW_MOD);
  }
};

float idk(float in) {
  in *= 180 / M_PI;
  int x = (int)in % 360;
  if (x >= 90 && x <= 270) {
    x = 180 - x;
  }
  if (x > 270) // 225 -> -45, 270 -> -90
    x -= 360;
  return (float)x / 180.f * M_PI;
}

// class to play a single sound
class SoundPlayer {
protected:
  // WASAPI
  IMMDeviceEnumerator *immde;
  IMMDevice *immd;
  IAudioClient *iac;
  WAVEFORMATEX *wf;
  UINT32 deviceBufferLength;  // number of samples
  IAudioRenderClient *iarc;
  REFERENCE_TIME deviceBufferDuration;  // 10,000,000ths of a second
  BYTE *deviceBuffer;

  const CLSID CLSID_MMDeviceEnumerator = __uuidof(MMDeviceEnumerator);
  const IID IID_IMMDeviceEnumerator = __uuidof(IMMDeviceEnumerator);
  const IID IID_IAudioClient = __uuidof(IAudioClient);
  const IID IID_IAudioRenderClient = __uuidof(IAudioRenderClient);

  sf::Int16 *monoBuffer;  // Source sound sample buffer (mono, average of stereo input track)
  sf::Int16 *leftBuffer;  // Left sound buffer for mucking around in
  sf::Int16 *rightBuffer;  // Right sound buffer for mucking around in

  // Sound state and misc
  float position;  // Angle in radians: 0 out in front, pi/2 to right
  PolarStorage *rightPS, *leftPS;
  int numChannels;
  uint64_t numSamples;
  uint64_t currentPlayOffset;

  // Sound modifiers
  float leftBoost, rightBoost;
  float itd;  // positive happens when sound is on right

public:
  // Construct a new SoundPlayer on the input file provided, with initial horizontal position "initPos"
  SoundPlayer(const char *filename, const float initPos) : currentPlayOffset(0) {
    // Read file
    LoadSound(filename);

    // Setup PolarStorages
    leftPS = new PolarStorage(PolarStorage::earSide::LEFT);
    rightPS = new PolarStorage(PolarStorage::earSide::RIGHT);

    // Calculate sound modifiers, update position
    position = initPos;
    leftBoost = rightBoost = 1;
    UpdatePosition(initPos);

    // Wasapi setup
    HRESULT hr = 0;
    CoInitializeEx(NULL, COINIT_MULTITHREADED);
    hr += CoCreateInstance(CLSID_MMDeviceEnumerator, NULL, CLSCTX_ALL, IID_IMMDeviceEnumerator, (void**)&immde);
    hr += immde->GetDefaultAudioEndpoint(eRender, eConsole, &immd);
    hr += immd->Activate(IID_IAudioClient, CLSCTX_ALL, NULL, (void**)&iac);
    hr += iac->GetMixFormat(&wf);
    printFormatInfo();
    hr += iac->Initialize(AUDCLNT_SHAREMODE_SHARED, 0, REFTIMES_PER_SEC * DELTA_TIME, 0, wf, NULL);
    hr += iac->GetBufferSize(&deviceBufferLength);
    printf("DeviceBufferLength: %u\n", deviceBufferLength);
    hr += iac->GetService(IID_IAudioRenderClient, (void**)&iarc);
    EXIT_ON_ERROR(hr);

    // Initialize workspace buffers
    leftBuffer = (sf::Int16*)calloc(deviceBufferLength / numChannels, sizeof(sf::Int16));
    rightBuffer = (sf::Int16*)calloc(deviceBufferLength / numChannels, sizeof(sf::Int16));

    // Calculate the actual duration of the allocated buffer.
    deviceBufferDuration = (double)REFTIMES_PER_SEC * deviceBufferLength / wf->nSamplesPerSec;
  }

  void printFormatInfo() {
    printf("*** Device Properties:\n");
    printf("*** nChannels = %lu\n", wf->nChannels);
    printf("*** nSamplesPerSec = %u\n", wf->nSamplesPerSec);
    printf("*** nBlockAlign = %lu\n", wf->nBlockAlign);
    printf("*** wBitsPerSample = %lu\n", wf->wBitsPerSample);
  }

  // Loads a sound from file, and readies it for processing
  void LoadSound(const char *filename) {
    sf::SoundBuffer buf;
    if (!buf.loadFromFile(filename)) {
      printf("Error loading file.\n");
      exit(1);
    }
    numChannels = buf.getChannelCount();
    numSamples = buf.getSampleCount();

    monoBuffer = (sf::Int16*)calloc(numSamples / numChannels, sizeof(sf::Int16));

    const sf::Int16 *copyBuf = buf.getSamples();

    // Average stereo tracks together, or copy mono track
    for (int i = 0; i < numSamples; i += numChannels) {
      if (numChannels == 2) {
        monoBuffer[i / 2] = (sf::Int16)(((int)copyBuf[i] + (int)copyBuf[i + 1]) / 2);
      }
      else {  // mono sound
        monoBuffer[i] = copyBuf[i];
      }
    }
  }

  // Receive a new position, recalculate sound modifiers
  void UpdatePosition(float newPosition) {
    position = newPosition;
    leftBoost = leftPS->get_value(position);
    rightBoost = rightPS->get_value(position);
    itd = HEAD_RADIUS * (idk(position) + sinf(idk(position))) / SPEED_OF_SOUND;
  }

  void copyData(void *dest, uint32_t numFramesCpy) {
    float lval, rval;
    for (unsigned int i = 0; i < numFramesCpy; i++) {
      lval = (float)leftBuffer[i] / 32768.f;
      rval = (float)rightBuffer[i] / 32768.f;
      ((float*)dest)[2 * i] = CLAMP_SAMPLE(lval, 0.99f);
      ((float*)dest)[2 * i + 1] = CLAMP_SAMPLE(rval, 0.99f);
    }
  }

  void GetNextModifiedFrames(uint32_t numFrames, BYTE *data, uint32_t *flags) {
    // check if we'll hit the end of the sound
    if (numFrames + currentPlayOffset >= numSamples) {
      numFrames = numSamples - currentPlayOffset;
      *flags = AUDCLNT_BUFFERFLAGS_SILENT;
    }

    // Fill working buffers for each ear
    for (int i = 0; i < numFrames; i++) {
      int leftIndex = i + currentPlayOffset - (itd * 44100 / 2);
      int rightIndex = i + currentPlayOffset + (itd * 44100 / 2);
      UpdatePosition(float(i + currentPlayOffset) / 44100.f * 1000.f * RADIANS_PER_MS);
      if (leftIndex >= 0) {
        leftBuffer[i] = CLAMP_SAMPLE(monoBuffer[leftIndex] * leftBoost * BOOST_MOD, 32767);
      }
      else {
        leftBuffer[i] = 0;
      }

      if (rightIndex >= 0) {
        rightBuffer[i] = CLAMP_SAMPLE(monoBuffer[rightIndex] * rightBoost * BOOST_MOD, 32767);
      }
      else {
        rightBuffer[i] = 0;
      }
    }

    printf("ITD: %f\n", itd);

    // copy over the requested number of frames
    copyData(data, numFrames);

    // update currPlayOffset
    currentPlayOffset += numFrames;
  }

  void Play() {
    uint32_t flags = 0;
    uint32_t numFramesAvailable, numFramesPadding;
    HRESULT hr;

    hr = iac->Start();
    EXIT_ON_ERROR(hr);
    startTime = GetTickCount();

    while (flags != AUDCLNT_BUFFERFLAGS_SILENT) {
      // Sleep for half the duration
      Sleep((DWORD)(deviceBufferDuration / REFTIMES_PER_MILLISEC / 2));

      // See how much buffer space is used
      iac->GetCurrentPadding(&numFramesPadding);

      // Calculate how much buffer space is available
      numFramesAvailable = deviceBufferLength - numFramesPadding;
      //printf(">>> In Play: numFramesAvailable = %u, deviceBufferLength = %u, numFramesPadding = %u\n", numFramesAvailable, deviceBufferLength, numFramesPadding);

      // Grab all the available space in the shared buffer.
      hr = iarc->GetBuffer(numFramesAvailable, &deviceBuffer);
      EXIT_ON_ERROR(hr);

      // Update the position of the sound source
      //UpdatePosition((GetTickCount() - startTime) * RADIANS_PER_MS);

      // Get next 1/2-second of data from the audio source.
      GetNextModifiedFrames(numFramesAvailable, deviceBuffer, &flags);

      // Release buffer
      hr = iarc->ReleaseBuffer(numFramesAvailable, flags);
      EXIT_ON_ERROR(hr);
    }

    // Let the rest of the sound play
    Sleep((DWORD)(deviceBufferDuration / REFTIMES_PER_MILLISEC / 2));

    // Teardown
    iac->Stop();
    SAFE_RELEASE(immde)
      SAFE_RELEASE(immd)
      SAFE_RELEASE(iac)
      SAFE_RELEASE(iarc)
  }

  void Teardown() {
    free(monoBuffer);
  }
};



void main(int argc, char *argv[]) {
  if (false && argc > 2) {
    printf("Usage: %s [options]\n\t\toptions:\n\t\t-d   disable Interaural Time Difference feature.", argv[0]);
    return;
  }

  // The Sound Player
  SoundPlayer sp("C:/Users/Johnathan/Desktop/Classes/Senior Project/flamenco.wav", 0);
  printf("> Before Play!\n");
  sp.Play();
  sp.Teardown();

  ////////////////

  //sf::SoundBuffer buffer_left;
  //sf::SoundBuffer buffer_right;
  //buffer_left.loadFromSamples(leftSamples, num_samples, 2, buffer.getSampleRate());
  //buffer_right.loadFromSamples(rightSamples, num_samples, 2, buffer.getSampleRate());
  //
  //sf::Sound left_clip;
  //sf::Sound right_clip;
  //left_clip.setLoop(true);
  //right_clip.setLoop(true);
  //left_clip.setBuffer(buffer_left);
  //right_clip.setBuffer(buffer_right);

  //if (argc < 2) {
  //	if ((int)(position * 180 / M_PI) % 360 <= 180) {
  //		right_clip.setPlayingOffset(init_offset);
  //	}
  //	else {
  //		left_clip.setPlayingOffset(init_offset);
  //	}
  //}

  //left_clip.setVolume(left_boost * BOOST_CONST + SHIFT_CONST);
  //right_clip.setVolume(right_boost * BOOST_CONST + SHIFT_CONST);

  //left_clip.play();
  //right_clip.play();

  //float new_position, shift_l, shift_r, itd_old, itd_new, itd_delta, pitch_l, pitch_r;
  //DWORD start_ticks = GetTickCount();
  //DWORD current_ticks, last_ticks = start_ticks;

  //itd_old = HEAD_RADIUS * (idk(position) + sinf(idk(position))) / SPEED_OF_SOUND;
 // printf("itd_old = %.6f\n\n", itd_old);

 // // dynamics loop
 // while (true) {
 //   if (DYNAMIC) {
 //     current_ticks = GetTickCount();

 //     // Set ILD
 //     new_position = position + RADIANS_PER_MS * (current_ticks - start_ticks);
 //     left_boost = left_ear_polar.get_value(new_position);
 //     right_boost = right_ear_polar.get_value(new_position);

 //     left_clip.setVolume(left_boost * BOOST_CONST + SHIFT_CONST);
 //     right_clip.setVolume(right_boost * BOOST_CONST + SHIFT_CONST);

 //     /*Dsp::Legendre::BandStop<16> bs;
 //     bs.setup(11, 1, 1, 1);
 //     bs.process(1, NULL, ;*/

 //     // Set ITD
 //     itd_new = HEAD_RADIUS * (idk(new_position) + sinf(idk(new_position))) / SPEED_OF_SOUND;
 //     itd_delta = itd_new - itd_old;  // positive for going right, negative for going left
 //     shift_r = itd_delta / 2 * SHIFT_MULT;
 //     shift_l = -shift_r;
 //     pitch_l = (shift_l + DELTA_TIME) / DELTA_TIME;
 //     pitch_r = (shift_r + DELTA_TIME) / DELTA_TIME;

 //     printf("left pitch = %.5f, right_pitch = %.5f, shift = %.8f, position diff = %ld\n",
 //       pitch_l, pitch_r, shift_r, left_clip.getPlayingOffset().asMicroseconds() - right_clip.getPlayingOffset().asMicroseconds());

 //     //printf("itd_new %.6f, pitch_l %.5f, pitch_r %.5f, shift_r %.6f\n", itd_new, pitch_l, pitch_r, shift_r);

 //     left_clip.setPitch(argc == 2 ? 1 : pitch_l);
 //     right_clip.setPitch(argc == 2 ? 1 : pitch_r);

 //     itd_old = itd_new;

 //     // handle timing
 //     current_ticks = GetTickCount();
 //     DWORD diff = current_ticks - last_ticks;
 //     if (diff < DELTA_TIME * 1000)
 //       Sleep((DELTA_TIME * 1000) - diff);
 //     last_ticks = GetTickCount();
 //   }
 // }

  //left_clip.stop();
  //right_clip.stop();
}