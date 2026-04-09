/*
 * evdev_probe.c — Minimal evdev key-code sniffer for MMF.
 *
 * Run on the device:  ./evdev-probe
 * Press every button; it prints the evdev code and name for each.
 * Press POWER (hold ~1 s) or Ctrl-C to quit.
 *
 * Usage:
 *   ./evdev-probe          # probe /dev/input/event0-7
 *   ./evdev-probe /dev/input/event2   # probe a single device
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <linux/input.h>
#include <signal.h>
#include <errno.h>

static volatile int running = 1;
static void handle_sig(int s) { (void)s; running = 0; }

/* Best-effort name table (subset of linux/input-event-codes.h). */
static const char *key_name(int code) {
    switch (code) {
        case 1:   return "KEY_ESC";
        case 2:   return "KEY_1";
        case 3:   return "KEY_2";
        case 4:   return "KEY_3";
        case 5:   return "KEY_4";
        case 6:   return "KEY_5";
        case 14:  return "KEY_BACKSPACE";
        case 15:  return "KEY_TAB";
        case 16:  return "KEY_Q";
        case 17:  return "KEY_W";
        case 18:  return "KEY_E";
        case 19:  return "KEY_R";
        case 20:  return "KEY_T";
        case 28:  return "KEY_ENTER";
        case 29:  return "KEY_LEFTCTRL";
        case 30:  return "KEY_A";
        case 31:  return "KEY_S";
        case 42:  return "KEY_LEFTSHIFT";
        case 44:  return "KEY_Z";
        case 45:  return "KEY_X";
        case 46:  return "KEY_C";
        case 54:  return "KEY_RIGHTSHIFT";
        case 56:  return "KEY_LEFTALT";
        case 57:  return "KEY_SPACE";
        case 97:  return "KEY_RIGHTCTRL";
        case 100: return "KEY_RIGHTALT";
        case 103: return "KEY_UP";
        case 105: return "KEY_LEFT";
        case 106: return "KEY_RIGHT";
        case 108: return "KEY_DOWN";
        case 113: return "KEY_MUTE";
        case 114: return "KEY_VOLUMEDOWN";
        case 115: return "KEY_VOLUMEUP";
        case 116: return "KEY_POWER";
        default:  return "?";
    }
}

int main(int argc, char *argv[]) {
    signal(SIGINT,  handle_sig);
    signal(SIGTERM, handle_sig);

    int fds[8];
    int nfds = 0;

    if (argc > 1) {
        /* Single device specified on command line. */
        int fd = open(argv[1], O_RDONLY | O_NONBLOCK);
        if (fd < 0) { perror(argv[1]); return 1; }
        fds[nfds++] = fd;
        printf("Opened %s (fd %d)\n", argv[1], fd);
    } else {
        /* Probe event0..event7. */
        for (int i = 0; i < 8; i++) {
            char path[32];
            snprintf(path, sizeof(path), "/dev/input/event%d", i);
            int fd = open(path, O_RDONLY | O_NONBLOCK);
            if (fd >= 0) {
                fds[nfds++] = fd;
                printf("Opened %s (fd %d)\n", path, fd);
            }
        }
    }

    if (nfds == 0) {
        fprintf(stderr, "No input devices found.\n");
        return 1;
    }

    printf("\nPress buttons now.  POWER (hold) or Ctrl-C to quit.\n");
    printf("%-12s  %-6s  %-4s  %s\n", "ACTION", "TYPE", "CODE", "NAME");
    printf("-------------------------------------------\n");

    while (running) {
        for (int i = 0; i < nfds; i++) {
            struct input_event ev;
            ssize_t n = read(fds[i], &ev, sizeof(ev));
            if (n != sizeof(ev)) continue;

            if (ev.type == EV_KEY) {
                const char *action = (ev.value == 1) ? "PRESS" :
                                     (ev.value == 0) ? "RELEASE" :
                                     (ev.value == 2) ? "REPEAT" : "???";
                printf("%-12s  EV_KEY  %-4d  %s\n", action, ev.code, key_name(ev.code));
                fflush(stdout);

                /* Quit on POWER release. */
                if (ev.code == 116 && ev.value == 0) running = 0;
            } else if (ev.type == EV_ABS) {
                printf("%-12s  EV_ABS  axis=%-3d  value=%d\n",
                       "ABS", ev.code, ev.value);
                fflush(stdout);
            }
        }
        usleep(10000); /* 10 ms poll */
    }

    for (int i = 0; i < nfds; i++) close(fds[i]);
    printf("\nDone.\n");
    return 0;
}
