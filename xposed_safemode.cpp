/*
 * Detects input combinations for recovering from bootloops.
 *
 * The safemode trigger is detected if upon entry one of the VOLUME keys is being
 * held down, the POWER or HOME key is pressed and released twice and the
 * VOLUME key is then released.
 * All this must happen within 10s of the start of the detection. No delay exists
 * if no VOLUME keys are held down, or as soon as they are released.
 *
 * Feedback:
 * - 2 short vibrations when entering the detection, if a VOLUME key is held down
 * - 2 short vibrations after the safemode sequence is successfully detected
 * - 1 short vibration if the detection timed out or an invalid sequence was used
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

#define MAX_DEVICES 4
#define DETECTION_TIMEOUT 10
#define VIBRATOR_CONTROL "/sys/class/timed_output/vibrator/enable"

#define test_bit(bit, array)    (array[bit/8] & (1<<(bit%8)))


static void vibrate(int pulses) {
    int fd;

    if ((fd = open(VIBRATOR_CONTROL, O_RDWR)) < 0)
        // Failed to open the control file, ignore it
        return;

    for (int i = 0; i < pulses; i++) {
        if (i != 0)
            // Pause 300ms after each vibration starts
            usleep(300 * 1000);
        // Vibrate (asynchronously) for 100ms
        write(fd, "100", 3);
    }
    close(fd);
}


static const char *DEVICE_PATH = "/dev/input";

static int openKeyDevices(int *fds, int max_fds, bool *volKeyPressed) {
    char devname[PATH_MAX];
    char *filename;
    DIR *dir;
    struct dirent *de;

    int count = 0;
    *volKeyPressed = false;

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
        bool reportsKeys = test_bit(KEY_VOLUMEDOWN, keyBitmask) || test_bit(KEY_VOLUMEUP, keyBitmask)
                || test_bit(KEY_POWER, keyBitmask) || test_bit(KEY_HOME, keyBitmask);
        if (!reportsKeys) {
            // This device doesn't report any of the relevant keys
            close(fd);
            continue;
        }

        fds[count++] = fd;

        // Check if one of the VOLUME keys is currently pressed on this device
        memset(keyBitmask, 0, sizeof(keyBitmask));
        ioctl(fd, EVIOCGKEY(sizeof(keyBitmask)), keyBitmask);
        if (test_bit(KEY_VOLUMEDOWN, keyBitmask) || test_bit(KEY_VOLUMEUP, keyBitmask))
            *volKeyPressed = true;
    }

    closedir(dir);
    return count;
}


namespace xposed {

bool detectSafemodeTrigger() {

    // Descriptors for input devices
    int fds[MAX_DEVICES];
    for (int i = 0; i < MAX_DEVICES; i++)
        fds[i] = -1;

    // Open up to MAX_DEVICES input devices that report one of the relevant physical keys
    bool volPressed = false;
    int deviceCount = openKeyDevices(fds, MAX_DEVICES, &volPressed);
    if (deviceCount == 0) {
        // No input devices not found, abort detection
        return false;
    }
    if (!volPressed) {
        // None of the VOLUME keys are initially pressed, combo not triggered
        for (int i = 0; i < deviceCount; i++)
            close(fds[i]);
        return false;
    }

    // Prepare waiting mechanism for received events in all devices
    int efd;
    if ((efd = epoll_create(deviceCount)) < 0) {
        // Failed to create the epoll handle, abort
        for (int i = 0; i < deviceCount; i++)
            close(fds[i]);
        return false;
    }
    // Register each device descriptor in the epoll handle
    for (int i = 0; i < deviceCount; i++) {
        struct epoll_event eventItem;
        memset(&eventItem, 0, sizeof(eventItem));
        eventItem.events = EPOLLIN;
        eventItem.data.fd = fds[i];
        if (epoll_ctl(efd, EPOLL_CTL_ADD, fds[i], &eventItem)) {
            // Failed to add device descriptor to the epoll handle, abort
            for (i = 0; i < deviceCount; i++)
                close(fds[i]);
            close(efd);
            return false;
        }
    }

    // Notify the user that we're waiting for the trigger presses
    vibrate(2);

    // Detection will wait at most DETECTION_TIMEOUT seconds
    struct timespec expiration;
    clock_gettime(CLOCK_MONOTONIC, &expiration);
    expiration.tv_sec += DETECTION_TIMEOUT;


    struct epoll_event mPendingEventItems[MAX_DEVICES];
    bool releasedVol = false;
    int triggerPresses = 0;

    // Loop waiting for the VOLUME key to be released or the timeout to be reached
    while (!releasedVol) {
        // Calculate remainint time to wait until expiration
        int timeout_ms;
        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        if (now.tv_sec > expiration.tv_sec)
            timeout_ms = 0;
        else
            timeout_ms = (expiration.tv_sec - now.tv_sec) * 1000 + (expiration.tv_nsec - now.tv_nsec) / 1000000;

        if (timeout_ms <= 0)
            // Maximum time elapsed, abort
            break;

        // Wait for next input event
        int pollResult = epoll_wait(efd, mPendingEventItems, sizeof(mPendingEventItems), timeout_ms);
        if (pollResult < 0) {
            // Failed to wait for event, abort
            for (int i = 0; i < deviceCount; i++)
                close(fds[i]);
            close(efd);
            return false;
        }
        for (int i = 0; i < pollResult; i++) {
            struct input_event evt;
            int32_t readSize = read(mPendingEventItems[i].data.fd, &evt, sizeof(evt));
            if (readSize != sizeof(evt))
                // Invalid size read, ignore
                continue;

            if (evt.type != EV_KEY)
                // Only consider key events
                continue;
            if (evt.value != 0)
                // Ignore key presses, we're monitoring releases
                continue;

            if (evt.code == KEY_POWER || evt.code == KEY_HOME) {
                // increment trigger presses
                triggerPresses++;
            } else if (evt.code == KEY_VOLUMEDOWN || evt.code == KEY_VOLUMEUP) {
                // finish key combo detection, successfully (non-timeout)
                releasedVol = true;
            }
        }
    }
    // Close input handles
    for (int i = 0; i < deviceCount; i++)
        close(fds[i]);
    close(efd);



    if (releasedVol && triggerPresses == 2) {
        // Trigger safemode
        vibrate(2);
        return true;

    } else {
        // Timed out or invalid trigger count
        vibrate(1);
        return false;
    }

}

}

