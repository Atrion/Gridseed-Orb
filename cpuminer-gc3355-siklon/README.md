cpuminer-gc3355
==============

CPUMiner with GridSeed GC3355 support

How to compile from git (Debian-based):

```
apt-get update
apt-get install -y build-essential libtool libcurl4-openssl-dev libjansson-dev libudev-dev libncurses5-dev autoconf automake
git clone https://github.com/siklon/cpuminer-gc3355
cd cpuminer-gc3355
./autogen.sh
./configure CFLAGS="-O3"
make
```

Failover pool strategy is supported (see config). The first pool specified is the main pool.

GC3355-specific options:

```
--gc3355=DEV0,DEV1,...,DEVn      				enable GC3355 chip mining mode (default: no)
--gc3355-detect					      			automatically detect GC3355 miners (default: no)
--freq=FREQUENCY  								set GC3355 core frequency in NONE dual mode (default: 600)
--gc3355-freq=DEV0:F0,DEV1:F1,...,DEVn:Fn		individual frequency setting
--gc3355-freq=DEV0:F0:CHIP0,...,DEVn:Fn:CHIPn	individual per chip frequency setting
--gc3355-autotune  								auto overclocking each GC3355 chip (default: no)
--gc3355-timeout=N  							max. time in seconds after no share is submitted before restarting GC3355 (default: never)
```

There are multiple ways to set the frequency.

By device name:

Linux: `--gc3355-freq=/dev/ttyACM0:850` Windows: `--gc3355-freq=\\.\COM3:850`

By serial string:

`--gc3355-freq=8D751F965355:850`

If you cannot find any /dev/ttyUSB or /dev/ttyACM, it related to running cgminer, this can easily be fixed by rebooting the system.

You do not need the set the # of chips for USB Miner or G-Blade, it is detected automatically

For the G-Blade, no additional command line parameters are needed, it will be detected automatically.
You can only set the frequency of chip_id 0-7, each chip_id represents 5 chips in this case.
You cannot set the frequency of an individual chip on your G-Blade.

Config
==============

Use JSON config with `-c name_of_config`

Example JSON Config:

```
{
	"gc3355-detect" : true,
	"gc3355-freq" : [
		"\\\\.\\COM3:850", "\\\\.\\COM3:875:0", "\\\\.\\COM3:900:3",
		"\\\\.\\COM4:900",
		"\\\\.\\COM5:875"
	],
	"gc3355-autotune" : true,
	"pools" : [
		{
			"url" : "stratum+tcp://eu.wafflepool.com:3333",
			"user" : "1AMsjqzXQpRunxUmtn3xzQ5cMdhV7fmet2",
			"pass" : "x"
		},
		{
			"url" : "stratum+tcp://doge.ghash.io:3333",
			"user" : "user",
			"pass" : "x"
		}
	],
	"freq" : "850",
	"debug" : true
}
```

API
==============
The API is accessible on port 4028 (by default), to change the port pass --api-port=PORT

One GET command is currently supported:
```
{"get":"stats"}\n
```
This will output a JSON encoded array with mining stats for each GC3355 chip.

One SET command is currently supported:
```
{"set":"frequency", "devices":{"ttyACM0":{"chips":[825,850,875,900,850]}}}\n
```
This will set the frequency on the fly of the GC3355 chips to 825MHz (chip0), 850MHz (chip1), 875MHz (chip2), 900MHz (chip3), 850MHz (chip4)

You can specify multiple devices, but the length of the chips array must be equal to the number of chips on the GC3355 miners, Blades have 40 chips but you can only address chip0-7 (clusters of 5 chips), so the max is 8.

To translate the JSON keys, please refer to cpu-miner.c:66

Do not forget the newline (\n), it is used to tell the API to stop reading and execute the command!

Windows is not supported.

Binaries
==============

Windows: https://www.dropbox.com/s/ttqa9p851siz8oi/minerd-gc3355.zip

Raspberry PI: https://www.dropbox.com/s/xc3lvysi8vtrt00/minerd-gc3355

Support
==============

`BTC: 1AMsjqzXQpRunxUmtn3xzQ5cMdhV7fmet2`


`LTC: Lc75scqhMCkpMhC3aYGPVB4BEAzHvvz2rm`


`DOGE: DFZ3rxAUgFspMfpZbqMzgRFFQKiT695HCo`