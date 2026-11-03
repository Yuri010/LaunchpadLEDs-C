#ifndef MIDI_BACKEND_H
#define MIDI_BACKEND_H

#include <stddef.h>
#include <stdint.h>

#define MIDI_DEVICE_NAME_SIZE 128

typedef struct MidiDevice MidiDevice;

typedef void (*MidiInputCallback)(void *context, uint8_t status, uint8_t note, uint8_t velocity);

/**
 * @brief Open matching Launchpad MIDI input and output ports.
 *
 * Opens the first input and output ports whose names contain "Launchpad".
 * @param[out] device Backend device handle to initialize.
 * @param[in] callback Callback for received channel MIDI messages.
 * @param[in] context Opaque value passed to the input callback.
 * @retval 1 Ports opened successfully.
 * @retval 0 No matching ports or an open operation failed.
 */
int midi_open(MidiDevice **device, MidiInputCallback callback, void *context);

/**
 * @brief Close both MIDI ports and release backend resources.
 *
 * Stops input delivery before closing the device.
 * @param[in,out] device Device handle returned by midi_open.
 */
void midi_close(MidiDevice *device);

/**
 * @brief Send a complete SysEx message.
 *
 * The supplied bytes must include the F0 start and F7 end bytes.
 * @param[in] device Open MIDI device.
 * @param[in] data Complete SysEx message bytes.
 * @param[in] length Number of bytes in data.
 * @retval 1 Message sent.
 * @retval 0 Invalid message or backend send failed.
 */
int midi_send_sysex(MidiDevice *device, const uint8_t *data, size_t length);

/**
 * @brief Send a packed short MIDI message.
 *
 * The first byte is the status byte; following data bytes are packed little-endian.
 * @param[in] device Open MIDI device.
 * @param[in] message Packed MIDI message.
 * @retval 1 Message sent.
 * @retval 0 Backend send failed.
 */
int midi_send_short(MidiDevice *device, uint32_t message);

/**
 * @brief Return the selected output port name.
 *
 * The returned string remains valid until midi_close.
 * @param[in] device Open MIDI device.
 * @return Output port name, or "Launchpad" if unavailable.
 */
const char *midi_device_name(const MidiDevice *device);

#endif
