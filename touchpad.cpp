#include <cstdlib>
#include <linux/input-event-codes.h>
#include <linux/input.h>
#include <linux/uinput.h>
#include <sys/poll.h>
#include <fcntl.h>
#include <unistd.h>
#include <dirent.h>
#include <cstring>
#include <cstdio>
#include <iostream>
#include <vector>
using std::string;

const char *deviceName = "x-virtual-touchpad";
std::vector<int> touch_fds;

// 鼠标速度系数，默认 0.3（慢），可通过命令行修改
float SPEED_FACTOR = 0.2f;

std::vector<string> ListInputDevices() {
    const string input_directory = "/dev/input";
    std::vector<string> filenames;
    DIR* directory = opendir(input_directory.c_str());
    struct dirent *entry = NULL;
    while ((entry = readdir(directory))) {
        if (strcmp(entry->d_name, ".") && strcmp(entry->d_name, "..")) {
          filenames.push_back(input_directory + "/" + entry->d_name);
        }
    }
  closedir(directory);
  return filenames;
}

bool HasSpecificAbs(int device_fd, unsigned int abs) {
  size_t nchar = KEY_MAX/8 + 1;
  unsigned char bits[nchar];
  ioctl(device_fd, EVIOCGBIT(EV_ABS, sizeof(bits)), &bits);
  return bits[abs/8] & (1 << (abs % 8));
}

void SetAbsInfoFrom(int device_fd, int uinput_fd) {
	for(int abs_i = ABS_X; abs_i <= ABS_MAX; abs_i++) {
		if(HasSpecificAbs(device_fd, abs_i)) {
			struct input_absinfo absinfo;
			if (ioctl(device_fd, EVIOCGABS(abs_i), &absinfo) == 0) {
				struct uinput_abs_setup uinputAbsInfo {};
				memset(&uinputAbsInfo, 0, sizeof(uinputAbsInfo));
				uinputAbsInfo.code = abs_i;
				uinputAbsInfo.absinfo = absinfo;
				ioctl(uinput_fd, UI_ABS_SETUP, &uinputAbsInfo);
			}
		}
	}
}

bool HasSpecificKey(int device_fd, unsigned int key) {
  size_t nchar = KEY_MAX/8 + 1;
  unsigned char bits[nchar];
  ioctl(device_fd, EVIOCGBIT(EV_KEY, sizeof(bits)), &bits);
  return bits[key/8] & (1 << (key % 8));
}

void SetKeyBits(int device_fd, int uinput_fd) {
	for(int key_i = BTN_MOUSE; key_i <= KEY_MAX; key_i++) {
		if (HasSpecificKey(device_fd, key_i)) {
			ioctl(uinput_fd, UI_SET_KEYBIT, key_i);
		}
	}
}

bool HasEventType(int device_fd, unsigned int type) {
  unsigned long evbit = 0;
  ioctl(device_fd, EVIOCGBIT(0, sizeof(evbit)), &evbit);
  return evbit & (1 << type);
}

void SetEventTypeBits(int device_fd, int uinput_fd) {
	for(int ev_i = EV_SYN; ev_i <= EV_MAX; ev_i++) {
		if (HasEventType(device_fd, ev_i)) {
			ioctl(uinput_fd, UI_SET_EVBIT, ev_i);
		}
	}
}

void PrintDeviceName(int device_fd) {
	char dev_name[24];
	if(ioctl(device_fd, EVIOCGNAME(sizeof(dev_name) - 1), &dev_name)) {
			printf(" %s\n", dev_name);
	}
}

int SetupUinputDevice(int device_fd) {
	struct uinput_setup uinputSetup;
	int uinput_fd = open("/dev/uinput", O_WRONLY | O_NONBLOCK);
	if (uinput_fd <= 0) exit(EXIT_FAILURE);
	SetEventTypeBits(device_fd, uinput_fd);
	SetKeyBits(device_fd, uinput_fd);
	SetAbsInfoFrom(device_fd, uinput_fd);
	ioctl(uinput_fd, UI_SET_PROPBIT, INPUT_PROP_POINTER);
	memset(&uinputSetup, 0, sizeof(uinputSetup));
	strncpy(uinputSetup.name, deviceName, strlen(deviceName));
	uinputSetup.id.version = 1;
	uinputSetup.id.bustype = BUS_VIRTUAL;
	ioctl(uinput_fd, UI_DEV_SETUP, &uinputSetup);
	if(ioctl(uinput_fd, UI_DEV_CREATE)) {
		close(uinput_fd);
		exit(EXIT_FAILURE);
	}
	return uinput_fd;
}

bool HasInputProp(int device_fd, unsigned int input_prop) {
	size_t nchar = INPUT_PROP_MAX/8 + 1;
	unsigned char bits[nchar];
	ioctl(device_fd, EVIOCGPROP(sizeof(bits)), &bits);
	return bits[input_prop/8] & (1 << (input_prop % 8));
}

bool IsTouchDevice(int device_fd) {
	for (size_t i = 0; i < touch_fds.size(); i++) {
		if (device_fd == touch_fds[i]) return true;
	}
	return false;
}

int getTouchDeviceIndex(int device_fd) {
        for (size_t i = 0; i < touch_fds.size(); i++) {
                if (device_fd == touch_fds[i]) return i;
        }
        return -1;
}

int main(int argc, char *argv[])
{
	// ============== 新增：命令行参数调节速度 ==============
	if (argc >= 2) {
	    float input_speed = atof(argv[1]);
	    if (input_speed > 0.0f && input_speed <= 5.0f) {
	        SPEED_FACTOR = input_speed;
	        printf("已设置鼠标速度: %.2f\n", SPEED_FACTOR);
	    } else {
	        printf("使用默认鼠标速度: %.2f\n", SPEED_FACTOR);
	    }
	} else {
	    printf("使用默认鼠标速度: %.2f\n", SPEED_FACTOR);
	}
	// =====================================================

	setlinebuf(stdout);
	std::vector<string> evdevNames = ListInputDevices();
	std::vector<pollfd> poll_fds;
	std::vector<int> uinput_fds;
	
	for (size_t i = 0; i < evdevNames.size(); i++) {
		int device_fd = open(evdevNames[i].c_str(), O_RDONLY);
		if (device_fd < 0) {
			return 1;
		}
		if(HasSpecificAbs(device_fd, ABS_X) || HasSpecificAbs(device_fd, ABS_Y) || 
			  HasSpecificAbs(device_fd, ABS_MT_POSITION_X) || HasSpecificAbs(device_fd, ABS_MT_POSITION_Y) ) {
				if(HasInputProp(device_fd, INPUT_PROP_DIRECT)) {
					printf("add touch device: %s", evdevNames[i].c_str());
					PrintDeviceName(device_fd);
					touch_fds.push_back(device_fd);
					poll_fds.push_back(pollfd{device_fd, POLLIN, 0});
					uinput_fds.push_back(SetupUinputDevice(device_fd));
				} else {
					close(device_fd);
				}
			
		} else if (HasSpecificKey(device_fd, KEY_VOLUMEDOWN) || HasSpecificKey(device_fd, KEY_VOLUMEUP)) {
			poll_fds.push_back(pollfd{device_fd, POLLIN, 0});
			printf("add button device: %s", evdevNames[i].c_str());
			PrintDeviceName(device_fd);
		} else {
				close(device_fd);
		}
	}
	if (poll_fds.empty()) return 1;
	bool active = true;
	bool ungrab = true;
	bool toggle_by_voldown = true;
	bool toggle_by_volup = false;
	struct input_event ie {};
	while(true) {
		if (ungrab) {
			for (size_t i = 0; i < touch_fds.size(); i++)
				ioctl(touch_fds[i], EVIOCGRAB, (void *)active);
			ungrab = false;
		}
		poll(poll_fds.data(), poll_fds.size(), -1);
		for (size_t i = 0; i < poll_fds.size(); i++)
			if (poll_fds[i].revents & POLLIN)
				if (read(poll_fds[i].fd, &ie, sizeof(ie)) == sizeof(struct input_event)){
					if (IsTouchDevice(poll_fds[i].fd)) {
						int i1 = getTouchDeviceIndex(poll_fds[i].fd);
						if ( i1 >= 0 && active ) {
							// 降低鼠标速度
							if (ie.type == EV_ABS) {
								if (ie.code == ABS_X || ie.code == ABS_MT_POSITION_X ||
									ie.code == ABS_Y || ie.code == ABS_MT_POSITION_Y) {
									ie.value = static_cast<int>(ie.value * SPEED_FACTOR);
								}
							}
							write(uinput_fds[i1], &ie, sizeof(struct input_event));
						}
					} else if (toggle_by_voldown || toggle_by_volup) {
						int toggle_key = (toggle_by_voldown) ? KEY_VOLUMEDOWN : KEY_VOLUMEUP;
						if (ie.code == toggle_key && ie.value == 0) {
							ungrab = true; active = !active;
							printf("toggle\n");
						}
					}
				}
	}
}
