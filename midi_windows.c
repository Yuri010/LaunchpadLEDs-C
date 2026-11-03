#include "midi_backend.h"

#include <windows.h>
#include <mmeapi.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct MidiDevice {
  HMIDIOUT          output;
  HMIDIIN           input;
  MidiInputCallback callback;
  void              *context;
  char              name[MIDI_DEVICE_NAME_SIZE];
  CRITICAL_SECTION  output_lock;
};

/**
 * @brief Print a WinMM error message.
 *
 * Uses the system's MIDI error text when available.
 * @param[in] operation Operation that failed.
 * @param[in] result WinMM error code.
 */
static void report_error(const char *operation, MMRESULT result) {
  char message[MAXERRORLENGTH];
  if (midiOutGetErrorTextA(result, message, sizeof(message)) == MMSYSERR_NOERROR) {
    fprintf(stderr, "%s: %s\n", operation, message);
  } else {
    fprintf(stderr, "%s failed (MIDI error %u)\n", operation, (unsigned)result);
  }
}

/**
 * @brief Find a case-insensitive Launchpad name.
 *
 * Scans a device name without requiring a platform-specific string function.
 * @param[in] name MIDI device name.
 * @retval 1 Name contains "Launchpad".
 * @retval 0 Name does not contain "Launchpad".
 */
static int has_launchpad_name(const char *name) {
  static const char needle[] = "launchpad";
  size_t            i;
  size_t            j;
  for (i = 0; name[i] != '\0'; ++i) {
    for (j = 0; needle[j] != '\0' && name[i + j] != '\0'; ++j) {
      char value = name[i + j];
      if (value >= 'A' && value <= 'Z') {
        value = (char)(value - 'A' + 'a');
      }
      if (value != needle[j]) {
        break;
      }
    }
    if (needle[j] == '\0') {
      return 1;
    }
  }
  return 0;
}

/**
 * @brief Forward WinMM channel input messages to the shared shell.
 *
 * Ignores non-channel input notifications.
 * @param[in] input WinMM input handle.
 * @param[in] message WinMM callback message.
 * @param[in] instance MidiDevice passed when opening the input.
 * @param[in] param1 Packed MIDI message for MIM_DATA.
 * @param[in] param2 Unused WinMM callback data.
 */
static void CALLBACK input_callback(HMIDIIN input, UINT message, DWORD_PTR instance,
                                    DWORD_PTR param1, DWORD_PTR param2) {
  MidiDevice *device = (MidiDevice *)instance;
  DWORD      packed;
  (void)input;
  (void)param2;
  if (message != MIM_DATA || device == NULL || device->callback == NULL) {
    return;
  }
  packed = (DWORD)param1;
  device->callback(device->context, (uint8_t)(packed & 0xFF),
                   (uint8_t)((packed >> 8) & 0xFF),
                   (uint8_t)((packed >> 16) & 0xFF));
}

/**
 * @brief Open matching Windows MIDI input and output ports.
 *
 * Selects the first WinMM port whose name contains "Launchpad".
 * @param[out] device Receives the allocated backend handle.
 * @param[in] callback Callback for received channel MIDI messages.
 * @param[in] context Opaque callback context.
 * @retval 1 Ports opened successfully.
 * @retval 0 Invalid arguments, missing ports, or a WinMM operation failed.
 */
int midi_open(MidiDevice **device, MidiInputCallback callback, void *context) {
  MidiDevice  *result;
  MIDIOUTCAPS output_caps;
  MIDIINCAPS  input_caps;
  UINT        output_id = (UINT)-1;
  UINT        input_id  = (UINT)-1;
  UINT        i;
  MMRESULT    status;

  if (device == NULL || callback == NULL) {
    return 0;
  }
  *device = NULL;
  result  = (MidiDevice *)calloc(1, sizeof(*result));
  if (result == NULL) {
    fprintf(stderr, "Unable to allocate MIDI device state.\n");
    return 0;
  }
  result->callback = callback;
  result->context  = context;
  InitializeCriticalSection(&result->output_lock);

  for (i = 0; i < midiOutGetNumDevs(); ++i) {
    if (midiOutGetDevCapsA(i, &output_caps, sizeof(output_caps)) == MMSYSERR_NOERROR &&
        has_launchpad_name(output_caps.szPname)) {
      output_id = i;
      break;
    }
  }
  for (i = 0; i < midiInGetNumDevs(); ++i) {
    if (midiInGetDevCapsA(i, &input_caps, sizeof(input_caps)) == MMSYSERR_NOERROR &&
        has_launchpad_name(input_caps.szPname)) {
      input_id = i;
      break;
    }
  }
  if (output_id == (UINT)-1 || input_id == (UINT)-1) {
    fprintf(stderr, "Could not find both Launchpad MIDI input and output devices.\n");
    DeleteCriticalSection(&result->output_lock);
    free(result);
    return 0;
  }

  status = midiOutOpen(&result->output, output_id, 0, 0, CALLBACK_NULL);
  if (status != MMSYSERR_NOERROR) {
    report_error("Opening MIDI output", status);
    DeleteCriticalSection(&result->output_lock);
    free(result);
    return 0;
  }
  status = midiInOpen(&result->input, input_id, (DWORD_PTR)input_callback, (DWORD_PTR)result, CALLBACK_FUNCTION);
  if (status != MMSYSERR_NOERROR) {
    report_error("Opening MIDI input", status);
    midiOutClose(result->output);
    DeleteCriticalSection(&result->output_lock);
    free(result);
    return 0;
  }
  status = midiInStart(result->input);
  if (status != MMSYSERR_NOERROR) {
    report_error("Starting MIDI input", status);
    midiInClose(result->input);
    midiOutClose(result->output);
    DeleteCriticalSection(&result->output_lock);
    free(result);
    return 0;
  }

  snprintf(result->name, sizeof(result->name), "%s", output_caps.szPname);
  *device = result;
  printf("Connected to: %s (out), %s (in)\n", output_caps.szPname, input_caps.szPname);
  return 1;
}

/**
 * @brief Stop and close both Windows MIDI ports.
 *
 * Releases the WinMM handles, lock, and allocated state.
 * @param[in,out] device Device handle returned by midi_open.
 */
void midi_close(MidiDevice *device) {
  MMRESULT status;
  if (device == NULL) {
    return;
  }
  if (device->input != NULL) {
    midiInStop(device->input);
    midiInReset(device->input);
    status = midiInClose(device->input);
    if (status != MMSYSERR_NOERROR) {
      report_error("Closing MIDI input", status);
    }
  }
  if (device->output != NULL) {
    midiOutReset(device->output);
    status = midiOutClose(device->output);
    if (status != MMSYSERR_NOERROR) {
      report_error("Closing MIDI output", status);
    }
  }
  DeleteCriticalSection(&device->output_lock);
  free(device);
}

/**
 * @brief Send a complete SysEx message to the Windows MIDI output.
 *
 * Keeps the data buffer alive until WinMM reports message completion.
 * @param[in] device Open MIDI device.
 * @param[in] data Complete SysEx message bytes.
 * @param[in] length Number of bytes in data.
 * @retval 1 Message sent.
 * @retval 0 Invalid message or a WinMM operation failed.
 */
int midi_send_sysex(MidiDevice *device, const uint8_t *data, size_t length) {
  char     buffer[1024];
  MIDIHDR  header;
  MMRESULT status;
  if (device == NULL || device->output == NULL || data == NULL || length == 0 || length > sizeof(buffer)) {
    fprintf(stderr, "Invalid SysEx message.\n");
    return 0;
  }
  memcpy(buffer, data, length);
  memset(&header, 0, sizeof(header));
  header.lpData         = buffer;
  header.dwBufferLength = (DWORD)length;

  EnterCriticalSection(&device->output_lock);
  status = midiOutPrepareHeader(device->output, &header, sizeof(header));
  if (status == MMSYSERR_NOERROR) {
    status = midiOutLongMsg(device->output, &header, sizeof(header));
    if (status == MMSYSERR_NOERROR) {
      while ((header.dwFlags & MHDR_DONE) == 0) {
        Sleep(1);
      }
    }
    {
      MMRESULT unprepare_status = midiOutUnprepareHeader(device->output, &header, sizeof(header));
      if (status == MMSYSERR_NOERROR && unprepare_status != MMSYSERR_NOERROR) {
        status = unprepare_status;
      }
    }
  }
  LeaveCriticalSection(&device->output_lock);
  if (status != MMSYSERR_NOERROR) {
    report_error("Sending SysEx", status);
    return 0;
  }
  return 1;
}

/**
 * @brief Send a packed short MIDI message to Windows MIDI output.
 *
 * Serializes access to the WinMM output handle.
 * @param[in] device Open MIDI device.
 * @param[in] message Packed MIDI message.
 * @retval 1 Message sent.
 * @retval 0 Device is unavailable or WinMM send failed.
 */
int midi_send_short(MidiDevice *device, uint32_t message) {
  MMRESULT status;
  if (device == NULL || device->output == NULL) {
    fprintf(stderr, "MIDI output is not open.\n");
    return 0;
  }
  EnterCriticalSection(&device->output_lock);
  status = midiOutShortMsg(device->output, (DWORD)message);
  LeaveCriticalSection(&device->output_lock);
  if (status != MMSYSERR_NOERROR) {
    report_error("Sending MIDI", status);
    return 0;
  }
  return 1;
}

/**
 * @brief Return the selected Windows MIDI output name.
 *
 * The name is stored in the backend device state.
 * @param[in] device Open MIDI device.
 * @return Output port name, or "Launchpad" if unavailable.
 */
const char *midi_device_name(const MidiDevice *device) {
  return device != NULL ? device->name : "Launchpad";
}
