// Audio: AW88298 speaker and ES7210 microphones on I2S0 (shared clocks).
// Methods: audio.get, audio.set_volume, audio.beep. Stream: audio.
#pragma once

namespace audio {

void init();
void registerMethods();
void start();

}  // namespace audio
