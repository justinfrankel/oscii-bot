#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <sys/time.h>
#include <math.h>

#include "../WDL/swell/swell.h"

#include <CoreMIDI/CoreMIDI.h>
#include <CoreServices/CoreServices.h>
#include <mach/mach.h>
#include <mach/mach_time.h>

#define NO_DEFINE_APPLE_MIDI_REFS
#include "device.h"

static MIDIClientRef g_client;


midiInputDevice::midiInputDevice(const char *namesubstr, int skipcnt, WDL_PtrList<inputDevice> *reuseDevList) 
{
  if (!g_client)  MIDIClientCreate(CFSTR("OSCII MIDI IO"), NULL, NULL, &g_client);

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
  int curstatus=*laststatus;
  
  while (length > 0)
  {
    unsigned char status=data[0];
    int bytesused=0;
    
    if (status == 0xF0) // sysex
    {
      int i;
      for (i=1; i < length; ++i)
      {
        if (data[i] == 0xF7) break;     
      }
      if (i >= length) break; // no 0xF7, we need to support running sysex dump, which we don't   
      ++i;
      
//      onMessage(0,i,data);
      bytesused=i;
    }
    else if (status >= 0xF4) // 1 byte message
    {
      if (status == 0xF8 || status == 0xFA || status == 0xFB || status == 0xFC) // only these 1 byte messages get added
      {
        const unsigned char m[3] = { status,0,0};
        onMessage(0,m,3);
      }
      bytesused=1;
    }
    else
    {
      unsigned char* msg=data+1;
      int msgavail=length-1;      

      if (status < 0x80) // running status
      {
        if (!curstatus) break;  // bad message, we don't know how much to advance        
        status=curstatus;
        --msg;
        ++msgavail;        
      }
      else
      {
        bytesused=1;
      }

      if (status == 0xF3 || status == 0xF1 || (status&0xF0) == 0xC0 || (status&0xF0) == 0xD0) // two-byte messages
      {
        if (msgavail < 1) break;
        const unsigned char m[3] = { status,msg[0],0};
        onMessage(0,m,3);
        ++bytesused;
      }
      else
      {
        if (msgavail < 2) break;
        const unsigned char m[3] = { status,msg[0],msg[1]};
        onMessage(0,m,3);
        bytesused += 2;
      }
      
      if (status >= 0x80 && status < 0xF0) curstatus=status;
    }
    
    data += bytesused;
    length -= bytesused;
  }
  
  *laststatus=curstatus;
}

void midiInputDevice::MyReadProc(const MIDIPacketList *pktlist, void *refCon, void *connRefCon)
{
  midiInputDevice *_this = (midiInputDevice *)refCon;
  MIDIPacket *packet = (MIDIPacket *)pktlist->packet;     // remove const (!)
    
  for (unsigned int j = 0; j < pktlist->numPackets; ++j) 
  {
    _this->ProcessPacket( (unsigned char *)packet->data,
                      packet->length,
                      &_this->m_curstatus);
      
      
    packet = MIDIPacketNext(packet);
  }
}

void midiInputDevice::do_open(WDL_PtrList<inputDevice> *reuseDevList)
{
  do_close();
  m_lastmsgtime = time(NULL);

  int x;
  const int n=MIDIGetNumberOfSources();
  int skipcnt = m_input_skipcnt;
  for(x=0;x<n;x++)
  {
    MIDIEndpointRef src = MIDIGetSource(x);
    if (src)
    {
      //get device name
      char nameptr[512];
      nameptr[0]=0;
      CFStringRef pname;        
      MIDIObjectGetStringProperty(src, kMIDIPropertyName, &pname);                
      if (pname) SWELL_CFStringToCString(pname, nameptr, sizeof(nameptr));
      // do not release pname -- we "Get" it, not create it

      if ((!m_name_substr[0] || strstr(nameptr,m_name_substr)) && !skipcnt--)
      {
        free(m_name_used);
        m_name_used = strdup(nameptr);

        if (reuseDevList)
        {
          int i;
          for (i=0;i<reuseDevList->GetSize();i++)
          {
            inputDevice *dev=reuseDevList->Get(i);
            if (dev && !strcmp(dev->get_type(),"MIDI"))
            {
              midiInputDevice *mid = (midiInputDevice *)dev;
              if (mid->m_last_dev_idx == x)
              {
                m_open_would_use_altdev = mid;
                return;
              }
            }
          }
        }

        m_last_dev_idx = x;

        if (g_client)
        {
          char buf[512];
          sprintf(buf,"Input port %d",x);
          CFStringRef pn = (CFStringRef)SWELL_CStringToCFString(buf);
          MIDIInputPortCreate(g_client, pn, MyReadProc, this, &m_port);
          CFRelease(pn);

          if (m_port)
          {
            m_handle = MIDIGetSource(x);
            if (m_handle)
            {
              MIDIPortConnectSource(m_port, m_handle, NULL);
            }
            else
            {
              MIDIPortDispose(m_port);
              m_port=0;
            }
          }
        }
        break;
      }
    }
  }
}


void midiInputDevice::do_close()
{
  if (m_handle) 
  {
    if (m_port && m_handle) MIDIPortDisconnectSource(m_port,m_handle);
    if(m_port) MIDIPortDispose(m_port);
    m_handle=0;
    m_port=0;
  }
}


void midiInputDevice::start() 
{ 
  m_running=true;
}
void midiInputDevice::run(WDL_FastString &textOut)
{
  int x;

  const time_t now=time(NULL);
  if (now > m_lastmsgtime+5) // every 5s of inactivity, query status
  {
    m_lastmsgtime = now;
    if (!m_handle)
    {
      const bool had_handle=!!m_handle;
      do_close();
      do_open();
      if (m_handle) 
      {
        textOut.AppendFormatted(1024,"***** Opened MIDI input device %s\r\n",m_name_used);
        start();
      }
      else if (had_handle) textOut.AppendFormatted(1024,"**** Closed MIDI input device %s\r\n",m_name_used);
    }
  }
}



midiOutputDevice::midiOutputDevice(const char *namesubstr, int skipcnt, WDL_PtrList<outputDevice> *reuseDevList) 
{
  if (!g_client)  MIDIClientCreate(CFSTR("OSCII MIDI IO"), NULL, NULL, &g_client);

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

void midiOutputDevice::do_open(WDL_PtrList<outputDevice> *reuseDevList)
{
  do_close();
  m_failed_time=0;

  int x;
  const int n=MIDIGetNumberOfDestinations();
  int skipcnt = m_skipcnt;
  for(x=0;x<n;x++)
  {
    MIDIEndpointRef dest = MIDIGetDestination(x);
    if (dest)
    {
      char nameptr[512];
      nameptr[0]=0;
      CFStringRef pname;        
      MIDIObjectGetStringProperty(dest, kMIDIPropertyName, &pname);                
      if (pname) SWELL_CFStringToCString(pname, nameptr, sizeof(nameptr));       
      if ((!m_name_substr[0] || strstr(nameptr,m_name_substr)) && !skipcnt--)
      {
        free(m_name_used);
        m_name_used = strdup(nameptr);

        if (reuseDevList)
        {
          int i;
          for (i=0;i<reuseDevList->GetSize();i++)
          {
            outputDevice *dev=reuseDevList->Get(i);
            if (dev && !strcmp(dev->get_type(),"MIDI"))
            {
              midiOutputDevice *mid = (midiOutputDevice *)dev;
              if (mid->m_last_dev_idx == x)
              {
                m_open_would_use_altdev = mid;
                return;
              }
            }
          }
        }

        m_last_dev_idx = x;
        if (g_client)
        {
          char buf[512];
          sprintf(buf,"Output port %d",x);
          CFStringRef pn = (CFStringRef)SWELL_CStringToCFString(buf);
          MIDIOutputPortCreate(g_client, pn, &m_port);
          CFRelease(pn);
        }
        if (m_port) m_handle = MIDIGetDestination(x);

        if (!m_handle)
        {
          if(m_port) MIDIPortDispose(m_port);
          m_port=0;
        }

        break;
      }
    }
  }
  if (!m_handle) m_failed_time=time(NULL);
}


void midiOutputDevice::do_close()
{
  if (m_handle) 
  {
    m_handle=0;

    if(m_port) MIDIPortDispose(m_port);
    m_port=0;
  }
}


void midiOutputDevice::run(WDL_FastString &textOut)
{
  if (m_failed_time && time(NULL)>m_failed_time+1)
  {
    // try to reopen
    const bool had_handle = !!m_handle;
    do_open();
    if (m_handle) textOut.AppendFormatted(1024,"***** Opened MIDI output device %s\r\n",m_name_used);
    else if (had_handle) textOut.AppendFormatted(1024,"***** Lost MIDI output device %s\r\n",m_name_used);

  }
}

void midiOutputDevice::midiSend(const unsigned char *buf, int len)
{
  if (m_handle && len>0 && len <= 3)
  {
    unsigned char status = buf[0];
    if (status == 0xF8 || status == 0xFA || status == 0xFB || status == 0xFC) len = 1;
    else if (status == 0xF3 || status == 0xF1 || (status&0xF0) == 0xC0 || (status&0xF0) == 0xD0) len = 2;    
    MIDIPacketList pktlist;
    pktlist.numPackets = 1;
    pktlist.packet[0].timeStamp = mach_absolute_time();
    pktlist.packet[0].length = len;
    memcpy(pktlist.packet[0].data, buf,len);
    MIDISend(m_port, m_handle, &pktlist);
  }
}
