// OSCII-bot
// Copyright (C) 2013 Cockos Incorporated
// License: GPL

#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <sys/time.h>
#include <math.h>

#include "../WDL/swell/swell.h"

#define NO_DEFINE_APPLE_MIDI_REFS
#include "device.h"



midiInputDevice::midiInputDevice(const char *namesubstr, int skipcnt, WDL_PtrList<ioDevice> *reuseDevList) 
{
  m_has_input=true;
//  if (!g_client)  MIDIClientCreate(CFSTR("OSCII MIDI IO"), NULL, NULL, &g_client);

  m_running=false;
  m_name_substr = strdup(namesubstr);
  m_input_skipcnt=skipcnt;
  m_handle = 0;
  m_port = 0;
  m_name_used=0;
  m_open_would_use_altdev = NULL;
  m_last_dev_idx=-1;
  m_curstatus=0;
  
  do_open(reuseDevList);
}

midiInputDevice::~midiInputDevice() 
{
  do_close();
  free(m_name_substr);
  free(m_name_used);
}

void midiInputDevice::ProcessPacket(unsigned char *data, int length, int* laststatus)
{
}

void midiInputDevice::MyReadProc(const MIDIPacketList *pktlist, void *refCon, void *connRefCon)
{
}

void midiInputDevice::do_open(WDL_PtrList<ioDevice> *reuseDevList)
{
}


void midiInputDevice::do_close()
{
}


void midiInputDevice::start() 
{ 
  m_running=true;
}
void midiInputDevice::run_input(WDL_FastString &textOut)
{
}



midiOutputDevice::midiOutputDevice(const char *namesubstr, int skipcnt, WDL_PtrList<ioDevice> *reuseDevList) 
{
  m_has_output=true;
//  if (!g_client)  MIDIClientCreate(CFSTR("OSCII MIDI IO"), NULL, NULL, &g_client);

  // found device!
  m_name_substr = strdup(namesubstr);
  m_skipcnt=skipcnt;
  m_handle=0;
  m_port = 0;
  m_name_used=0;
  m_open_would_use_altdev = NULL;
  m_last_dev_idx=-1;
  
  do_open(reuseDevList);
}

midiOutputDevice::~midiOutputDevice() 
{
  do_close();
  free(m_name_substr);
  free(m_name_used);
}

void midiOutputDevice::do_open(WDL_PtrList<ioDevice> *reuseDevList)
{
}


void midiOutputDevice::do_close()
{
}


void midiOutputDevice::run_output(WDL_FastString &textOut)
{
}

void midiOutputDevice::midiSend(const unsigned char *buf, int len)
{
}
