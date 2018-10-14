Unofficial Linux port of Cockos' [Oscii-bot](https://cockos.com/oscii-bot/) app, a scriptable OSC-to-MIDI, MIDI-to-OSC, MIDI-to-MIDI, or OSC-to-OSC converter.

## Getting Started

It uses [Jack](http://www.jackaudio.org) as MIDI interface. Start the script after Jack is running + your Device is up and it should automatically build the connections you specified in the script (see [Oscii-bot documentation](https://cockos.com/oscii-bot/oscii-bot-doc.html)).

By default it loads all scripts in the current directory of the binary but you can set a search directory by using the command line option `-dir /home/foo/bar/`

## Build
Navigate into /oscii-bot/ and type ```make``` or ```make DEBUG=1```

## Prerequisites

* Jack + Jack development packages (i.e. `libjack-dev` for Jack1 or `libjack-jackd2-dev` for Jack2)
* if you want to use ALSA MIDI devices and have Jack2 installed: [a2jmidid](http://manual.ardour.org/setting-up-your-system/setting-up-midi/midi-on-linux/)
* gcc
* make
* GTK/GDK 2.0 or 3.0 development packages
* php (php-cli should be sufficient)

## Where to get help

* [Oscii-bot documentation](https://cockos.com/oscii-bot/oscii-bot-doc.html)
* [Oscii-bot Forum](https://forum.cockos.com/forumdisplay.php?f=50)
* [Reaper Linux Forum](https://forum.cockos.com/forumdisplay.php?f=52)

##

It is also possible to use a libSwell.colortheme file to theme the look of the app. Refer to the [Reaper Linux Forum](https://forum.cockos.com/forumdisplay.php?f=52) for more info.
