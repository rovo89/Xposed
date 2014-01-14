/*
 * Detects input combinations for recovering from bootloops.
 *
 * The safemode trigger is detected if exactly one of the physical keys is pressed in
 * the first 2 seconds after detection startup (or already held down), and a total of
 * 5 consecutive presses of that same key are performed in the subsequent 5 seconds.
 *
 * 2 short vibrations are performed when the first key is pressed; an additional
 * vibration is performed for each subsequent press of the same key, and a final
 * long vibration is performed if the trigger was successful.
 *
 * The initial 2-second delay can be disabled through configuration; in that case,
 * one of the keys must already be pressed when the detection starts, otherwise
 * the detection fails and no delays are introduced.
 *
 * References:
 *   /frameworks/base/services/input/EventHub.cpp (AOSP)
 *   /include/uapi/linux/input.h (Linux)
 *   Using the Input Subsystem, Linux Journal
 */
#include "xposed_safemode.h"

#include <dirent.h>
#include <fcntl.h>
#include <linux/input.h>
#include <sys/epoll.h>
#include <time.h>
#include <stdio.h>

#define INITIAL_DELAY 2
#define DETECTION_TIMEOUT 5

#define DETECTION_PRESSES 5

#define VIBRATOR_CONTROL "/sys/class/timed_output/vibrator/enable"
#define VIBRATION_SHORT 150
#define VIBRATION_LONG 500

static const char *DEVICE_PATH = "/dev/input";
#define MAX_DEVICES 4

#define test_bit(bit, array)    (array[bit/8] & (1<<(bit%8)))

static const int physical_keycodes[] = { KEY_VOLUMEDOWN, KEY_VOLUMEUP, KEY_POWER,
                                         KEY_HOME, KEY_BACK, KEY_MENU, KEY_CAMERA };



static void vibrate(int count, int duration_ms, int interval_ms) {
    int fd;
    int len;
    char value[30];

    if ((fd = open(VIBRATOR_CONTROL, O_RDWR)) < 0)
        // Failed to open the control file, ignore it
        return;

    len = sprintf(value, "%d\n", duration_ms);
    for (int i = 0; i < count; i++) {
        if (i != 0)
            // Pause between the several vibrations
            usleep((duration_ms + interval_ms) * 1000);
        // Vibrate (asynchronously)
        write(fd, value, len);
    }
    close(fd);
}



static int openKeyDevices(int *fds, int max_fds, int *pressedKey) {
    char devname[PATH_MAX];
    char *filename;
    DIR *dir;
    struct dirent *de;

    int count = 0;
    *pressedKey = 0;

    dir = opendir(DEVICE_PATH);
    if(dir == NULL)
        return 0;

    strcpy(devname, DEVICE_PATH);
    filename = devname + strlen(devname);
    *filename++ = '/';
    while (count < max_fds && (de = readdir(dir))) {
        // Skip '.' and '..'
        if(de->d_name[0] == '.' &&
           (de->d_name[1] == '\0' ||
            (de->d_name[1] == '.' && de->d_name[2] == '\0')))
            continue;

        strcpy(filename, de->d_name);
        int fd = open(devname, O_RDWR | O_CLOEXEC);
        if(fd < 0)
            // Skip files that could not be opened
            continue;

        // Check if this device reports one of the relevant keys
        uint8_t keyBitmask[(KEY_MAX + 1) / 8];
        memset(keyBitmask, 0, sizeof(keyBitmask));
        ioctl(fd, EVIOCGBIT(EV_KEY, sizeof(keyBitmask)), keyBitmask);
        bool reportsKeys = false;
        for (size_t i = 0; i < sizeof(physical_keycodes) / sizeof(physical_keycodes[0]); i++) {
            if (test_bit(physical_keycodes[i], keyBitmask)) {
                reportsKeys = true;
                break;
            }
        }
        if (!reportsKeys) {
            // This device doesn't report any of the relevant keys
            close(fd);
            continue;
        }

        fds[count++] = fd;

        // Check if one of the keys is currently pressed on this device, to report it to the caller
        memset(keyBitmask, 0, sizeof(keyBitmask));
        ioctl(fd, EVIOCGKEY(sizeof(keyBitmask)), keyBitmask);
        for (size_t i = 0; i < sizeof(physical_keycodes) / sizeof(physical_keycodes[0]); i++) {
            if (test_bit(physical_keycodes[i], keyBitmask)) {
                if (*pressedKey == 0) {
                    *pressedKey = physical_keycodes[i];
                } else {
                    // More than one key is pressed, abort
                    *pressedKey = 0;
                    break;
                }
            }
        }
    }

    closedir(dir);
    return count;
}


int getRemainingTime(struct timespec expiration) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    if (now.tv_sec > expiration.tv_sec)
        return 0;
    else
        return (expiration.tv_sec - now.tv_sec) * 1000 + (expiration.tv_nsec - now.tv_nsec) / 1000000;
}



namespace xposed {

bool detectSafemodeTrigger(bool skipInitialDelay) {

    int efd = -1;
    int fds[MAX_DEVICES];
    int deviceCount = 0;
    int pressedKey = 0;
    int triggerPresses = 0;
    bool result = false;

    // Open input devices that report one of the relevant physical keys
    deviceCount = openKeyDevices(fds, sizeof(fds) / sizeof(fds[0]), &pressedKey);
    if (deviceCount == 0)
        // No input devices found, abort detection
        goto leave;

    if (pressedKey == 0 && skipInitialDelay)
        // None of the keys was held down and the initial delay is disabled
        // Immediately report a negative detection, with no further delays
        goto leave;

    // Prepare waiting mechanism for received events in all devices
    if ((efd = epoll_create(deviceCount)) < 0)
        // Failed to create the epoll handle, abort
        goto leave;

    // Register each device descriptor in the epoll handle
    for (int i = 0; i < deviceCount; i++) {
        struct epoll_event eventItem;
        memset(&eventItem, 0, sizeof(eventItem));
        eventItem.events = EPOLLIN;
        eventItem.data.fd = fds[i];
        if (epoll_ctl(efd, EPOLL_CTL_ADD, fds[i], &eventItem))
            // Failed to add device descriptor to the epoll handle, abort
            goto leave;
    }


    struct epoll_event mEvents[MAX_DEVICES];

    int timeout_ms;
    struct timespec expiration;
    clock_gettime(CLOCK_MONOTONIC, &expiration);
    expiration.tv_sec += INITIAL_DELAY;

    // Wait up to INITIAL_DELAY seconds for an initial keypress, it no key was initially down
    while (pressedKey == 0 && (timeout_ms = getRemainingTime(expiration)) > 0) {
        // Wait for next input event
        int pollResult = epoll_wait(efd, mEvents, sizeof(mEvents) / sizeof(mEvents[0]), timeout_ms);
        if (pollResult < 0)
            // Failed to wait for event, abort
            goto leave;
        for (int i = 0; i < pollResult; i++) {
            struct input_event evt;
            int32_t readSize = read(mEvents[i].data.fd, &evt, sizeof(evt));
            if (readSize != sizeof(evt))
                // Invalid size read, ignore
                continue;

            if (evt.type != EV_KEY)
                // Only consider key events
                continue;
            if (evt.value != 1)
                // Ignore key releases, we're monitoring presses
                continue;

            for (size_t j = 0; j < sizeof(physical_keycodes) / sizeof(physical_keycodes[0]); j++) {
                if (evt.code == physical_keycodes[j]) {
                    // One of the keys was pressed, end the initial detection
                    pressedKey = evt.code;
                    break;
                }
            }
        }
    }
    if (pressedKey == 0)
        // No key was pressed during the initial delay or upfront, so the detection has failed
        goto leave;

    // Notify the user that the safemode sequence has been started and we're waiting for
    // the remaining key presses
    vibrate(2, VIBRATION_SHORT, 200);


    // Detection will wait at most DETECTION_TIMEOUT seconds
    clock_gettime(CLOCK_MONOTONIC, &expiration);
    expiration.tv_sec += DETECTION_TIMEOUT;


    // Initial key press is counted as well
    triggerPresses++;

    // Loop waiting for the same key to be pressed the appropriate number of times, a different key to
    // be pressed, or the timeout to be reached
    while (pressedKey != 0 && triggerPresses < DETECTION_PRESSES && (timeout_ms = getRemainingTime(expiration)) > 0) {
        // Wait for next input event
        int pollResult = epoll_wait(efd, mEvents, sizeof(mEvents) / sizeof(mEvents[0]), timeout_ms);
        if (pollResult < 0)
            // Failed to wait for event, abort
            goto leave;
        for (int i = 0; i < pollResult; i++) {
            struct input_event evt;
            int32_t readSize = read(mEvents[i].data.fd, &evt, sizeof(evt));
            if (readSize != sizeof(evt))
                // Invalid size read, ignore
                continue;

            if (evt.type != EV_KEY)
                // Only consider key events
                continue;
            if (evt.value != 1)
                // Ignore key releases, we're monitoring presses
                continue;

            for (size_t j = 0; j < sizeof(physical_keycodes) / sizeof(physical_keycodes[0]); j++) {
                if (evt.code == physical_keycodes[j]) {
                    if (pressedKey == evt.code) {
                        // The same key was pressed again, increment the counter and notify the user
                        triggerPresses++;
                        if (triggerPresses < DETECTION_PRESSES)
                            vibrate(1, VIBRATION_SHORT, 0);
                    } else {
                        // A different key was pressed, abort
                        pressedKey = 0;
                    }
                    break;
                }
            }
        }
    }

    // Was safemode successfully triggered?
    if (pressedKey != 0 && triggerPresses == DETECTION_PRESSES) {
        vibrate(1, VIBRATION_LONG, 0);
        result = true;
    }

leave:
    if (efd >= 0)
        close(efd);
    for (int i = 0; i < deviceCount; i++)
        close(fds[i]);

    return result;

}

}

