// HAS Karaoke's own ASIO host: a JUCE device type shown as "HAS Direct" that loads the ASIO drivers listed in the
// registry (HKLM\SOFTWARE\ASIO) through their COM interface. Written from the public driver ABI, without
// the Steinberg SDK.
#pragma once

#include <juce_audio_devices/juce_audio_devices.h>

#include <memory>

namespace hearaside::karaoke::asio {

// ASIO sample types this host converts (little-endian, the Windows ones)
enum SampleType : long {
    Int16LSB = 16, Int24LSB = 17, Int32LSB = 18, Float32LSB = 19, Float64LSB = 20,
    Int32LSB16 = 24, Int32LSB18 = 25, Int32LSB20 = 26, Int32LSB24 = 27
};

// bytes per sample, 0 = a type this host can't handle
int bytesPerSample(long type) noexcept;
int bitDepth(long type) noexcept;
// driver buffer <-> float. false = unsupported type (dst untouched)
bool toFloat(long type, const void* src, float* dst, int n) noexcept;
bool fromFloat(long type, const float* src, void* dst, int n) noexcept;

// nullptr off Windows
std::unique_ptr<juce::AudioIODeviceType> createDeviceType();

} // namespace hearaside::karaoke::asio
