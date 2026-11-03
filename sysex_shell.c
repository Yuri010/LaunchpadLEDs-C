#include "midi_backend.h"

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdatomic.h>
#include <string.h>
#include <time.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#endif

#define MAX_LINE   1024
#define MAX_ARGS   512
#define MAX_SYSEX  1024
#define NOTE_COUNT 81

static const uint8_t sysex_header[] = {0xF0, 0x00, 0x20, 0x29, 0x02, 0x18};

typedef struct {
  const char *name;
  uint8_t    layout;
  uint8_t    note;
  uint8_t    inactive[3];
  uint8_t    active[3];
} Mode;

typedef struct {
  MidiDevice *midi;
  atomic_int listener_active;
  int        current_mode;
} AppState;

static const Mode modes[] = {
    {"session", 0x00, 108, {0, 32, 0}, {0, 63, 0}},
    {"user1", 0x01, 109, {5, 0, 32}, {10, 0, 63}},
    {"user2", 0x02, 110, {32, 0, 32}, {63, 0, 63}},
    {"mixer", 0x04, 111, {0, 21, 32}, {0, 42, 63}},
};

static const uint8_t mode_statuses[] = {0x90, 0x95, 0x9D, 0xB0};
static atomic_flag   mode_lock       = ATOMIC_FLAG_INIT;

/**
 * @brief Compare two ASCII strings without regard to case.
 *
 * Stops at the first differing character or the shared string terminator.
 * @param[in] left First string.
 * @param[in] right Second string.
 * @retval 0 Strings are equal ignoring ASCII case.
 * @retval nonzero Strings differ.
 */
static int compare_case_insensitive(const char *left, const char *right) {
  while (*left != '\0' && *right != '\0') {
    int a = tolower((unsigned char)*left++);
    int b = tolower((unsigned char)*right++);
    if (a != b) {
      return a - b;
    }
  }
  return (unsigned char)*left - (unsigned char)*right;
}

/**
 * @brief Acquire the shared mode-update lock.
 *
 * Prevents simultaneous MIDI callbacks and shell commands from interleaving mode updates.
 */
static void lock_mode(void) {
  while (atomic_flag_test_and_set_explicit(&mode_lock, memory_order_acquire)) {
  }
}

/**
 * @brief Release the shared mode-update lock.
 *
 * Publishes the updated current mode to waiting callers.
 */
static void unlock_mode(void) {
  atomic_flag_clear_explicit(&mode_lock, memory_order_release);
}

/**
 * @brief Derive the Launchpad note number for a pad index.
 *
 * The top row has eight mode notes; the pad grid has nine columns per row.
 * @param[in] index Pad index in the shared command order.
 * @return MIDI note for index.
 */
static uint8_t pad_note(size_t index) {
  if (index < 8) {
    return (uint8_t)(104 + index);
  }
  return (uint8_t)(81 - 10 * ((index - 8) / 9) + (index - 8) % 9);
}

/**
 * @brief Send a Launchpad SysEx payload.
 *
 * Adds the product header and SysEx terminator around the payload.
 * @param[in] app Shell state containing the open MIDI device.
 * @param[in] payload Launchpad command bytes.
 * @param[in] length Number of payload bytes.
 * @retval 1 Message sent.
 * @retval 0 Message is too large or the MIDI backend failed.
 */
static int send_payload(AppState *app, const uint8_t *payload, size_t length) {
  uint8_t message[MAX_SYSEX];
  size_t  header_length = sizeof(sysex_header);
  if (length + header_length + 1 > sizeof(message)) {
    fprintf(stderr, "SysEx message is too long.\n");
    return 0;
  }
  memcpy(message, sysex_header, header_length);
  memcpy(message + header_length, payload, length);
  message[header_length + length] = 0xF7;
  return midi_send_sysex(app->midi, message, header_length + length + 1);
}

/**
 * @brief Find a mode name without regard to case.
 *
 * Checks the four Launchpad layouts supported by the shell.
 * @param[in] name User-supplied mode name.
 * @return Mode index, or -1 if no mode matches.
 */
static int find_mode(const char *name) {
  size_t i;
  for (i = 0; i < sizeof(modes) / sizeof(modes[0]); ++i) {
    if (compare_case_insensitive(name, modes[i].name) == 0) {
      return (int)i;
    }
  }
  return -1;
}

/**
 * @brief Change the Launchpad layout and color its mode buttons.
 *
 * Sends the same layout and mode-button lighting sequence as the Python class.
 * @param[in,out] app Shell state whose current mode is updated on success.
 * @param[in] index Requested mode index.
 * @retval 1 Mode changed or already active.
 * @retval 0 Index is invalid or a MIDI message failed.
 */
static int set_mode(AppState *app, int index) {
  uint8_t payload[5];
  size_t  i;
  if (index < 0 || index >= (int)(sizeof(modes) / sizeof(modes[0]))) {
    return 0;
  }
  lock_mode();
  if (app->current_mode == index) {
    unlock_mode();
    return 1;
  }
  if (app->current_mode >= 0) {
    payload[0] = 0x0B;
    payload[1] = modes[app->current_mode].note;
    payload[2] = payload[3] = payload[4] = 0;
    if (!send_payload(app, payload, sizeof(payload))) {
      unlock_mode();
      return 0;
    }
  }
  payload[0] = 0x22;
  payload[1] = modes[index].layout;
  if (!send_payload(app, payload, 2)) {
    unlock_mode();
    return 0;
  }
  for (i = 0; i < sizeof(modes) / sizeof(modes[0]); ++i) {
    const uint8_t *rgb;
    static const uint8_t off[] = {0, 0, 0};
    if ((int)i == index) {
      rgb = modes[i].active;
    } else if (index == 0 || index == 3) {
      rgb = modes[i].inactive;
    } else {
      rgb = off;
    }
    payload[0] = 0x0B;
    payload[1] = modes[i].note;
    memcpy(payload + 2, rgb, 3);
    if (!send_payload(app, payload, sizeof(payload))) {
      unlock_mode();
      return 0;
    }
  }
  app->current_mode = index;
  unlock_mode();
  return 1;
}

/**
 * @brief Process a MIDI message received from the backend.
 *
 * Mode-pad presses change the current layout; listen output is user-toggleable.
 * @param[in] context AppState pointer supplied to midi_open.
 * @param[in] status MIDI status byte.
 * @param[in] note Note or controller number.
 * @param[in] velocity Note velocity or controller value.
 */
static void on_midi_input(void *context, uint8_t status, uint8_t note, uint8_t velocity) {
  AppState *app = (AppState *)context;
  size_t i;
  if (velocity != 0) {
    for (i = 0; i < sizeof(modes) / sizeof(modes[0]); ++i) {
      if (note == modes[i].note) {
        set_mode(app, (int)i);
        break;
      }
    }
  }
  if (atomic_load(&app->listener_active)) {
    const char *status_name = NULL;
    for (i = 0; i < sizeof(modes) / sizeof(modes[0]); ++i) {
      if (status == mode_statuses[i]) {
        status_name = modes[i].name;
        break;
      }
    }
    if (status_name != NULL) {
      printf("MIDI Input: [%s] [%u, %u, %u]\n", status_name, status, note, velocity);
    } else {
      printf("MIDI Input: [0x%02X] [%u, %u, %u]\n", status, status, note, velocity);
    }
    fflush(stdout);
  }
}

/**
 * @brief Parse a decimal or 0x-prefixed integer within an inclusive range.
 *
 * Reports invalid values to stderr and writes the parsed value on success.
 * @param[in] text Input token.
 * @param[in] minimum Inclusive lower bound.
 * @param[in] maximum Inclusive upper bound.
 * @param[in] name Value label used in the error message.
 * @param[out] value Receives the parsed integer.
 * @retval 1 Input is valid and within range.
 * @retval 0 Input is malformed or out of range.
 */
static int parse_int(const char *text, long minimum, long maximum, const char *name, long *value) {
  char *end;
  int base = text[0] == '0' && (text[1] == 'x' || text[1] == 'X') ? 16 : 10;
  long parsed;
  errno  = 0;
  parsed = strtol(text, &end, base);
  if (text == end || *end != '\0' || errno == ERANGE || parsed < minimum || parsed > maximum) {
    fprintf(stderr, "%s must be between %ld and %ld.\n", name, minimum, maximum);
    return 0;
  }
  *value = parsed;
  return 1;
}

/**
 * @brief Parse comma-separated note numbers.
 *
 * Accepts zero through 127 and stores at most 128 notes.
 * @param[in,out] text Mutable comma-separated note list.
 * @param[out] notes Receives the parsed note numbers.
 * @param[out] count Receives the number of parsed notes.
 * @retval 1 At least one valid note was parsed.
 * @retval 0 A note is invalid or the list is empty or too long.
 */
static int parse_note_list(char *text, uint8_t *notes, size_t *count) {
  char *token = strtok(text, ",");
  *count      = 0;
  while (token != NULL) {
    long note;
    char *end;
    while (isspace((unsigned char)*token)) {
      ++token;
    }
    end = token + strlen(token);
    while (end > token && isspace((unsigned char)end[-1])) {
      *--end = '\0';
    }
    if (*token == '\0' || !parse_int(token, 0, 127, "Note", &note) || *count == 128) {
      return 0;
    }
    notes[(*count)++] = (uint8_t)note;
    token             = strtok(NULL, ",");
  }
  if (*count == 0) {
    fprintf(stderr, "Invalid note list.\n");
    return 0;
  }
  return 1;
}

/**
 * @brief Parse one hexadecimal byte token.
 *
 * Accepts tokens with or without a 0x prefix.
 * @param[in] text Hexadecimal token.
 * @param[out] value Receives the parsed byte.
 * @retval 1 Token is a valid byte.
 * @retval 0 Token is malformed or exceeds 255.
 */
static int parse_hex_byte(const char *text, uint8_t *value) {
  char          *end;
  unsigned long parsed;
  errno = 0;
  parsed = strtoul(text, &end, 16);
  if (text == end || *end != '\0' || errno == ERANGE || parsed > 255) {
    return 0;
  }
  *value = (uint8_t)parsed;
  return 1;
}

/**
 * @brief Print the interactive command reference.
 *
 * Lists lighting, utility, and input-listener commands.
 */
static void print_help(void) {
  puts("\nLighting commands:\n"
       "  solid <r> <g> <b> [notes]   Set RGB (0-63); notes is comma-separated\n"
       "  solid <palette> [notes]     Set palette color (0-127)\n"
       "  pulse <color>               Pulse all pads (color 0-127)\n"
       "  flash <color>               Flash all pads (color 0-127)\n"
       "  text <color> <speed> <msg>  Scroll text (color 0-127, speed 0-7)\n"
       "  clear                       Turn off all pads\n"
       "\nUtility commands:\n"
       "  tempo <bpm> [count]         Send MIDI clock (40-240 BPM, default 32)\n"
       "  send <hex bytes...>         Send SysEx payload (header/F7 optional)\n"
       "  sendraw <hex bytes...>      Send raw SysEx or one short MIDI message\n"
       "  mode <session|user1|user2|mixer>\n"
       "  reconnect                   Reconnect to the Launchpad\n"
       "  consoleclear                Clear the console\n"
       "  listenon | listenoff        Toggle MIDI input messages\n"
       "  help | exit                 Show help or exit\n");
}

/**
 * @brief Set RGB or palette colors on pads.
 *
 * Uses the Launchpad all-pad palette command when no note list is supplied.
 * @param[in] app Shell state with an open MIDI device.
 * @param[in] argc Number of command arguments.
 * @param[in] args Command argument tokens.
 * @retval 1 Color command completed.
 * @retval 0 Usage, validation, or MIDI send error.
 */
static int command_solid(AppState *app, int argc, char **args) {
  long    value[3];
  uint8_t notes[128];
  size_t  count = NOTE_COUNT;
  size_t  i;
  int     all_notes = 1;
  if (argc == 0) {
    puts("Usage: solid <r> <g> <b> [notes] or solid <palette> [notes]");
    return 0;
  }
  if (argc >= 3) {
    if (argc > 4 ||
        !parse_int(args[0], 0, 63, "Red", &value[0]) ||
        !parse_int(args[1], 0, 63, "Green", &value[1]) ||
        !parse_int(args[2], 0, 63, "Blue", &value[2])) {
      return 0;
    }
    if (argc == 4) {
      if (!parse_note_list(args[3], notes, &count)) {
        return 0;
      }
      all_notes = 0;
    } else {
      for (i = 0; i < count; ++i) {
        notes[i] = pad_note(i);
      }
    }
    for (i = 0; i < count; ++i) {
      uint8_t payload[] = {0x0B, notes[i], (uint8_t)value[0], (uint8_t)value[1], (uint8_t)value[2]};
      if (!send_payload(app, payload, sizeof(payload))) {
        return 0;
      }
    }
    printf("Notes set to RGB (%ld, %ld, %ld).\n", value[0], value[1], value[2]);
    return 1;
  }
  if (argc > 2 || !parse_int(args[0], 0, 127, "Palette", &value[0])) {
    if (argc > 2) {
      puts("Usage: solid <palette> [notes]");
    }
    return 0;
  }
  if (argc == 2) {
    if (!parse_note_list(args[1], notes, &count)) {
      return 0;
    }
    all_notes = 0;
  }
  if (all_notes) {
    uint8_t payload[] = {0x0E, (uint8_t)value[0]};
    if (!send_payload(app, payload, sizeof(payload))) {
      return 0;
    }
  } else {
    for (i = 0; i < count; ++i) {
      uint8_t payload[] = {0x0A, notes[i], (uint8_t)value[0]};
      if (!send_payload(app, payload, sizeof(payload))) {
        return 0;
      }
    }
  }
  printf("Notes set to palette color %ld.\n", value[0]);
  return 1;
}

/**
 * @brief Apply a pulse or flash effect to every pad.
 *
 * Sends the effect command once for each grid note.
 * @param[in] app Shell state with an open MIDI device.
 * @param[in] argc Number of command arguments.
 * @param[in] args Command argument tokens.
 * @param[in] effect Effect command byte.
 * @param[in] name Effect name for usage and confirmation.
 * @retval 1 Effect commands sent.
 * @retval 0 Usage, validation, or MIDI send error.
 */
static int command_effect(AppState *app, int argc, char **args, uint8_t effect, const char *name) {
  long   color;
  size_t i;
  if (argc != 1 || !parse_int(argc ? args[0] : "", 0, 127, "Color index", &color)) {
    printf("Usage: %s <color_index>\n", name);
    return 0;
  }
  for (i = 0; i < NOTE_COUNT; ++i) {
    uint8_t payload[] = {effect, 0x00, pad_note(i), (uint8_t)color};
    if (!send_payload(app, payload, sizeof(payload))) {
      return 0;
    }
  }
  printf("%s effect set to color index %ld.\n", name, color);
  return 1;
}

/**
 * @brief Display a scrolling text message.
 *
 * Joins remaining command arguments with spaces and sends 7-bit text bytes.
 * @param[in] app Shell state with an open MIDI device.
 * @param[in] argc Number of command arguments.
 * @param[in] args Color, speed, and message tokens.
 * @retval 1 Text command sent.
 * @retval 0 Usage, validation, character, or MIDI send error.
 */
static int command_text(AppState *app, int argc, char **args) {
  long    color;
  long    speed;
  uint8_t payload[MAX_LINE];
  size_t  length = 4;
  int     i;
  if (argc < 3 ||
      !parse_int(argc > 0 ? args[0] : "", 0, 127, "Color", &color) ||
      !parse_int(argc > 1 ? args[1] : "", 0, 7, "Speed", &speed)) {
    puts("Usage: text <color (0-127)> <speed (0-7)> <message>");
    return 0;
  }
  payload[0] = 0x14;
  payload[1] = (uint8_t)color;
  payload[2] = 0;
  payload[3] = (uint8_t)speed;
  for (i = 2; i < argc; ++i) {
    const unsigned char *character = (const unsigned char *)args[i];
    if (i > 2) {
      if (length >= sizeof(payload)) {
        fprintf(stderr, "Text message is too long.\n");
        return 0;
      }
      payload[length++] = ' ';
    }
    while (*character != '\0') {
      if (*character > 0x7F) {
        fprintf(stderr, "Text must use 7-bit ASCII characters.\n");
        return 0;
      }
      if (length >= sizeof(payload)) {
        fprintf(stderr, "Text message is too long.\n");
        return 0;
      }
      payload[length++] = *character++;
    }
  }
  if (!send_payload(app, payload, length)) {
    return 0;
  }
  printf("Displaying text (color %ld, speed %ld).\n", color, speed);
  return 1;
}

/**
 * @brief Send a user-provided Launchpad SysEx payload.
 *
 * Accepts a raw payload or a full matching product SysEx message.
 * @param[in] app Shell state with an open MIDI device.
 * @param[in] argc Number of byte tokens.
 * @param[in] args Hexadecimal byte tokens.
 * @retval 1 SysEx message sent.
 * @retval 0 Usage, validation, or MIDI send error.
 */
static int command_send(AppState *app, int argc, char **args) {
  uint8_t bytes[MAX_SYSEX];
  size_t  length = 0;
  int     i;
  if (argc == 0) {
    puts("Usage: send <hex bytes...>");
    return 0;
  }
  for (i = 0; i < argc; ++i) {
    if (length == sizeof(bytes) || !parse_hex_byte(args[i], &bytes[length])) {
      fprintf(stderr, "Invalid or excessive hex byte: %s\n", args[i]);
      return 0;
    }
    ++length;
  }
  if (bytes[0] == 0xF0) {
    if (length < sizeof(sysex_header) ||
        memcmp(bytes, sysex_header, sizeof(sysex_header)) != 0) {
      fprintf(stderr, "Full SysEx must begin F0 00 20 29 02 18.\n");
      return 0;
    }
    if (length > sizeof(sysex_header) && bytes[length - 1] == 0xF7) {
      --length;
    }
    if (length <= sizeof(sysex_header)) {
      fprintf(stderr, "SysEx message has no payload.\n");
      return 0;
    }
    memmove(bytes, bytes + sizeof(sysex_header), length - sizeof(sysex_header));
    length -= sizeof(sysex_header);
  } else if (bytes[length - 1] == 0xF7) {
    --length;
  }
  if (!send_payload(app, bytes, length)) {
    return 0;
  }
  puts("SysEx sent.");
  return 1;
}

/**
 * @brief Send a complete raw SysEx or short MIDI message.
 *
 * Raw SysEx bytes are preserved; short messages use the backend MIDI API.
 * @param[in] app Shell state with an open MIDI device.
 * @param[in] argc Number of byte tokens.
 * @param[in] args Hexadecimal byte tokens.
 * @retval 1 MIDI message sent.
 * @retval 0 Usage, validation, or MIDI send error.
 */
static int command_sendraw(AppState *app, int argc, char **args) {
  uint8_t bytes[MAX_SYSEX];
  size_t  length = 0;
  int i;
  if (argc == 0) {
    puts("Usage: sendraw <hex bytes...>");
    return 0;
  }
  for (i = 0; i < argc; ++i) {
    if (length == sizeof(bytes) || !parse_hex_byte(args[i], &bytes[length])) {
      fprintf(stderr, "Invalid or excessive hex byte: %s\n", args[i]);
      return 0;
    }
    ++length;
  }
  if (bytes[0] == 0xF0) {
    if (length < 2 || bytes[length - 1] != 0xF7) {
      fprintf(stderr, "Raw SysEx must end with F7.\n");
      return 0;
    }
    if (!midi_send_sysex(app->midi, bytes, length)) {
      return 0;
    }
  } else {
    uint32_t message;
    if (length > 3 || bytes[0] < 0x80) {
      fprintf(stderr, "Raw MIDI must be SysEx or one short MIDI message.\n");
      return 0;
    }
    message = bytes[0];
    if (length > 1) {
      message |= (uint32_t)bytes[1] << 8;
    }
    if (length > 2) {
      message |= (uint32_t)bytes[2] << 16;
    }
    if (!midi_send_short(app->midi, message)) {
      return 0;
    }
  }
  puts("Raw MIDI message sent.");
  return 1;
}

/**
 * @brief Send MIDI clock at a requested tempo.
 *
 * Emits one clock pulse every 60/(24*BPM) seconds.
 * @param[in] app Shell state with an open MIDI device.
 * @param[in] argc Number of command arguments.
 * @param[in] args BPM and optional message count.
 * @retval 1 All clock messages sent.
 * @retval 0 Usage, validation, or MIDI send error.
 */
static int command_tempo(AppState *app, int argc, char **args) {
  long            bpm;
  long            count = 32;
  long            i;
  struct timespec delay;
  if ((argc != 1 && argc != 2) ||
      !parse_int(argc > 0 ? args[0] : "", 40, 240, "BPM", &bpm) ||
      (argc == 2 && !parse_int(args[1], 1, 10000, "Count", &count))) {
    puts("Usage: tempo <bpm (40-240)> [count (1-10000)]");
    return 0;
  }
  delay.tv_sec = 0;
  delay.tv_nsec = (long)(60000000000.0 / (bpm * 24));
  for (i = 0; i < count; ++i) {
    if (!midi_send_short(app->midi, 0xF8)) {
      return 0;
    }
#ifdef _WIN32
    Sleep((DWORD)(delay.tv_nsec / 1000000));
#else
    nanosleep(&delay, NULL);
#endif
  }
  printf("Sent %ld MIDI clock messages.\n", count);
  return 1;
}

/**
 * @brief Execute one parsed shell command.
 *
 * Dispatches commands and returns a negative value only for exit.
 * @param[in,out] app Shared shell state.
 * @param[in] argc Number of tokens in argv.
 * @param[in,out] argv Command and argument tokens.
 * @retval -1 User requested exit.
 * @retval 0 Command handled or an unknown command was reported.
 */
static int run_command(AppState *app, int argc, char **argv) {
  char   *command = argv[0];
  size_t i;
  for (i = 0; command[i] != '\0'; ++i) {
    command[i] = (char)tolower((unsigned char)command[i]);
  }
  if (strcmp(command, "help") == 0 || strcmp(command, "?") == 0) {
    print_help();
  } else if (strcmp(command, "exit") == 0) {
    return -1;
  } else if (strcmp(command, "clear") == 0) {
    const uint8_t payload[] = {0x0E, 0x00};
    if (argc == 1 && send_payload(app, payload, sizeof(payload))) {
      puts("All pads cleared.");
    } else if (argc != 1) {
      puts("Usage: clear");
    }
  } else if (strcmp(command, "solid") == 0) {
    command_solid(app, argc - 1, argv + 1);
  } else if (strcmp(command, "pulse") == 0) {
    command_effect(app, argc - 1, argv + 1, 0x28, "pulse");
  } else if (strcmp(command, "flash") == 0) {
    command_effect(app, argc - 1, argv + 1, 0x23, "flash");
  } else if (strcmp(command, "text") == 0) {
    command_text(app, argc - 1, argv + 1);
  } else if (strcmp(command, "tempo") == 0) {
    command_tempo(app, argc - 1, argv + 1);
  } else if (strcmp(command, "send") == 0) {
    command_send(app, argc - 1, argv + 1);
  } else if (strcmp(command, "sendraw") == 0) {
    command_sendraw(app, argc - 1, argv + 1);
  } else if (strcmp(command, "mode") == 0) {
    int mode;
    if (argc != 2) {
      puts("Usage: mode <session|user1|user2|mixer>");
    } else if ((mode = find_mode(argv[1])) < 0) {
      puts("Unknown mode. Choose session, user1, user2, or mixer.");
    } else if (set_mode(app, mode)) {
      printf("Set mode to %s.\n", modes[mode].name);
    }
  } else if (strcmp(command, "reconnect") == 0) {
    if (argc != 1) {
      puts("Usage: reconnect");
    } else {
      const uint8_t clear_payload[] = {0x0E, 0x00};
      send_payload(app, clear_payload, sizeof(clear_payload));
      midi_close(app->midi);
      app->midi         = NULL;
      app->current_mode = -1;
      if (midi_open(&app->midi, on_midi_input, app) && set_mode(app, 0)) {
        puts("Reconnected to Launchpad.");
      } else {
        midi_close(app->midi);
        app->midi = NULL;
      }
    }
  } else if (strcmp(command, "consoleclear") == 0) {
    if (argc != 1) {
      puts("Usage: consoleclear");
#ifdef _WIN32
    } else if (system("cls") != 0) {
#else
    } else if (system("clear") != 0) {
#endif
      fprintf(stderr, "Unable to clear the console.\n");
    }
  } else if (strcmp(command, "listenon") == 0) {
    if (atomic_exchange(&app->listener_active, 1)) {
      puts("Input listener already running.");
    } else {
      puts("Input listener started.");
    }
  } else if (strcmp(command, "listenoff") == 0) {
    if (atomic_exchange(&app->listener_active, 0)) {
      puts("Input listener stopped.");
    } else {
      puts("Input listener is not running.");
    }
  } else {
    printf("Unknown command: %s. Try 'help'.\n", command);
  }
  return 0;
}

/**
 * @brief Run the Launchpad command shell.
 *
 * Connects to the device, selects session mode, and releases it on exit.
 * @param[in] none No parameters.
 * @retval 0 Shell exited normally.
 * @retval 1 MIDI device or initial mode could not be opened.
 */
int main(void) {
  AppState app = {NULL, ATOMIC_VAR_INIT(0), -1};
  char     line[MAX_LINE];
  int      should_exit = 0;
  if (!midi_open(&app.midi, on_midi_input, &app)) {
    return 1;
  }
  if (!set_mode(&app, 0)) {
    const uint8_t clear_payload[] = {0x0E, 0x00};
    send_payload(&app, clear_payload, sizeof(clear_payload));
    midi_close(app.midi);
    return 1;
  }
  puts("Type 'help' for commands.");
  while (!should_exit) {
    char *argv[MAX_ARGS];
    char *token;
    int  argc = 0;
    printf("%s> ", midi_device_name(app.midi));
    fflush(stdout);
    if (fgets(line, sizeof(line), stdin) == NULL) {
      break;
    }
    if (strchr(line, '\n') == NULL && !feof(stdin)) {
      int character;
      while ((character = getchar()) != '\n' && character != EOF) {
      }
      fprintf(stderr, "Input line is too long.\n");
      continue;
    }
    token = strtok(line, " \t\r\n");
    while (token != NULL && argc < MAX_ARGS) {
      argv[argc++] = token;
      token        = strtok(NULL, " \t\r\n");
    }
    if (argc > 0) {
      should_exit = run_command(&app, argc, argv) < 0;
    }
  }
  if (app.midi != NULL) {
    const uint8_t clear_payload[] = {0x0E, 0x00};
    send_payload(&app, clear_payload, sizeof(clear_payload));
    midi_close(app.midi);
  }
  puts("Exiting.");
  return 0;
}
