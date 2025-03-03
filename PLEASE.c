#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>
#include <string.h>
#include <sys/ioctl.h>
#include <linux/input.h>  // For event handling (input device)
#include <sys/types.h>
#include <sys/stat.h>

#define DEVICE_NAME "/dev/wacom-tablet"  // The character device path

// Function to test reading from the character device
void test_character_device() {
    int fd;
    char buffer[1024];
    ssize_t bytes_read;

    fd = open(DEVICE_NAME, O_RDONLY);
    if (fd == -1) {
        perror("Failed to open character device");
        return;
    }

    printf("Reading from character device...\n");
    bytes_read = read(fd, buffer, sizeof(buffer));
    if (bytes_read == -1) {
        perror("Failed to read data from device");
    } else {
        buffer[bytes_read] = '\0';  // Null-terminate the read data
        printf("Read %zd bytes from character device: %s\n", bytes_read, buffer);
    }

    close(fd);
}

// Function to test writing to the character device
void test_character_device_write() {
    int fd;
    const char *data = "Test data to write to device";

    fd = open(DEVICE_NAME, O_WRONLY);
    if (fd == -1) {
        perror("Failed to open character device");
        return;
    }

    printf("Writing to character device...\n");
    ssize_t bytes_written = write(fd, data, strlen(data));
    if (bytes_written == -1) {
        perror("Failed to write data to device");
    } else {
        printf("Written %zd bytes to character device\n", bytes_written);
    }

    close(fd);
}

// Function to test listening to input events from the tablet
void test_input_device() {
    int fd;
    struct input_event ev;

    fd = open("/dev/input/event0", O_RDONLY); // You may need to adjust this depending on your system's event device
    if (fd == -1) {
        perror("Failed to open input device");
        return;
    }

    printf("Listening to input events from the tablet...\n");
    while (1) {
        ssize_t bytes_read = read(fd, &ev, sizeof(struct input_event));
        if (bytes_read == -1) {
            perror("Failed to read input event");
            break;
        }

        if (bytes_read == sizeof(struct input_event)) {
            // Check for specific events, for example, ABS_X, ABS_Y (coordinates), or ABS_PRESSURE
            if (ev.type == EV_ABS) {
                switch (ev.code) {
                    case ABS_X:
                        printf("ABS_X: %d\n", ev.value);
                        break;
                    case ABS_Y:
                        printf("ABS_Y: %d\n", ev.value);
                        break;
                    case ABS_PRESSURE:
                        printf("ABS_PRESSURE: %d\n", ev.value);
                        break;
                    default:
                        break;
                }
            }
        }
    }

    close(fd);
}

int main() {
    // Test the character device read/write functionality
    test_character_device_write(); // Write test
    test_character_device(); // Read test

    // Test the input device for tablet touch events
    test_input_device();

    return 0;
}

