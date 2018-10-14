// OSCII-bot
// Copyright (C) 2013 Cockos Incorporated
// License: GPL

#include "../WDL/swell/swell.h"

#include <jack/jack.h>
#include <jack/midiport.h>
#include <jack/ringbuffer.h>

#define NO_DEFINE_LINUX_MIDI_REFS
#include "device.h"

static jack_client_t *g_client;
extern WDL_PtrList<ioDevice> g_devices;

static int process(jack_nframes_t nframes, void *refCon)
{
  WDL_PtrList<ioDevice> *devices=(WDL_PtrList<ioDevice>*)refCon;
  if (devices) {
    for (int i=0;i<devices->GetSize();i++)
    {
      ioDevice *dev=devices->Get(i);
      if (dev && !strcmp(dev->get_type(),"MIDI"))
      {
        if (dev->m_has_input)
        {
          midiInputDevice *mid = (midiInputDevice *)dev;
          if (mid && mid->m_port)
          {
            void* portBuf = jack_port_get_buffer(mid->m_port, nframes);
            if (portBuf) {
              jack_midi_event_t ev;
              for (unsigned int j = 0; j < jack_midi_get_event_count(portBuf); ++j)
              {
                jack_midi_event_get(&ev,portBuf,j);
                mid->ProcessPacket( (unsigned char *)ev.buffer, ev.size, &mid->m_curstatus);
              }
            }
          }
        }
        else if(dev->m_has_output)
        {
          midiOutputDevice *mod = (midiOutputDevice *)dev;
          if (mod && mod->m_port && mod->m_ring)
          {
            void* portBuf = jack_port_get_buffer(mod->m_port, nframes);
            if (portBuf)
            {
              jack_midi_clear_buffer(portBuf);
              int len;
              while(jack_ringbuffer_read_space(mod->m_ring) >= (int)sizeof(int))
              {
                int lenResult = jack_ringbuffer_read(mod->m_ring, (char*)&len, sizeof(int));  // length of upcoming MIDI msg
                if (lenResult==(int)sizeof(int))
                {
                  if (len>0)
                  {
                    if ((unsigned int)len<=jack_midi_max_event_size(portBuf))
                    {
                      if (len==3) 
                      {
                        char msg[3];
                        int msgResult = jack_ringbuffer_read(mod->m_ring,msg,len);
                        if (msgResult==len)
                        {
                          int out = jack_midi_event_write(portBuf,0,(const unsigned char*)msg,len);   // send on frame 0
                          if(out) fprintf(stderr,"OSCII-bot/Jack: Could not write MIDI event to port buffer\n");
                        }
                        else 
                        {
                          fprintf(stderr,"OSCII-bot/Jack: Could not read full MIDI message from ringbuffer\n");
                          jack_ringbuffer_reset(mod->m_ring);   // should never happen but if something goes wrong make sure we dont read len as MIDI or v/v
                        }
                      }
                      else
                      {
                        char *msg=(char*)malloc(len);
                        if (msg!=NULL)
                        {
                          int msgResult = jack_ringbuffer_read(mod->m_ring,msg,len);
                          if (msgResult==len)
                          {
                            int out = jack_midi_event_write(portBuf,0,(const unsigned char*)msg,len);
                            if(out) fprintf(stderr,"OSCII-bot/Jack: Could not write MIDI event to port buffer\n");
                          }
                          else 
                          {
                            fprintf(stderr,"OSCII-bot/Jack: Could not read full MIDI message from ringbuffer\n");
                            jack_ringbuffer_reset(mod->m_ring);
                          }
                          free(msg);
                        }
                      }
                    }
                    else 
                    {
                      fprintf(stderr,"OSCII-bot/Jack: Event too large for port buffer, skipping outgoing MIDI message\n");
                      jack_ringbuffer_reset(mod->m_ring);
                    }
                  }
                }
                else 
                {
                  fprintf(stderr,"OSCII-bot/Jack: Could not read length of MIDI message from ringbuffer\n");
                  jack_ringbuffer_reset(mod->m_ring);
                }
              }
            }
          }
        }
      }
    }
  }
  return 0;
}

static int jack_port_is_mine_name(jack_client_t *cl, const char *nm)
{
  if (cl && nm)
  {
    jack_port_t* p=jack_port_by_name(cl,nm);
    if (p && jack_port_is_mine(cl,p)) return 1;
  }
  return 0;
}

midiInputDevice::midiInputDevice(const char *namesubstr, int skipcnt, WDL_PtrList<ioDevice> *reuseDevList) 
{
  m_has_input=true;
  if (!g_client) g_client = jack_client_open ("OSCII MIDI IO", JackNullOption, NULL);

  m_running=false;
  m_name_substr = strdup(namesubstr);
  m_input_skipcnt=skipcnt;
  m_handle = NULL;   // MIDI source (in case of midiInputDevice) or destination (in case of midiOutputDevice)
  m_port = NULL;  // port this client created
  m_name_used=0;
  m_open_would_use_altdev = NULL;
  m_last_dev_idx=-1;
  m_curstatus=0;
  m_lastmsgtime = time(NULL);
  m_failed_time = time(NULL);
  
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
      
      onMessage(0,data,i);
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



void midiInputDevice::do_open(WDL_PtrList<ioDevice> *reuseDevList)
{
  do_close();

  const char ** ports=jack_get_ports(g_client,NULL,JACK_DEFAULT_MIDI_TYPE,JackPortIsOutput);
  if (ports) 
  {
    int x;
    int skipcnt = m_input_skipcnt;
    for(x=0;ports[x];x++)
    {
      if ((!m_name_substr[0] || strstr(ports[x],m_name_substr)) && !skipcnt-- && !jack_port_is_mine_name(g_client,ports[x]))
      {
        free(m_name_used);
        m_name_used = strdup(ports[x]);

        if (reuseDevList)
        {
          int i;
          for (i=0;i<reuseDevList->GetSize();i++)
          {
            ioDevice *dev=reuseDevList->Get(i);
            if (dev && !strcmp(dev->get_type(),"MIDI") && dev->m_has_input)
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
        m_last_open_time=get_time_precise();

        if (g_client)
        {
          jack_deactivate(g_client);  // jack_deactivate() destroys all connections
          jack_set_process_callback (g_client, process, &g_devices);
          char buf[512];
          sprintf(buf,"Input port from %s",m_name_substr);
          m_port=jack_port_register(g_client, buf, JACK_DEFAULT_MIDI_TYPE, JackPortIsInput, 0);
          if (!jack_activate(g_client)) 
          {
            if (m_port)
            {
              m_handle = jack_port_by_name(g_client,ports[x]);
              if (!m_handle)
              {
                jack_port_unregister(g_client, m_port);
                m_port=NULL;
              }
            }
          }
          else fprintf(stderr,"OSCII-bot/Jack: Could not activate client\n");
        }
        break;
      }
    }
  }
  jack_free(ports);
}

void midiInputDevice::do_close()
{
  if (m_handle) 
  {
    if (m_port && m_handle) jack_disconnect(g_client, jack_port_name(m_handle), jack_port_name(m_port));
    if(m_port) jack_port_unregister(g_client, m_port);
    m_handle=NULL;
    m_port=NULL;
  }
}

void midiInputDevice::start() 
{ 
  m_running=true;
}

void midiInputDevice::run_input(WDL_FastString &textOut)
{
  const time_t now=time(NULL);
  if (!m_handle)
  {
    if (now > m_failed_time+5) // every 5s of inactivity try reopen device
    {
      m_failed_time = now;
      do_close();
      do_open();
      if (m_handle) 
      {
        textOut.AppendFormatted(1024,"***** Reopened MIDI input device %s\r\n",m_name_used);
        start();
      }
    }
  }
  else
  {
    if (now > m_lastmsgtime+1) // every 1s of activity check if port exists
    {
      m_lastmsgtime = now;
      const char ** ports=jack_get_ports(g_client,NULL,JACK_DEFAULT_MIDI_TYPE,JackPortIsOutput);
      if (ports)
      {
        bool port_exists=false;
        for(unsigned int x=0;ports[x];x++)
        {
          if (m_name_used && !strcmp(m_name_used,ports[x]))
          {
            port_exists=true;
            break;
          }
        }
        if (!port_exists)
        {
          textOut.AppendFormatted(1024,"***** Lost MIDI input device %s\r\n",m_name_used);
          do_close();
        }
      }
      jack_free(ports);
    }
    if (m_port && !jack_port_connected_to(m_port,m_name_used))
    {
      jack_connect(g_client, jack_port_name(m_handle), jack_port_name(m_port));
    }
  }
}



midiOutputDevice::midiOutputDevice(const char *namesubstr, int skipcnt, WDL_PtrList<ioDevice> *reuseDevList) 
{
  m_has_output=true;
  if (!g_client) g_client = jack_client_open ("OSCII MIDI IO", JackNullOption, NULL);

  // found device!
  m_name_substr = strdup(namesubstr);
  m_skipcnt=skipcnt;
  m_handle = NULL;
  m_port = NULL;
  m_name_used=0;
  m_open_would_use_altdev = NULL;
  m_last_dev_idx=-1;
  m_ring = jack_ringbuffer_create(65536);  // Ringbuffer size
  if (jack_ringbuffer_mlock(m_ring)) fprintf(stderr,"OSCII-bot/Jack: Could not lock memory for ringbuffer\n");
  m_lastmsgtime = time(NULL);
  m_failed_time = time(NULL);
  
  do_open(reuseDevList);
}

midiOutputDevice::~midiOutputDevice() 
{
  do_close();
  free(m_name_substr);
  free(m_name_used);
  jack_ringbuffer_free(m_ring);
}

void midiOutputDevice::do_open(WDL_PtrList<ioDevice> *reuseDevList)
{
  do_close();

  const char ** ports=jack_get_ports(g_client,NULL,JACK_DEFAULT_MIDI_TYPE,JackPortIsInput);
  if (ports) 
  {
    int x;
    int skipcnt = m_skipcnt;
    for(x=0;ports[x];x++)
    {
      if ((!m_name_substr[0] || strstr(ports[x],m_name_substr)) && !skipcnt-- && !jack_port_is_mine_name(g_client,ports[x]))
      {
        free(m_name_used);
        m_name_used = strdup(ports[x]);

        if (reuseDevList)
        {
          int i;
          for (i=0;i<reuseDevList->GetSize();i++)
          {
            ioDevice *dev=reuseDevList->Get(i);
            if (dev && !strcmp(dev->get_type(),"MIDI") && dev->m_has_output)
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
        m_last_open_time=get_time_precise();
        if (g_client)
        {
          jack_deactivate(g_client);
          jack_set_process_callback (g_client, process, &g_devices);
          char buf[512];
          sprintf(buf,"Output port to %s",m_name_substr);
          m_port=jack_port_register(g_client, buf, JACK_DEFAULT_MIDI_TYPE, JackPortIsOutput, 0);
          if (!jack_activate(g_client)) 
          {
            if (m_port)
            {
              m_handle = jack_port_by_name(g_client,ports[x]);
              if (!m_handle)
              {
                jack_port_unregister(g_client, m_port);
                m_port=NULL;
              }
            }
          }
          else fprintf(stderr,"OSCII-bot/Jack: Could not activate client\n");
        }
        break;
      }
    }
  }
  jack_free(ports);
}


void midiOutputDevice::do_close()
{
  if (m_handle) 
  {
    if (m_port && m_handle) jack_disconnect(g_client, jack_port_name(m_port), jack_port_name(m_handle));
    if(m_port) jack_port_unregister(g_client, m_port);
    m_handle=NULL;
    m_port=NULL;
  }
}


void midiOutputDevice::run_output(WDL_FastString &textOut)
{
  const time_t now=time(NULL);
  if (!m_handle)
  {
    if (now > m_failed_time+5) // every 5s of inactivity try reopen device
    {
      m_failed_time = now;
      do_close();
      do_open();
      if (m_handle) 
      {
        textOut.AppendFormatted(1024,"***** Reopened MIDI output device %s\r\n",m_name_used);
        start();
      }
    }
  }
  else
  {
    if (now > m_lastmsgtime+1) // every 1s of activity check if port exists
    {
      m_lastmsgtime = now;
      const char ** ports=jack_get_ports(g_client,NULL,JACK_DEFAULT_MIDI_TYPE,JackPortIsInput);
      if (ports)
      {
        bool port_exists=false;
        for(unsigned int x=0;ports[x];x++)
        {
          if (m_name_used && !strcmp(m_name_used,ports[x]))
          {
            port_exists=true;
            break;
          }
        }
        if (!port_exists)
        {
          textOut.AppendFormatted(1024,"***** Lost MIDI output device %s\r\n",m_name_used);
          do_close();
        }
      }
      jack_free(ports);
    }
    if (m_port && !jack_port_connected_to(m_port,m_name_used))
    {
      jack_connect(g_client, jack_port_name(m_port), jack_port_name(m_handle));
    }
  }
}

void midiOutputDevice::midiSend(const unsigned char *buf, int len)
{
  if (m_handle && len>0)
  {
    unsigned char status = buf[0];
    if (status == 0xF8 || status == 0xFA || status == 0xFB || status == 0xFC) len = 1;
    else if (status == 0xF3 || status == 0xF1 || (status&0xF0) == 0xC0 || (status&0xF0) == 0xD0) len = 2;
    
    if (m_ring)
    {
      int avail = jack_ringbuffer_write_space(m_ring);
      if (avail >= ((int)sizeof(int)+len))
      {
        int lenWritten = jack_ringbuffer_write(m_ring, (const char*)&len, sizeof(int));  // write len to ringbuffer first
        if (lenWritten==(int)sizeof(int)) 
        {
          int bufWritten = jack_ringbuffer_write(m_ring, (const char*)buf, len);
          if (bufWritten != len) fprintf(stderr,"OSCII-bot/Jack: Could not write full MIDI message to ringbuffer\n");
        }
        else fprintf(stderr,"OSCII-bot/Jack: Could not write length of MIDI message to ringbuffer\n");
      }
      else fprintf(stderr,"OSCII-bot/Jack: Ringbuffer full, skipping outgoing MIDI message\n");
    }
  }
}
