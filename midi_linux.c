#include "midi_backend.h"

#include <alsa/asoundlib.h>
#include <alsa/seq_midi_event.h>
#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

struct MidiDevice {
  snd_seq_t         *sequence;
  snd_midi_event_t  *encoder;
  int               output_port;
  int               input_port;
  int               target_output_client;
  int               target_output_port;
  MidiInputCallback callback;
  void              *context;
  char              name[MIDI_DEVICE_NAME_SIZE];
  pthread_mutex_t   lock;
  pthread_t         input_thread;
  atomic_int        running;
  int               thread_started;
};

/**
 * @brief Print an ALSA sequencer error.
 *
 * Converts a negative ALSA result into its readable error text.
 * @param[in] operation Operation that failed.
 * @param[in] result ALSA result code.
 */
static void report_error(const char *operation, int result) {
  fprintf(stderr, "%s: %s\n", operation, snd_strerror(result));
}

/**
 * @brief Find a case-insensitive Launchpad name.
 *
 * Scans a port name without allocating a temporary lowercase copy.
 * @param[in] name ALSA MIDI port name.
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
 * @brief Find the first compatible Launchpad input and output ports.
 *
 * Enumerates ALSA sequencer clients and checks their port capabilities.
 * @param[in] sequence Open ALSA sequencer handle.
 * @param[out] output_client Receives the destination client id.
 * @param[out] output_port Receives the destination output port id.
 * @param[out] input_client Receives the input source client id.
 * @param[out] input_port Receives the input source port id.
 * @param[out] output_name Receives a copy of the output port name.
 * @retval 1 Both compatible ports found.
 * @retval 0 Enumeration failed or either port was not found.
 */
static int find_launchpad_ports(snd_seq_t *sequence, int *output_client,
                                int *output_port, int *input_client,
                                int *input_port, char *output_name) {
  snd_seq_client_info_t *client_info = NULL;
  snd_seq_port_info_t   *port_info   = NULL;
  int                   found_output = 0;
  int                   found_input  = 0;
  int                   result;
  int                   self         = snd_seq_client_id(sequence);

  if (snd_seq_client_info_malloc(&client_info) < 0 || snd_seq_port_info_malloc(&port_info) < 0) {
    fprintf(stderr, "Unable to allocate ALSA port query state.\n");
    if (port_info != NULL) {
      snd_seq_port_info_free(port_info);
    }
    if (client_info != NULL) {
      snd_seq_client_info_free(client_info);
    }
    return 0;
  }
  snd_seq_client_info_set_client(client_info, -1);
  while (snd_seq_query_next_client(sequence, client_info) >= 0 && !(found_output && found_input)) {
    int client = snd_seq_client_info_get_client(client_info);
    if (client == self) {
      continue;
    }
    snd_seq_port_info_set_client(port_info, client);
    snd_seq_port_info_set_port(port_info, -1);
    while (snd_seq_query_next_port(sequence, port_info) >= 0 && !(found_output && found_input)) {
      const char   *name = snd_seq_port_info_get_name(port_info);
      unsigned int caps  = snd_seq_port_info_get_capability(port_info);
      int          port  = snd_seq_port_info_get_port(port_info);
      if (name == NULL || !has_launchpad_name(name)) {
        continue;
      }
      if (!found_output &&
          (caps & (SND_SEQ_PORT_CAP_WRITE | SND_SEQ_PORT_CAP_SUBS_WRITE)) ==
              (SND_SEQ_PORT_CAP_WRITE | SND_SEQ_PORT_CAP_SUBS_WRITE)) {
        *output_client = client;
        *output_port   = port;
        snprintf(output_name, MIDI_DEVICE_NAME_SIZE, "%s", name);
        found_output = 1;
      }
      if (!found_input &&
          (caps & (SND_SEQ_PORT_CAP_READ | SND_SEQ_PORT_CAP_SUBS_READ)) ==
              (SND_SEQ_PORT_CAP_READ | SND_SEQ_PORT_CAP_SUBS_READ)) {
        *input_client = client;
        *input_port   = port;
        found_input   = 1;
      }
    }
  }
  result = found_output && found_input;
  snd_seq_port_info_free(port_info);
  snd_seq_client_info_free(client_info);
  return result;
}

/**
 * @brief Dispatch supported ALSA input events to the shared shell.
 *
 * Converts note and controller events into three-byte MIDI channel messages.
 * @param[in] device Backend device receiving the event.
 * @param[in] event ALSA sequencer event.
 */
static void dispatch_input_event(MidiDevice *device, const snd_seq_event_t *event) {
  uint8_t status;
  uint8_t note;
  uint8_t velocity;
  switch (event->type) {
  case SND_SEQ_EVENT_NOTEON:
    status   = (uint8_t)(0x90 | event->data.note.channel);
    note     = event->data.note.note;
    velocity = event->data.note.velocity;
    break;
  case SND_SEQ_EVENT_NOTEOFF:
    status   = (uint8_t)(0x80 | event->data.note.channel);
    note     = event->data.note.note;
    velocity = event->data.note.velocity;
    break;
  case SND_SEQ_EVENT_CONTROLLER:
    status   = (uint8_t)(0xB0 | event->data.control.channel);
    note     = (uint8_t)event->data.control.param;
    velocity = (uint8_t)event->data.control.value;
    break;
  default:
    return;
  }
  device->callback(device->context, status, note, velocity);
}

/**
 * @brief Read MIDI events on the ALSA input thread.
 *
 * Polls the nonblocking sequencer queue until midi_close requests shutdown.
 * @param[in] context MidiDevice pointer passed to pthread_create.
 * @return Always NULL.
 */
static void *input_thread(void *context) {
  MidiDevice            *device = (MidiDevice *)context;
  const struct timespec delay   = {0, 10000000};
  while (atomic_load(&device->running)) {
    snd_seq_event_t *event = NULL;
    int result;
    pthread_mutex_lock(&device->lock);
    result = snd_seq_event_input(device->sequence, &event);
    pthread_mutex_unlock(&device->lock);
    if (result > 0 && event != NULL) {
      dispatch_input_event(device, event);
      snd_seq_free_event(event);
    } else if (result < 0 && result != -EAGAIN) {
      report_error("Reading MIDI input", result);
      atomic_store(&device->running, 0);
      break;
    } else {
      nanosleep(&delay, NULL);
    }
  }
  return NULL;
}

/**
 * @brief Open matching Linux ALSA sequencer ports.
 *
 * Creates local ports, connects the Launchpad, then starts input polling.
 * @param[out] device Receives the allocated backend handle.
 * @param[in] callback Callback for received channel MIDI messages.
 * @param[in] context Opaque callback context.
 * @retval 1 Ports opened successfully.
 * @retval 0 Invalid arguments or an ALSA/POSIX operation failed.
 */
int midi_open(MidiDevice **device, MidiInputCallback callback, void *context) {
  MidiDevice *result;
  int        status;
  int        input_client;
  int        input_target_port;

  if (device == NULL || callback == NULL) {
    return 0;
  }
  *device = NULL;
  result  = (MidiDevice *)calloc(1, sizeof(*result));
  if (result == NULL) {
    fprintf(stderr, "Unable to allocate MIDI device state.\n");
    return 0;
  }
  result->output_port = -1;
  result->input_port  = -1;
  result->callback    = callback;
  result->context     = context;
  if (pthread_mutex_init(&result->lock, NULL) != 0) {
    fprintf(stderr, "Unable to initialize MIDI output lock.\n");
    free(result);
    return 0;
  }

  status = snd_seq_open(&result->sequence, "default", SND_SEQ_OPEN_DUPLEX, 0);
  if (status < 0) {
    report_error("Opening ALSA sequencer", status);
    pthread_mutex_destroy(&result->lock);
    free(result);
    return 0;
  }
  status = snd_seq_set_client_name(result->sequence, "Launchpad shell");
  if (status < 0) {
    report_error("Naming ALSA sequencer client", status);
    goto fail;
  }
  status = snd_seq_create_simple_port(
      result->sequence, "Launchpad shell output",
      SND_SEQ_PORT_CAP_READ | SND_SEQ_PORT_CAP_SUBS_READ,
      SND_SEQ_PORT_TYPE_MIDI_GENERIC | SND_SEQ_PORT_TYPE_APPLICATION);
  if (status < 0) {
    report_error("Creating ALSA output port", status);
    goto fail;
  }
  result->output_port = status;
  status = snd_seq_create_simple_port(
      result->sequence, "Launchpad shell input",
      SND_SEQ_PORT_CAP_WRITE | SND_SEQ_PORT_CAP_SUBS_WRITE,
      SND_SEQ_PORT_TYPE_MIDI_GENERIC | SND_SEQ_PORT_TYPE_APPLICATION);
  if (status < 0) {
    report_error("Creating ALSA input port", status);
    goto fail;
  }
  result->input_port = status;

  if (!find_launchpad_ports(result->sequence, &result->target_output_client,
                            &result->target_output_port, &input_client,
                            &input_target_port, result->name)) {
    fprintf(stderr, "Could not find both compatible Launchpad ALSA ports.\n");
    goto fail;
  }
  status = snd_seq_connect_to(result->sequence, result->output_port,
                              result->target_output_client,
                              result->target_output_port);
  if (status < 0) {
    report_error("Connecting ALSA MIDI output", status);
    goto fail;
  }
  status = snd_seq_connect_from(result->sequence, result->input_port, input_client, input_target_port);
  if (status < 0) {
    report_error("Connecting ALSA MIDI input", status);
    goto fail;
  }
  status = snd_midi_event_new(32, &result->encoder);
  if (status < 0) {
    report_error("Creating ALSA MIDI encoder", status);
    goto fail;
  }
  status = snd_seq_nonblock(result->sequence, 1);
  if (status < 0) {
    report_error("Setting ALSA input nonblocking", status);
    goto fail;
  }

  atomic_init(&result->running, 1);
  status = pthread_create(&result->input_thread, NULL, input_thread, result);
  if (status != 0) {
    fprintf(stderr, "Unable to start MIDI input thread (error %d).\n", status);
    goto fail;
  }
  result->thread_started = 1;
  *device                = result;
  printf("Connected to: %s (out), Launchpad MIDI input\n", result->name);
  return 1;

fail:
  if (result->encoder != NULL) {
    snd_midi_event_free(result->encoder);
  }
  if (result->sequence != NULL) {
    snd_seq_close(result->sequence);
  }
  pthread_mutex_destroy(&result->lock);
  free(result);
  return 0;
}

/**
 * @brief Stop input delivery and close Linux ALSA ports.
 *
 * Joins the input thread before releasing sequencer resources.
 * @param[in,out] device Device handle returned by midi_open.
 */
void midi_close(MidiDevice *device) {
  int status;
  if (device == NULL) {
    return;
  }
  atomic_store(&device->running, 0);
  if (device->thread_started) {
    status = pthread_join(device->input_thread, NULL);
    if (status != 0) {
      fprintf(stderr, "Unable to stop MIDI input thread (error %d).\n", status);
      return;
    }
  }
  if (device->encoder != NULL) {
    snd_midi_event_free(device->encoder);
  }
  if (device->sequence != NULL) {
    status = snd_seq_close(device->sequence);
    if (status < 0) {
      report_error("Closing ALSA sequencer", status);
    }
  }
  pthread_mutex_destroy(&device->lock);
  free(device);
}

/**
 * @brief Send a complete SysEx message to the ALSA output port.
 *
 * Emits the message as one ALSA sequencer SysEx event.
 * @param[in] device Open MIDI device.
 * @param[in] data Complete SysEx message bytes.
 * @param[in] length Number of bytes in data.
 * @retval 1 Message sent.
 * @retval 0 Invalid message or ALSA send failed.
 */
int midi_send_sysex(MidiDevice *device, const uint8_t *data, size_t length) {
  snd_seq_event_t event;
  int             status;
  if (device == NULL || data == NULL || length < 2 ||
      data[0] != 0xF0 || data[length - 1] != 0xF7) {
    fprintf(stderr, "Invalid SysEx message.\n");
    return 0;
  }
  snd_seq_ev_clear(&event);
  snd_seq_ev_set_source(&event, device->output_port);
  snd_seq_ev_set_dest(&event, device->target_output_client, device->target_output_port);
  snd_seq_ev_set_sysex(&event, (size_t)length, (void *)data);
  pthread_mutex_lock(&device->lock);
  status = snd_seq_event_output_direct(device->sequence, &event);
  pthread_mutex_unlock(&device->lock);
  if (status < 0) {
    report_error("Sending SysEx", status);
    return 0;
  }
  return 1;
}

/**
 * @brief Send a packed short MIDI message to the ALSA output port.
 *
 * Encodes the packed bytes into one ALSA sequencer event.
 * @param[in] device Open MIDI device.
 * @param[in] message Packed MIDI message.
 * @retval 1 Message sent.
 * @retval 0 Invalid message or ALSA send failed.
 */
int midi_send_short(MidiDevice *device, uint32_t message) {
  uint8_t         bytes[3];
  size_t          length;
  snd_seq_event_t event;
  long            encoded;
  int             status;
  uint8_t         command = (uint8_t)(message & 0xF0);
  uint8_t         first   = (uint8_t)(message & 0xFF);

  if (device == NULL || first < 0x80) {
    fprintf(stderr, "Invalid short MIDI message.\n");
    return 0;
  }
  if (first < 0xF0) {
    length = command == 0xC0 || command == 0xD0 ? 2 : 3;
  } else if (first == 0xF2) {
    length = 3;
  } else if (first == 0xF1 || first == 0xF3) {
    length = 2;
  } else {
    length = 1;
  }
  bytes[0] = first;
  bytes[1] = (uint8_t)((message >> 8) & 0xFF);
  bytes[2] = (uint8_t)((message >> 16) & 0xFF);
  snd_seq_ev_clear(&event);

  pthread_mutex_lock(&device->lock);
  encoded = snd_midi_event_encode(device->encoder, bytes, (long)length, &event);
  if (encoded != (long)length) {
    pthread_mutex_unlock(&device->lock);
    if (encoded < 0) {
      report_error("Encoding MIDI message", (int)encoded);
    } else {
      fprintf(stderr, "Incomplete short MIDI message.\n");
    }
    return 0;
  }
  snd_seq_ev_set_source(&event, device->output_port);
  snd_seq_ev_set_dest(&event, device->target_output_client, device->target_output_port);
  status = snd_seq_event_output_direct(device->sequence, &event);
  pthread_mutex_unlock(&device->lock);
  if (status < 0) {
    report_error("Sending MIDI", status);
    return 0;
  }
  return 1;
}

/**
 * @brief Return the selected Linux ALSA output port name.
 *
 * The name is stored in the backend device state.
 * @param[in] device Open MIDI device.
 * @return Output port name, or "Launchpad" if unavailable.
 */
const char *midi_device_name(const MidiDevice *device) {
  return device != NULL && device->name[0] != '\0' ? device->name : "Launchpad";
}
