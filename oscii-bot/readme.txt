OSCII-bot readme.txt
Copyright (C) 2013 and onwards, Cockos Incorporated
LICENSE: GPL

OSCII-bot is a programmable tool for doing [OSC|MIDI]<-->[OSC|MIDI] conversions. It is based on the earlier tool 
"midi2osc", but is not compatible with it.

OSCII-bot can load one or more conversion scripts, by default it will look for script files in:

~/Library/Application Support/OSCII-bot/  (OSX)
or
\Users\<username>\AppData\Roaming\OSCII-bot\ (Windows 7+)
 
You can place some of the included script files there to try them: sinewave_text.txt does no OSC or MIDI, just 
generates text.

If you wish to run OSCII-bot portably, put an empty file named OSCII-bot.ini with the executable, and it will use 
that path for script files and for configuration state.

For a reference of the scripting syntax, please see sample_script.txt

Platforms: Win32 (Windows 95+), OSX 10.5+ (x86/x86-64 only -- PPC is likely possible but not included with this build)

Source code is included. Requires Cockos WDL to compile: http://www.cockos.com/wdl . The project files included are for
VC6 or Xcode5 (with a little work it should compile with other tools).

