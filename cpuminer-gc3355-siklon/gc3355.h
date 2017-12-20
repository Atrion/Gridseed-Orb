/*
 * Driver for GC3355 chip to mine Litecoin, power by GridChip & GridSeed
 *
 * Copyright 2013 faster <develop@gridseed.com>
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 2 of the License, or (at your option)
 * any later version.  See COPYING for more details.
 */
 
#ifndef WIN32
#include <termios.h>
#include <time.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <errno.h>
#include <libudev.h>
#include "elist.h"
#else
#include <setupapi.h>
#include <initguid.h>
#if !defined(GUID_DEVINTERFACE_USB_DEVICE)
const GUID GUID_DEVINTERFACE_USB_DEVICE = {0xA5DCBF10, 0x6530, 0x11D2, {0x90, 0x1F, 0x00, 0xC0, 0x4F, 0xB9, 0x51, 0xED}};
#endif
#define htobe16 htons
#define htole16(x) (x)
#define be16toh ntohs
#define le16toh(x) (x)
#define htobe32 htonl
#define htole32(x) (x)
#define be32toh ntohl
#define le32toh(x) (x)
#define htobe64 htonll
#define htole64(x) (x)
#define be64toh ntohll
#define le64toh(x) (x)
char* strtok_r(char *str, const char *delim, char **nextp);
char* strtok_r(char *str, const char *delim, char **nextp)
{
    char *ret;

    if (str == NULL)
    {
        str = *nextp;
    }

    str += strspn(str, delim);

    if (*str == '\0')
    {
        return NULL;
    }

    ret = str;

    str += strcspn(str, delim);

    if (*str)
    {
        *str++ = '\0';
    }

    *nextp = str;

    return ret;
}
#include <windows.h>
#include <winsock2.h>
#include <io.h>
typedef unsigned int speed_t;
#define  B115200  115200
#endif
#include <ctype.h>
#include <gc3355-commands.h>
#include <string.h>

#define GC3355_OVERCLOCK_MAX_HWE 2
#define GC3355_OVERCLOCK_ADJUST_MIN 10
#define GC3355_OVERCLOCK_ADJUST_STEPS 3845
#define GC3355_OVERCLOCK_FREQ_STEP 25
#define GC3355_MIN_FREQ 600
#define GC3355_MAX_FREQ 1400
#define GC3355_HASH_SPEED 84.705882
#define GC3355_TRESHOLD 0.98
#define GC3355_MAX_CHIPS 8
#define GC3355_USB_STR "GC3355 5-chip USB-Mini Miner"
#define GC3355_BLADE_STR "GC3355 40-chip G-Blade Miner"
#define GC3355_NONE_STR "Unknown GC3355 Miner"

struct chip_frequency_list
{
	struct list_head list;
	uint8_t chip_id;
	uint16_t freq;
};

struct device_frequency_list
{
	struct list_head list;
	char *devname;
 	struct chip_frequency_list chip_list;
};

struct gc3355_devices
{
	struct list_head list;
	char *path;
	char *serial;
};

static uint16_t can_start = 0;
static struct device_frequency_list *frequency_list;

static struct device_frequency_list* init_device_frequency_list()
{
	struct device_frequency_list *list = calloc(1, sizeof(struct device_frequency_list));
	INIT_LIST_HEAD(&list->list);
	INIT_LIST_HEAD(&list->chip_list.list);
	return list;
}

static void add_frequency(struct device_frequency_list *list, char *devname, uint16_t chip_id, uint16_t freq)
{
	struct device_frequency_list *device, *new_device;
	struct chip_frequency_list *chip, *new_chip;
	list_for_each_entry(device, &list->list, list)
	{
		if(!strcmp(device->devname, devname))
		{
			list_for_each_entry(chip, &device->chip_list.list, list)
			{
				if(chip->chip_id == chip_id)
				{
					chip->freq = freq;
					goto out;
				}
			}
new_chip:
			new_chip = calloc(1, sizeof(struct chip_frequency_list));
			new_chip->chip_id = chip_id;
			new_chip->freq = freq;
			list_add(&new_chip->list, &device->chip_list.list);
			goto out;
		}
	}
	new_device = calloc(1, sizeof(struct device_frequency_list));
	new_device->devname = strdup(devname);
	INIT_LIST_HEAD(&new_device->chip_list.list);
	list_add(&new_device->list, &list->list);
	device = new_device;
	goto new_chip;
out:
	return;
}

static void free_device_frequency_list(struct device_frequency_list *list)
{
	struct device_frequency_list *device, *tmp_device;
	struct chip_frequency_list *chip, *tmp_chip;
	list_for_each_entry_safe(device, tmp_device, &list->list, list)
	{
		list_for_each_entry_safe(chip, tmp_chip, &device->chip_list.list, list)
		{
			list_del(&chip->list);
			free(chip);
		}
		list_del(&device->list);
		free(device->devname);
		free(device);
	}	
}

#ifdef HAVE_LIBUDEV
static struct gc3355_devices *gc3355_get_device_list()
{
	struct gc3355_devices *device_list, *device;
	struct udev *udev;
	struct udev_enumerate *enumerate;
	struct udev_list_entry *devices, *dev_list_entry;
	struct udev_device *dev, *usb_dev;
	device_list = calloc(1, sizeof(struct gc3355_devices));
	INIT_LIST_HEAD(&device_list->list);
	udev = udev_new();
	if(!udev) return device_list;
	enumerate = udev_enumerate_new(udev);
	udev_enumerate_add_match_subsystem(enumerate, "tty");
	udev_enumerate_scan_devices(enumerate);
	devices = udev_enumerate_get_list_entry(enumerate);
	udev_list_entry_foreach(dev_list_entry, devices)
	{
		const char *path;
		const char *devnode_path;
		const char *serial;
		const char *id_vendor;
		const char *id_product;
		path = udev_list_entry_get_name(dev_list_entry);
		dev = udev_device_new_from_syspath(udev, path);
		usb_dev = udev_device_get_parent_with_subsystem_devtype(dev, "usb", "usb_device");
		if (!usb_dev) continue;
		id_vendor = udev_device_get_sysattr_value(usb_dev,"idVendor");
		id_product = udev_device_get_sysattr_value(usb_dev, "idProduct");
		if((strcmp(id_vendor, "10c4") || strcmp(id_product, "ea60")) && (strcmp(id_vendor, "0483") || strcmp(id_product, "5740"))) continue;
		devnode_path = udev_device_get_devnode(dev);
		serial = udev_device_get_sysattr_value(usb_dev, "serial");
		device = calloc(1, sizeof(struct gc3355_devices));
		device->path = strdup(devnode_path);
		device->serial = strdup(serial);
		list_add(&device->list, &device_list->list);
		udev_device_unref(dev); 
	}
	udev_enumerate_unref(enumerate);
	udev_unref(udev);
	return device_list;
}
#else
static struct gc3355_devices *gc3355_get_device_list()
{
	struct gc3355_devices *device_list, *device;
	HDEVINFO                         hDevInfo;
	SP_DEVICE_INTERFACE_DATA         DevIntfData;
	PSP_DEVICE_INTERFACE_DETAIL_DATA DevIntfDetailData;
	SP_DEVINFO_DATA                  DevData;
	DWORD dwSize, dwType, dwMemberIdx;
	HKEY hKey;
	BYTE lpData[1024];
	device_list = calloc(1, sizeof(struct gc3355_devices));
	INIT_LIST_HEAD(&device_list->list);
	hDevInfo = SetupDiGetClassDevs(&GUID_DEVINTERFACE_USB_DEVICE, NULL, 0, DIGCF_DEVICEINTERFACE | DIGCF_PRESENT);
	if (hDevInfo != INVALID_HANDLE_VALUE)
	{
		DevIntfData.cbSize = sizeof(SP_DEVICE_INTERFACE_DATA);
		dwMemberIdx = 0;
		SetupDiEnumDeviceInterfaces(hDevInfo, NULL, &GUID_DEVINTERFACE_USB_DEVICE, dwMemberIdx, &DevIntfData);

		while(GetLastError() != ERROR_NO_MORE_ITEMS)
		{
			DevData.cbSize = sizeof(DevData);
			SetupDiGetDeviceInterfaceDetail(hDevInfo, &DevIntfData, NULL, 0, &dwSize, NULL);
			DevIntfDetailData = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, dwSize);
			DevIntfDetailData->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA);
			if (SetupDiGetDeviceInterfaceDetail(hDevInfo, &DevIntfData, DevIntfDetailData, dwSize, &dwSize, &DevData))
			{
				if (strstr(DevIntfDetailData->DevicePath, "vid_0483&pid_5740") != NULL || strstr(DevIntfDetailData->DevicePath, "vid_10c4&pid_ea60") != NULL)
				{
					hKey = SetupDiOpenDevRegKey(hDevInfo, &DevData, DICS_FLAG_GLOBAL, 0, DIREG_DEV, KEY_READ);
					dwType = REG_SZ;
					dwSize = sizeof(lpData);
					RegQueryValueEx(hKey, "PortName", NULL, &dwType, lpData, &dwSize);
					RegCloseKey(hKey);
					char *serial, *devname, *tmp;
					serial = strchr(DevIntfDetailData->DevicePath, '#');
					if(serial == NULL)
						continue;
					serial = strchr(serial + 1, '#') + 1;
					tmp = strchr(serial, '#');
					*tmp = '\0';
					tmp = serial;
					while (*tmp != '\0')
					{
						*tmp = toupper(*tmp);
						tmp++;
					}
					devname = malloc(strlen(lpData) + 5);
					strcpy(devname, "\\\\.\\");
					strcpy(devname + 4, lpData);
					device = calloc(1, sizeof(struct gc3355_devices));
					device->path = devname;
					device->serial = strdup(serial);
					list_add(&device->list, &device_list->list);
				}
			}
			HeapFree(GetProcessHeap(), 0, DevIntfDetailData);
			SetupDiEnumDeviceInterfaces(hDevInfo, NULL, &GUID_DEVINTERFACE_USB_DEVICE, ++dwMemberIdx, &DevIntfData);
		}
		SetupDiDestroyDeviceInfoList(hDevInfo);
	}
	return device_list;
}
#endif

static int gc3355_get_device_count(struct gc3355_devices *device_list)
{
	struct gc3355_devices *device;
	int count = 0;
	list_for_each_entry(device, &device_list->list, list)
		count++;
	return count;
}

static struct gc3355_devices *gc3355_get_device(struct gc3355_devices *device_list, char *path)
{
	struct gc3355_devices *device, *tmp, *ret = NULL;
	list_for_each_entry_safe(device, tmp, &device_list->list, list)
	{
		if(!strcmp(device->path, path))
		{
			list_del(&device->list);
			ret = device;
			break;
		}
	}
	return ret;
}

static struct gc3355_devices *gc3355_get_next_device(struct gc3355_devices *device_list)
{
	struct gc3355_devices *device, *tmp, *ret = NULL;
	list_for_each_entry_safe(device, tmp, &device_list->list, list)
	{
		list_del(&device->list);
		ret = device;
		break;
	}
	return ret;
}

static void gc3355_free_device(struct gc3355_devices *device)
{
	free(device->path);
	free(device->serial);
	free(device);
}

static void gc3355_free_device_list(struct gc3355_devices *device_list)
{
	struct gc3355_devices *device, *tmp;
	list_for_each_entry_safe(device, tmp, &device_list->list, list)
	{
		list_del(&device->list);
		gc3355_free_device(device);
	}
}

/* external functions */
extern void scrypt_1024_1_1_256(const uint32_t *input, uint32_t *output,
    uint32_t *midstate, unsigned char *scratchpad);

/* local functions */
static int gc3355_scanhash(struct gc3355_dev *gc3355, struct work *work, unsigned char *scratchbuf, uint32_t *midstate);

/* close UART device */
static void gc3355_close(int fd)
{
	if (fd > 0)
		close(fd);
	return;
}

static void gc3355_exit(struct gc3355_dev *gc3355)
{
	applog(LOG_INFO, "%d: Terminating GC3355 chip mining thread", gc3355->id);
	gc3355_close(gc3355->dev_fd);
	pthread_exit(NULL);
}

/* open UART device */
static int gc3355_open(struct gc3355_dev *gc3355, speed_t baud)
{
#ifdef WIN32
	DWORD	timeout = 100;

	applog(LOG_INFO, "%d: Open device %s", gc3355->id, gc3355->devname);
	if (gc3355->dev_fd > 0)
		gc3355_close(gc3355->dev_fd);

	HANDLE hSerial = CreateFile(gc3355->devname, GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING, 0, NULL);
	if (unlikely(hSerial == INVALID_HANDLE_VALUE))
	{
		DWORD e = GetLastError();
		switch (e) {
		case ERROR_ACCESS_DENIED:
			applog(LOG_ERR, "%d: Do not have user privileges required to open %s", gc3355->id, gc3355->devname);
			break;
		case ERROR_SHARING_VIOLATION:
			applog(LOG_ERR, "%d: %s is already in use by another process", gc3355->id, gc3355->devname);
			break;
		default:
			applog(LOG_DEBUG, "%d: Open %s failed, GetLastError:%u", gc3355->id, gc3355->devname, e);
			break;
		}
		return -1;
	}

	// thanks to af_newbie for pointers about this
	COMMCONFIG comCfg = {0};
	comCfg.dwSize = sizeof(COMMCONFIG);
	comCfg.wVersion = 1;
	comCfg.dcb.DCBlength = sizeof(DCB);
	comCfg.dcb.BaudRate = baud;
	comCfg.dcb.fBinary = 1;
	comCfg.dcb.fDtrControl = 0;
	comCfg.dcb.fRtsControl = 0;
	comCfg.dcb.fTXContinueOnXoff = 0;
	comCfg.dcb.fDsrSensitivity = 0;
	comCfg.dcb.ByteSize = 8;
	comCfg.dcb.fParity = 0;
	comCfg.dcb.fOutxCtsFlow = 0;
	comCfg.dcb.fOutxDsrFlow = 0;
	comCfg.dcb.fOutX = 0;
	comCfg.dcb.fInX = 0;
	comCfg.dcb.fAbortOnError = 0;
	SetCommConfig(hSerial, &comCfg, sizeof(comCfg));

	// Code must specify a valid timeout value (0 means don't timeout)
	const DWORD ctoms = timeout;
	COMMTIMEOUTS cto = {ctoms, 0, ctoms, 0, ctoms};
	SetCommTimeouts(hSerial, &cto);

	PurgeComm(hSerial, PURGE_RXABORT);
	PurgeComm(hSerial, PURGE_TXABORT);
	PurgeComm(hSerial, PURGE_RXCLEAR);
	PurgeComm(hSerial, PURGE_TXCLEAR);

	gc3355->dev_fd = _open_osfhandle((intptr_t)hSerial, 0);
	if (gc3355->dev_fd < 0)
		return -1;
	return 0;
#else
	struct termios	my_termios;
	int fd;

	applog(LOG_INFO, "%d: Open device %s", gc3355->id, gc3355->devname);
	if (gc3355->dev_fd > 0)
		gc3355_close(gc3355->dev_fd);

    fd = open(gc3355->devname, O_RDWR | O_CLOEXEC | O_NOCTTY | O_SYNC);
	if (fd < 0) {
		if (errno == EACCES)
			applog(LOG_ERR, "%d: Do not have user privileges to open %s", gc3355->id, gc3355->devname);
		else
			applog(LOG_ERR, "%d: failed open device %s", gc3355->id, gc3355->devname);
		return 1;
	}

	tcgetattr(fd, &my_termios);
	cfsetispeed(&my_termios, baud);
	cfsetospeed(&my_termios, baud);
	cfsetspeed(&my_termios,  baud);
	
	my_termios.c_cflag &= ~(CSIZE | PARENB | CSTOPB);
	my_termios.c_cflag |= CS8;
	my_termios.c_cflag |= CREAD;
	my_termios.c_cflag |= CLOCAL;

	my_termios.c_iflag &= ~(IGNBRK | BRKINT | PARMRK |
			ISTRIP | INLCR | IGNCR | ICRNL | IXON);
	my_termios.c_oflag &= ~OPOST;
	my_termios.c_lflag &= ~(ECHO | ECHOE | ECHONL | ICANON | ISIG | IEXTEN);

	// Code must specify a valid timeout value (0 means don't timeout)
	my_termios.c_cc[VTIME] = (cc_t)1;
	my_termios.c_cc[VMIN] = 0;
	
	tcsetattr(fd, TCSANOW, &my_termios);
	tcflush(fd, TCIOFLUSH);
	gc3355->dev_fd = fd;

	return 0;
#endif
}

/* send data to UART */
static int gc3355_write(struct gc3355_dev *gc3355, const void *buf, size_t buflen)
{
	size_t ret = write(gc3355->dev_fd, buf, buflen);
	usleep(10000);
	if (ret != buflen)
	{
		applog(LOG_INFO, "%d: UART write error", gc3355->id);
		gc3355_exit(gc3355);
	}
	return 0;
}

static int gc3355_gets(struct gc3355_dev *gc3355, unsigned char *buf, int read_amount)
{
	int fd;
	ssize_t nread = 0;
	
	fd = gc3355->dev_fd;
	memset(buf, 0, read_amount);
	nread = read(fd, buf, read_amount);
	if(nread == -1)
	{
		applog(LOG_ERR, "%d: Read error: %s", gc3355->id, strerror(errno));
		return 1;
	}
	if(nread == 0)
	{
		return -1;
	}
	if (nread != read_amount)
	{
		applog(LOG_ERR, "%d: Read error: Read %d bytes, but expected %d bytes", gc3355->id, nread, read_amount);
		return 1;
	}
	return 0;
}

static void gc3355_send_cmds(struct gc3355_dev *gc3355, const unsigned char *cmds[])
{
	int i;
	for(i = 0; cmds[i] != NULL; i++)
	{
		gc3355_write(gc3355, cmds[i] + 1, cmds[i][0]);
	}
}

static void gc3355_send_chip_cmds(struct gc3355_dev *gc3355, const unsigned char *cmds[], unsigned char chip_id)
{
	int i;
	if(chip_id = 0xf)
	{
		gc3355_send_cmds(gc3355, cmds);
		return;
	}
	for(i = 0; cmds[i] != NULL; i++)
	{
		int size = cmds[i][0];
		unsigned char chip_cmd[size];
		memcpy(chip_cmd, cmds[i] + 1, size);
		if(chip_cmd[2] == 0x1f)
			chip_cmd[2] = 0x10 | chip_id;
		gc3355_write(gc3355, chip_cmd, size);
	}
}

static uint32_t gc3355_get_firmware_version(struct gc3355_dev *gc3355)
{
	unsigned char buf[12];
	int read;
	
	gc3355_send_cmds(gc3355, firmware_request_cmd);
	read = gc3355_gets(gc3355, buf, 12);
	if (read)
	{
		applog(LOG_ERR, "%d: Failed reading firmware version", gc3355->id);
		return -1;
	}
	// firmware response begins with 55aac000 90909090
	if (memcmp(buf, "\x55\xaa\xc0\x00\x90\x90\x90\x90", 8) != 0)
	{
		applog(LOG_ERR, "%d: Invalid response while reading firmware version", gc3355->id);
		return -1;
	}
	uint32_t fw_version = htobe32(*(uint32_t *)(buf + 8));
	return fw_version;
}

static void gc3355_set_core_freq(struct gc3355_dev *gc3355, const unsigned short chip_id, unsigned short freq)
{
	// See https://github.com/gridseed/gc3355-doc/blob/master/GC3355_Register_Spec.pdf
	int i;
	const float mhz = freq;
	uint8_t freq_div, freq_mult, last_freq_mult;  // mhz = (25 / freq_div * freq_mult)
	float actual_mhz, last_actual_mhz = -1;
	const uint8_t pll_bypass = 1;
	const uint8_t pll_bandselect = 0;
	const uint8_t pll_outdiv = 0;
	const uint8_t core_clk_out1_diven = 0;
	const uint8_t core_clk_sel1 = 0;
	const uint8_t core_clk_sel0 = 0;
	const uint8_t pll_clk_gate = 0;
	const uint8_t pll_recfg = 1;
	const uint8_t cfg_cpm = 1;
	unsigned char buf[8];
	uint32_t cfg;
	for (freq_div = 1; freq_div <= 32; ++freq_div)
	{
		freq_mult = mhz * freq_div / 25;
		if (freq_mult > 0x80)
			freq_mult = 0x80;
		actual_mhz = 25. / freq_div * freq_mult;
		if (last_actual_mhz > actual_mhz)
		{
			--freq_div;
			freq_mult = last_freq_mult;
			break;
		}
		if (actual_mhz > mhz - .5)
			break;
		last_actual_mhz = actual_mhz;
		last_freq_mult = freq_mult;
	}
	const uint8_t pll_F = freq_mult - 1;
	const uint8_t pll_R = freq_div - 1;
	cfg = (pll_bypass << 31) | (pll_bandselect << 30) | (pll_outdiv << 28) | (pll_F << 21) | (pll_R << 16) | (core_clk_out1_diven << 6) | (core_clk_sel1 << 5) | (core_clk_sel0 << 4) | (pll_clk_gate << 3) | (pll_recfg << 2) | (cfg_cpm << 0);
	buf[0] = 0x55;
	buf[1] = 0xaa;
	buf[2] = 0xe0 + (chip_id == 0xf ? 0xf : chip_id % GC3355_MAX_CHIPS);
	buf[3] = 0;
	buf[4] = cfg & 0xff;
	buf[5] = (cfg >> 8) & 0xff;
	buf[6] = (cfg >> 16) & 0xff;
	buf[7] = (cfg >> 24) & 0xff;
	gc3355_write(gc3355, buf, 8);
	if(chip_id < GC3355_MAX_CHIPS)
	{
		for(i = 0; i < gc3355->chips; i++)
		{
			if((i - chip_id) >= 0 && !((i - chip_id) % GC3355_MAX_CHIPS))
			{
				gc3355->freq[i] = freq;
				applog(LOG_INFO, "%d@%d: Set GC3355 core frequency to %dMhz", gc3355->id, i, gc3355->freq[i]);
			}
		}
	}
	else
	{
		for(i = 0; i < gc3355->chips; i++)
		{
			gc3355->freq[i] = freq;
		}
		applog(LOG_INFO, "%d: Set GC3355 core frequency to %dMhz", gc3355->id, freq);
	}
}

static unsigned short fix_freq(unsigned short freq)
{
	return freq >= GC3355_MIN_FREQ ? (freq < GC3355_MAX_FREQ ? freq : GC3355_MAX_FREQ) : GC3355_MIN_FREQ;
}

static unsigned short next_freq(struct gc3355_dev *gc3355, int chip_id)
{
	return gc3355->freq[chip_id] <= gc3355->adjust[chip_id] - GC3355_OVERCLOCK_FREQ_STEP ? gc3355->freq[chip_id] + GC3355_OVERCLOCK_FREQ_STEP : gc3355->freq[chip_id];
}

static unsigned short prev_freq(struct gc3355_dev *gc3355, int chip_id)
{
	return gc3355->freq[chip_id] - GC3355_OVERCLOCK_FREQ_STEP >= GC3355_MIN_FREQ ? gc3355->freq[chip_id] - GC3355_OVERCLOCK_FREQ_STEP : gc3355->freq[chip_id];
}

static bool is_global_freq(struct gc3355_dev *gc3355)
{
	int i;
	unsigned short freq = gc3355->freq[0];
	for(i = 0; i < gc3355->chips; i++)
	{
		if(gc3355->freq[i] != freq) return false;
	}
	return true;
}

static void gc3355_reset_single(struct gc3355_dev *gc3355, unsigned char chip_id)
{
	if(gc3355->time_now != NULL && gc3355->time_now[0])
	{
		struct timeval timestr;
		double time_now;
		gettimeofday(&timestr, NULL);
		time_now = timestr.tv_sec + timestr.tv_usec / 1000000.0;
		if(time_now - gc3355->time_now[0] < 0.1)
		{
			usleep(1000000 * (0.1 - time_now + gc3355->time_now[0]));
		}
	}
	if(chip_id == 0xf)
		applog(LOG_DEBUG, "%d: Resetting GC3355 chips", gc3355->id);
	else
		applog(LOG_DEBUG, "%d: Resetting GC3355 chip #%d", gc3355->id, chip_id);
	gc3355_send_chip_cmds(gc3355, single_cmd_reset, chip_id);
	usleep(70000);
}

static void gc3355_reset_all(struct gc3355_dev *gc3355)
{
	gc3355_reset_single(gc3355, 0xf);
}

/*
 * miner thread
 */
static void *gc3355_thread(void *userdata)
{
	struct thr_info	*mythr = userdata;
	int thr_id = mythr->id;
	struct gc3355_dev *gc3355;
	struct work work = {};
	unsigned char *scratchbuf = NULL;
	int i, rc;
	struct timeval timestr;
	struct device_frequency_list *device;
	struct chip_frequency_list *chip;
	
	work.thr_id = thr_id;
	gettimeofday(&timestr, NULL);
	gc3355 = &gc3355_devs[thr_id];
	gc3355->id = thr_id;
	gc3355->dev_fd = -1;
	gc3355->resend = true;

	scratchbuf = scrypt_buffer_alloc();

	applog(LOG_INFO, "%d: GC3355 chip mining thread started, in SINGLE mode", thr_id);
	if (gc3355_open(gc3355, B115200))
	{
		can_start++;
		gc3355_exit(gc3355);
	}
	gc3355_send_cmds(gc3355, gcp_cmd_reset);
	sleep(1);
	// clear read buffer
	do
	{
		unsigned char buf[1];
		rc = read(gc3355->dev_fd, buf, 1);
	}
	while(rc);
	gc3355_send_cmds(gc3355, single_cmd_init);
	gc3355_reset_all(gc3355);
	uint32_t fw_version = gc3355_get_firmware_version(gc3355);
	applog(LOG_INFO, "%d: Firmware version: 0x%08x", thr_id, fw_version);
	gc3355->chips = opt_gc3355_chips;
	if((fw_version & 0xffff) == 0x1401)
	{
		gc3355->type = 1;
		gc3355->chips = 5;
		applog(LOG_INFO, "%d: %s detected", thr_id, GC3355_USB_STR);
	}
	else if((fw_version & 0xffff) == 0x1402)
	{
		gc3355->type = 2;
		gc3355->chips = 40;
		applog(LOG_INFO, "%d: %s detected", thr_id, GC3355_BLADE_STR);
	}
	else
	{
		gc3355->type = 0;
		applog(LOG_INFO, "%d: %s detected (chips=%d)", thr_id, GC3355_NONE_STR, gc3355->chips);
	}
	
	gc3355->freq = calloc(gc3355->chips, sizeof(unsigned short));
	gc3355->last_nonce = calloc(gc3355->chips, sizeof(uint32_t));
	gc3355->hashes = calloc(gc3355->chips, sizeof(unsigned long long));
	gc3355->time_now = calloc(gc3355->chips, sizeof(double));
	gc3355->time_spent = calloc(gc3355->chips, sizeof(double));
	gc3355->total_hwe = calloc(gc3355->chips, sizeof(unsigned short));
	gc3355->hwe = calloc(gc3355->chips, sizeof(unsigned short));
	gc3355->adjust = calloc(gc3355->chips, sizeof(unsigned short));
	gc3355->steps = calloc(gc3355->chips, sizeof(unsigned short));
	gc3355->autotune_accepted = calloc(gc3355->chips, sizeof(unsigned int));
	gc3355->accepted = calloc(gc3355->chips, sizeof(unsigned int));
	gc3355->rejected = calloc(gc3355->chips, sizeof(unsigned int));
	gc3355->hashrate = calloc(gc3355->chips, sizeof(double));
	gc3355->shares = calloc(gc3355->chips, sizeof(unsigned long long));
	gc3355->last_share = calloc(gc3355->chips, sizeof(unsigned int));
	list_for_each_entry(device, &frequency_list->list, list)
	{
		if(!strcmp(device->devname, gc3355->devname) || (gc3355->serial != NULL && !strcmp(device->devname, gc3355->serial)))
		{
			list_for_each_entry(chip, &device->chip_list.list, list)
			{
				if(chip->chip_id == 0xf)
				{
					for(i = 0; i < gc3355->chips; i++)
					{
						if(!gc3355->freq[i])
							gc3355->freq[i] = fix_freq(chip->freq);
					}
				}
				else
					gc3355->freq[chip->chip_id % GC3355_MAX_CHIPS] = fix_freq(chip->freq);
			}
		}
	}
	for(i = 0; i < gc3355->chips; i++)
	{
		gc3355->adjust[i] = GC3355_MAX_FREQ;
		gc3355->last_share[i] = timestr.tv_sec;
		if(!gc3355->freq[i])
			gc3355->freq[i] = fix_freq(opt_frequency);
	}
	if(!is_global_freq(gc3355))
	{
		for(i = 0; i < gc3355->chips; i++)
		{
			if(i == GC3355_MAX_CHIPS)
				break;
			gc3355_set_core_freq(gc3355, i, gc3355->freq[i]);
		}
	}
	else
	{
		gc3355_set_core_freq(gc3355, 0xf, gc3355->freq[0]);
	}
	rc = 0;
	uint32_t midstate[8];
	can_start++;
	gc3355->ready = true;
	if(can_start == opt_n_threads)
	{
		free_device_frequency_list(frequency_list);
	}
	while(1)
	{
		if (have_stratum)
		{
			while (can_start < opt_n_threads || !can_work || g_work.job_id == NULL || time(NULL) >= g_work_time + 120)
			usleep(10000);
		}
		if (work_restart[thr_id].restart || strcmp(work.job_id, g_work.job_id) || work.work_id != g_work.work_id)
		{
			gc3355_reset_all(gc3355);
			pthread_mutex_lock(&g_work_lock);
			pthread_mutex_lock(&stratum->work_lock);
			stratum_gen_work(stratum, &work);
			pthread_mutex_unlock(&stratum->work_lock);
			pthread_mutex_unlock(&g_work_lock);
			sha256_init(midstate);
			sha256_transform(midstate, work.data, 0);
			gc3355->resend = true;
		}
		else
		{
			gc3355->resend = false;
		}
		work_restart[thr_id].restart = 0;
		
		rc = gc3355_scanhash(gc3355, &work, scratchbuf, midstate);
		if(rc == -1)
		{
			continue;
		}
		if (rc && !submit_work(mythr, &work))
			break;
	}
	gc3355_exit(gc3355);
	return NULL;
}

static void gc3355_restart(struct gc3355_dev *gc3355)
{
	int i, ret;
	struct timeval timestr;
	gettimeofday(&timestr, NULL);
	applog(LOG_INFO, "%d: Restarting GC3355", gc3355->id);
	gc3355_send_cmds(gc3355, gcp_cmd_reset);
	sleep(1);
	// clear read buffer
	do
	{
		unsigned char buf[1];
		ret = read(gc3355->dev_fd, buf, 1);
	}
	while(ret);
	gc3355_send_cmds(gc3355, single_cmd_init);
	bool is_global = is_global_freq(gc3355);
	for(i = 0; i < gc3355->chips; i++)
	{
		gc3355->last_nonce[i] = 0;
		gc3355->hashes[i] = 0;
		gc3355->time_now[i] = 0;
		gc3355->time_spent[i] = 0;
		gc3355->hwe[i] = 0;
		gc3355->adjust[i] = GC3355_MAX_FREQ;
		gc3355->steps[i] = 0;
		gc3355->hashrate[i] = 0;
		gc3355->last_share[i] = timestr.tv_sec;
		if(!is_global)
		{
			if(i < GC3355_MAX_CHIPS)
				gc3355_set_core_freq(gc3355, i, gc3355->freq[i]);
		}
	}
	if(is_global)
	{
		gc3355_set_core_freq(gc3355, 0xf, gc3355->freq[0]);
	}
}

/* scan hash in GC3355 chips */
static int gc3355_scanhash(struct gc3355_dev *gc3355, struct work *work, unsigned char *scratchbuf, uint32_t *midstate)
{
	uint32_t *pdata = work->data;
	const uint32_t *ptarget = work->target;
	int i, ret;
	int thr_id = gc3355->id;
	unsigned char rptbuf[12];
	struct timeval timestr;
	double time_now;
	
	if (gc3355->resend)
	{
		if(opt_gc3355_timeout > 0)
		{
			unsigned int last = 0;
			for(i = 0; i < gc3355->chips; i++)
			{
				if(gc3355->last_share[i] > last)
					last = gc3355->last_share[i];
			}
			gettimeofday(&timestr, NULL);
			if(timestr.tv_sec - last > opt_gc3355_timeout)
			{
				gc3355_restart(gc3355);
				gc3355_reset_all(gc3355);
			}
		}
		unsigned char bin[156];
		// swab for big endian
		uint32_t midstate2[8];
		uint32_t data2[20];
		uint32_t target2[8];
		for(i = 0; i < 19; i++)
		{
			data2[i] = htole32(pdata[i]);
			if(i >= 8) continue;
			target2[i] = htole32(ptarget[i]);
			midstate2[i] = htole32(midstate[i]);
		}
		data2[19] = 0;
		memset(bin, 0, sizeof(bin));
		memcpy(bin, "\x55\xaa\x1f\x00", 4);
		memcpy(bin+4, (unsigned char *)target2, 32);
		memcpy(bin+36, (unsigned char *)midstate2, 32);
		memcpy(bin+68, (unsigned char *)data2, 80);
		memcpy(bin+148, "\xff\xff\xff\xff", 4);
		memcpy(bin+152, (unsigned char[]){work->work_id >> 24, work->work_id >> 16, work->work_id >> 8, work->work_id}, 4);
		gc3355_write(gc3355, bin, 156);
		gc3355->resend = false;
		gettimeofday(&timestr, NULL);
		time_now = timestr.tv_sec + timestr.tv_usec / 1000000.0;
		for(i = 0; i < gc3355->chips; i++)
		{
			gc3355->time_now[i] = time_now;
			gc3355->last_nonce[i] = i * (0xffffffff / gc3355->chips);
		}
		applog(LOG_DEBUG, "%d: Dispatching new work to GC3355 cores (0x%x)", gc3355->id, work->work_id);
	}
	
	while(!work_restart[thr_id].restart && (ret = gc3355_gets(gc3355, (unsigned char *)rptbuf, 12)) <= 0 && !work_restart[thr_id].restart)
	{
		if (rptbuf[0] == 0x55 && rptbuf[1] == 0x20)
		{
			uint32_t nonce, work_id, hash[8];
			const uint32_t Htarg = ptarget[7];
			int stop, chip_id;
			unsigned short freq;
			unsigned int add_hashes = 0;
			unsigned char add_hwe = 0;
			
			if(rptbuf[2] || rptbuf[3])
			{
				applog(LOG_DEBUG, "%d: Invalid response: (0x5520%02x%02x%02x%02x%02x%02x)", gc3355->id, rptbuf[2], rptbuf[3], rptbuf[4], rptbuf[5], rptbuf[6], rptbuf[7]);
				continue;
			}
			
			// swab for big endian
			memcpy((unsigned char *)&nonce, rptbuf+4, 4);
			nonce = htole32(nonce);
			memcpy((unsigned char *)&work_id, rptbuf+8, 4);
			work_id = htobe32(work_id);
			memcpy(pdata+19, &nonce, sizeof(nonce));
			scrypt_1024_1_1_256(pdata, hash, midstate, scratchbuf);
				
			stop = 1;
			chip_id = nonce / (0xffffffff / gc3355->chips);
			if(work_id != g_work.work_id)
			{
				applog(LOG_DEBUG, "%d@%d: Work_id differs (%08x != %08x)", gc3355->id, chip_id, work_id, g_work.work_id);
				continue;
			}
			if(work_restart[thr_id].restart || !can_work)
			{
				applog(LOG_DEBUG, "%d@%d: Scanhash restart requested", gc3355->id, chip_id);
				gc3355->last_nonce[chip_id] = nonce;
				break;
			}
			gettimeofday(&timestr, NULL);
			time_now = timestr.tv_sec + timestr.tv_usec / 1000000.0;
			freq = gc3355->freq[chip_id];
			if (hash[7] <= Htarg && fulltest(hash, ptarget))
			{
				if(nonce < gc3355->last_nonce[chip_id])
				{
					gc3355->last_nonce[chip_id] = chip_id * (0xffffffff / gc3355->chips);
				}
				add_hashes = nonce - gc3355->last_nonce[chip_id];
				applog(LOG_DEBUG, "%d@%d %dMHz: Got nonce %08x, Hash <= Htarget! (0x%x) %.1lf KH/s", gc3355->id, chip_id, freq, nonce, work_id, (add_hashes / (time_now - gc3355->time_now[chip_id])) / 1000);
			}
			else
			{
				add_hwe = 1;
				stop = -1;
				applog(LOG_DEBUG, "%d@%d %dMHz: Got nonce %08x, Invalid nonce! (%d/%d) (0x%x)", gc3355->id, chip_id, freq, nonce, gc3355->hwe[chip_id] + 1, GC3355_OVERCLOCK_MAX_HWE, work_id);
			}
			pthread_mutex_lock(&stats_lock);
			gc3355->hashes[chip_id] += add_hashes;
			gc3355->total_hwe[chip_id] += add_hwe;
			gc3355->hwe[chip_id] += add_hwe;
			gc3355->time_spent[chip_id] += time_now - gc3355->time_now[chip_id];
			gc3355->hashrate[chip_id] = gc3355->hashes[chip_id] / gc3355->time_spent[chip_id];
			gc3355->last_nonce[chip_id] = nonce;
			gc3355->time_now[chip_id] = time_now;
			if(opt_gc3355_autotune)
			{
				if(gc3355->type == 0)
				{
					applog(LOG_ERR, "%d: %s cannot be autotuned", gc3355->id, GC3355_NONE_STR);
				}
				else if(gc3355->type == 2)
				{
					applog(LOG_ERR, "%d: %s cannot be autotuned", gc3355->id, GC3355_BLADE_STR);
				}
				else if(gc3355->type == 1 && gc3355->adjust[chip_id] > 0)
				{
					gc3355->steps[chip_id] += stratum->job.diff;
					if(gc3355->hwe[chip_id] >= GC3355_OVERCLOCK_MAX_HWE || (gc3355->hwe[chip_id] > 0 && (GC3355_OVERCLOCK_ADJUST_STEPS / 2) / stratum->job.diff >= 3 && gc3355->steps[chip_id] >= GC3355_OVERCLOCK_ADJUST_STEPS / 2 && gc3355->hashrate[chip_id] < GC3355_HASH_SPEED * freq * 0.8))
					{
						freq = prev_freq(gc3355, chip_id);
						gc3355->adjust[chip_id] = freq;
					}
					else
					{
						unsigned short steps = GC3355_OVERCLOCK_ADJUST_STEPS - gc3355->steps[chip_id];
						if(GC3355_OVERCLOCK_ADJUST_MIN > gc3355->autotune_accepted[chip_id] && steps < stratum->job.diff * (GC3355_OVERCLOCK_ADJUST_MIN - gc3355->autotune_accepted[chip_id]))
						{
							steps = stratum->job.diff * (GC3355_OVERCLOCK_ADJUST_MIN - gc3355->autotune_accepted[chip_id]);
						}
						if(gc3355->hashrate[chip_id] < GC3355_HASH_SPEED * freq * GC3355_TRESHOLD)
						{
							unsigned short prev_f = prev_freq(gc3355, chip_id);
							if(gc3355->steps[chip_id] >= GC3355_OVERCLOCK_ADJUST_STEPS && gc3355->autotune_accepted[chip_id] >= GC3355_OVERCLOCK_ADJUST_MIN)
							{
								if(prev_f != freq)
								{
									freq = prev_f;
									gc3355->adjust[chip_id] = freq;
								}
								else
								{
									gc3355->adjust[chip_id] = -1;
									applog(LOG_DEBUG, "%d@%d: autotune stopped", gc3355->id, chip_id);
								}
							}
							else
							{
								applog(LOG_DEBUG, "%d@%d: ~%d steps until frequency adjusts to %dMHz", gc3355->id, chip_id, steps, prev_f);
							}
						}
						else
						{
							unsigned short next_f = next_freq(gc3355, chip_id);
							if(gc3355->steps[chip_id] >= GC3355_OVERCLOCK_ADJUST_STEPS)
							{
								if(next_f != freq)
									freq = next_f;
								else
								{
									gc3355->adjust[chip_id] = -1;
									applog(LOG_DEBUG, "%d@%d: autotune stopped", gc3355->id, chip_id);
								}
							}
							else
							{
								if(next_f != freq)
									applog(LOG_DEBUG, "%d@%d: ~%d steps until frequency adjusts to %dMHz", gc3355->id, chip_id, steps, next_f);
								else
									applog(LOG_DEBUG, "%d@%d: ~%d steps until autotune stops", gc3355->id, chip_id, steps);
							}
						}
					}
					if(freq != gc3355->freq[chip_id])
					{
						gc3355->hashes[chip_id] = 0;
						gc3355->time_spent[chip_id] = 0;
						gc3355_set_core_freq(gc3355, chip_id, freq);
						gc3355->hwe[chip_id] = 0;
						gc3355->steps[chip_id] = 0;
						gc3355->autotune_accepted[chip_id] = 0;
					}
				}
			}
			pthread_mutex_unlock(&stats_lock);
			return stop;
		}
		else if(ret == 0)
		{
			applog(LOG_DEBUG, "%d: Invalid header: (0x%02x%02x%02x%02x)", gc3355->id, rptbuf[0], rptbuf[1], rptbuf[2], rptbuf[3]);
			continue;
		}
	}
	return 0;
}

/*
 * create miner thread
 */
static int create_gc3355_miner_threads(struct thr_info *thr_info, int opt_n_threads)
{
	struct thr_info *thr;
	int i;
	uint16_t freq;
	uint8_t chip_id;
	char *p, *pd, *end, *str, *end2, *tmp, *devname;
	struct timeval timestr;
	struct gc3355_devices *device;
	
	gettimeofday(&timestr, NULL);
	gc3355_time_start = timestr.tv_sec;
	
	frequency_list = init_device_frequency_list();
	
	if(opt_gc3355_frequency != NULL)
	{
		pd = opt_gc3355_frequency;
		while((str = strtok_r(pd, ",", &end)))
		{
			devname = strtok_r(str, ":", &end2);
			tmp = strtok_r(NULL, ":", &end2);
			if(tmp == NULL)
				goto next;
			freq = atoi(tmp);
			chip_id = 0xf;
			tmp = strtok_r(NULL, ":", &end2);
			if(tmp != NULL)
				chip_id = atoi(tmp) % GC3355_MAX_CHIPS;
			add_frequency(frequency_list, devname, chip_id, freq);
	next:
			pd = end;
		}
	}
	
	pd = gc3355_devname;
	for (i = 0; i < opt_n_threads; i++)
	{
		thr = &thr_info[i];
		thr->id = i;
		
		if(opt_gc3355_detect)
		{
			device = gc3355_get_next_device(device_list);
			gc3355_devs[i].devname = strdup(device->path);
			gc3355_devs[i].serial = strdup(device->serial);
			gc3355_free_device(device);
		}
		else
		{
			p = strchr(pd, ',');
			if(p != NULL)
				*p = '\0';
			gc3355_devs[i].devname = strdup(pd);
			device = gc3355_get_device(device_list, gc3355_devs[i].devname);
			if(device != NULL)
			{
				gc3355_devs[i].serial = strdup(device->serial);
				gc3355_free_device(device);
			}
			pd = p + 1;
		}
		
		pthread_attr_t attrs;
		pthread_attr_init(&attrs);
		if(unlikely(pthread_attr_setdetachstate(&attrs, PTHREAD_CREATE_DETACHED)))
		{
			applog(LOG_ERR, "%d: Failed to detach GC3355 chip mining thread", thr->id);
			return 1;
		}
		if (unlikely(pthread_create(&thr->pth, &attrs, gc3355_thread, thr)))
		{
			applog(LOG_ERR, "%d: GC3355 chip mining thread create failed", thr->id);
			return 1;
		}
		usleep(100000);
	}
	if(gc3355_devname != NULL)
		free(gc3355_devname);
	if(!opt_gc3355_detect)
		gc3355_free_device_list(device_list);
	return 0;
}