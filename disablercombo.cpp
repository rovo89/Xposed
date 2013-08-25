/*
 * Detects input combinations for recovering from bootloops.
 *
 * Most of the logic was grabbed from frameworks/base/services/input/EventHub.cpp
 */
#include "disablercombo.h"

#include <dirent.h>
#include <fcntl.h>
#include <linux/input.h>

#define test_bit(bit, array)    (array[bit/8] & (1<<(bit%8)))
#define sizeof_bit_array(bits)  ((bits + 7) / 8)

#define MAX_FINGERS 10

namespace xposed {


static bool containsNonZeroByte(const uint8_t* array, uint32_t startIndex, uint32_t endIndex) {
    const uint8_t* end = array + endIndex;
    array += startIndex;
    while (array != end) {
        if (*(array++) != 0) {
            return true;
        }
    }
    return false;
}

static const char *DEVICE_PATH = "/dev/input";

static int openTouchDevice() {
    char devname[PATH_MAX];
    char *filename;
    DIR *dir;
    struct dirent *de;

    int result = -1;

    dir = opendir(DEVICE_PATH);
    if(dir == NULL)
        return result;

    strcpy(devname, DEVICE_PATH);
    filename = devname + strlen(devname);
    *filename++ = '/';
    while((de = readdir(dir))) {
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

        // Check if this is the device we want
        uint8_t keyBitmask[(KEY_MAX + 1) / 8];
        uint8_t absBitmask[(ABS_MAX + 1) / 8];
        memset(keyBitmask, 0, sizeof(keyBitmask));
        memset(absBitmask, 0, sizeof(absBitmask));

        ioctl(fd, EVIOCGBIT(EV_KEY, sizeof(keyBitmask)), keyBitmask);
        ioctl(fd, EVIOCGBIT(EV_ABS, sizeof(absBitmask)), absBitmask);

        bool hasGamepadButtons = containsNonZeroByte(keyBitmask, sizeof_bit_array(BTN_MISC), sizeof_bit_array(BTN_MOUSE))
                || containsNonZeroByte(keyBitmask, sizeof_bit_array(BTN_JOYSTICK), sizeof_bit_array(BTN_DIGI));
        if (test_bit(ABS_MT_POSITION_X, absBitmask)
                && test_bit(ABS_MT_POSITION_Y, absBitmask)
                && (test_bit(BTN_TOUCH, keyBitmask) || !hasGamepadButtons)) {
            // Found touch device
            result = fd;
            break;
        }

        // Close this device and move to the next
        close(fd);
    }

    closedir(dir);
    return result;
}


static int getTouchCount(int fd) {
    struct input_mt_request_layout {
      __u32 code;
      __s32 values[MAX_FINGERS];
    } mt_request;

    memset(&mt_request, 0, sizeof(mt_request));
    mt_request.code = ABS_MT_TRACKING_ID;
    if (ioctl(fd, EVIOCGMTSLOTS(sizeof(mt_request)), &mt_request)) {
        // Error, ignore
        return 0;
    } else {
        int touchCount = 0;
        for (int i = 0; i < MAX_FINGERS; i++) {
            if (mt_request.values[i] > 0)
                touchCount++;
        }
        return touchCount;
    }
}


////////////////////////////////////////////////////////////
// Main function to detect the disabler combo
////////////////////////////////////////////////////////////

bool detectDisableCombo() {

    int fd;
    fd = openTouchDevice();
    if (fd < 0) {
        return false;
    }

    int touchCount = getTouchCount(fd);
    close(fd);

    return (touchCount == 3) ? true : false;
}

}

