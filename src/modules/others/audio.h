
#include <SPIFFS.h>
// Keep SPIFFS first

#include <ESP8266Audio.h>
#include <ESP8266SAM.h>
#include <functional>

class AudioOutputI2S;

bool playAudioFile(FS *fs, String filepath); // TODO: bool async arg -> play in a task?
typedef std::function<bool(size_t, size_t, AudioOutputI2S *)> AudioProgressCb;
bool playAudioFile(FS *fs, String filepath, AudioProgressCb progressCb, size_t startPos = 0, bool *stoppedByCb = nullptr);

bool playAudioRTTTLString(String song);

bool tts(String text);

bool isAudioFile(String filePath);

void playTone(unsigned int frequency, unsigned long duration = 0UL, short waveType = 0);

void _tone(unsigned int frequency, unsigned long duration = 0UL);
