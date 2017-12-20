cgminer-scrypt
==============

CGMiner 4.3.5 with GridSeed and Zeus scrypt ASIC support.

This file describes Scrypt-specific settings and options.
For general CGMiner information refer to README.
Scrypt algorithm code was ported from CGMiner version 3.7.2.

## Zeus ##

	./autogen.sh
	./configure --enable-scrypt --enable-zeus
	make

### Option Summary ###

```
  --zeus-chips <chips>   Default number of chips per device
  --zeus-clock <clock>   Default chip clock speed (MHz)

  --zeus-options <ID>,<chips>,<clock>[;<ID>,<chips>,<clock>...]
                         Set chips and clock speed for individual devices

  --zeus-nocheck-golden  Skip golden nonce verification during initialization (serial mode only)
  --zeus-debug           Enable extra Zeus driver debugging output in verbose mode
```

The `zeus-chips` and `zeus-clock` options apply to every device unless overridden
on a per-device basis using `zeus-options`. The ID values to use with that option
depend on how CGMiner is communicating with the miners, explained in the next section.

### Device Selection ###

The driver supports using either libusb or directly reading/writing to the serial
port to communicate with the miner. Libusb is generally more convenient on Linux
while Windows users may prefer serial to bypass the requirement of installing special
libusb drivers. Mode selection depends on the `scan-serial` option:

 * By default with no `--scan-serial` options the driver will use libusb to autodetect
   any connected miners and to perform all device I/O operations. In this mode the ID
   assigned to each miner for use with `--zeus-options` will be the miner's serial number
   (except for Blizzard models, see note below).

 * For serial I/O each miner must be specified manually using `--scan-serial zeus:<PORT>`
   and providing the path to the serial port (i.e. `/dev/ttyUSBx` on Linux or `\\.\COMx`
   on Windows). The prefix "zeus:" is optional if CGMiner has been compiled with only the
   Zeus driver enabled, otherwise it is required. In this mode the ID assigned to each
   miner for use with `--zeus-options` will be the final part of the port (i.e.
   `ttyUSBx` or `COMx`). Note that autodetection is not supported in this mode.

 * On Linux systems only it is possible to use serial I/O and still have autodetection
   by specifying `--scan-serial zeus:auto`. In this mode the driver will use libudev to
   identify which USB-serial ports are from a Zeus miner. All I/O will still be done
   using direct serial reads and writes (not through libusb) and the ID for each miner
   will still be the final part of the port (ttyUSBx). This method is not recommended
   if multiple drivers are compiled in as autodetection can be quirky in those cases.

The following three examples are equivalent assuming three miners are connected:

	# Using libusb
	./cgminer --scrypt --zeus-chips 96 --zeus-clock 328
	
	# Direct serial I/O, manual port specification
	./cgminer --scrypt --zeus-chips 96 --zeus-clock 328 --scan-serial /dev/ttyUSB0 \
		--scan-serial /dev/ttyUSB1 --scan-serial /dev/ttyUSB2
	
	# Direct serial I/O, auto-detect ports (Linux only)
	./cgminer --scrypt --zeus-chips 96 --zeus-clock 328 --scan-serial zeus:auto

### Notes ###

Blizzard ID note: Blizzard models use a different USB-Serial chip which does not provide
a valid serial number. When using libusb mode with one of these the ID shown in CGMiner and
used with `--zeus-options` is the USB bus and device address in this format:
`<bus number>:<device address>`

Chip count for different models:
Blizzard: 6, Hurricane X2: 48, Hurricane X3: 64, Thunder X2: 96, Thunder X3: 128

The model name displayed in the CGMiner UI depends on the chip count being specified correctly.
It is not auto-determined.

Zeus driver is based on [documentation][zeus] and the official reference implementation.
Many thanks also to sling00 and LinuxETC for providing access to test hardware.

[zeus]: <http://zeusminer.com/user-manual-ver-1-0/>

- - - - - - - -

## Gridseed ##

	./autogen.sh
	./configure --enable-scrypt --enable-gridseed
	make

GC3355-specific options can be specified via `--gridseed-options` or
`"gridseed-options"` in the configuration file as a comma-separated list of
sub-options:

* baud - miner baud rate (default 115200)
* freq - any frequency multiple of 12.5 MHz, non-integer frequencies rounded up (default 600)
* pll_r, pll_f, pll_od - fine-grained frequency tuning; see below
* chips - number of chips per device (default 5)
* per_chip_stats - print per-chip nonce generations and hardware failures (only for 5-chip models)
* start_port - first port number for scrypt proxy mode (default 3350); see below
* voltage - switch the voltage to the GC3355 chips; see below
* led_off - turn off the LEDs on the Gridseed miner

When mining scrypt-only this version of cgminer does not initialize the SHA cores so that
power usage is low. On a 5-chip USB miner, power usage is around 10 W.

Gridseed support is based largely on the original [Gridseed CGMiner][] and
[dtbartle][]'s scrypt modifications.

[Gridseed CGMiner]: <https://github.com/gridseed/usb-miner/>
[dtbartle]: <https://github.com/dtbartle/cgminer-gc3355/>

### Frequency Tuning ###

If `pll_r/pll_f/pll_od` are specified, freq is ignored, and calculated as follows:
* Fin = 25
* Fref = int(Fin / (pll_r + 1))
* Fvco = int(Fref * (pll_f + 1))
* Fout = int(Fvco / (1 << pll_od))
* freq = Fout

### Dual Mining ###

When dual-mining `start_port` will set the listening proxy port of the first gridseed
device on the SHA256 instance of cgminer, with additional miners using successive ports.
The scrypt instance of cgminer will attempt to connect starting at this port.

When dual mining, start the SHA mining instance of cgminer first, wait for it to begin
mining and then start the scrypt version. The second instance will detect that the USB
ports are in use and will attempt to connect to the first via UDP.

If everything is working the same devices will appear in both cgminer windows.

### Voltage Modding ###

If `voltage=1` is set the gridseed chips will be switched to an alternate voltage.
Specifically, this flag will cause the MCU to assert the VID0 input to the voltage
regulator. This *requires* a voltmodded miner. On a stock unit this will actually
reduce the regulator's output voltage.

### More Complex Options ###

The options can also be specified for each device individually by serial number via
`--gridseed-freq` or `--gridseed-override` or their configuration file equivalents.
`--gridseed-freq` takes a comma-separated list of serial number to frequency mappings
while `--gridseed-override` takes the same format as `--gridseed-options`:

	--gridseed-freq "<SERIAL>=<FREQ>[,...]"

	--gridseed-override "<SERIAL>:freq=<FREQ>,voltage=<0/1>[,...];<SERIAL>:freq=<FREQ>[,...[;...]]"

