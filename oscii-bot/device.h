// OSCII-bot
// Copyright (C) 2013 Cockos Incorporated
// License: GPL

#ifndef _M2O_DEVICE_H_
#define _M2O_DEVICE_H_


#include "../WDL/wdlstring.h"
#include "../WDL/ptrlist.h"

extern double get_time_precise(); // ioDevice implementations will set m_last_open_time to this on open/re-open

class ioDevice {
  protected:
    ioDevice() 
    {
      m_has_input = false;
      m_has_output = false;
      m_last_open_time = 0.0;
    }
    struct rec
    {
      void (*callback)(void *d1, void *d2, char type, int msglen, void *msg); // type=0 for MIDI, 1=osc
      void *data1;
      void *data2;
    };
    WDL_TypedBuf<rec> m_instances;

  public:

    double m_last_open_time;
    bool m_has_input, m_has_output;

    virtual ~ioDevice() { };
    virtual void start() { }
    virtual void run_input(WDL_FastString &textOut)=0;
    virtual void run_output(WDL_FastString &textOut)=0;
    virtual const char *get_type()=0;

    virtual void oscSend(const char *src, int len) { }
    virtual void midiSend(const unsigned char *buf, int len) { }

    virtual void addinst(void (*callback)(void *d1, void *d2, char type, int msglen, void *msg), void *d1, void *d2)
    {
      const rec r={callback,d1,d2};
      m_instances.Add(r);
    }

    virtual void onMessage(char type, const unsigned char *msg, int len)
    {
      const int n=m_instances.GetSize();
      const rec *r = m_instances.Get();
      for (int x=0;x<n; x++)
        if (r[x].callback) r[x].callback(r[x].data1,r[x].data2,type,len,(void*)msg);
    }
};

class omniInputDevice  : public ioDevice {
  const char *m_required_mode;
public:
  omniInputDevice(const char *required_mode) { m_required_mode=required_mode; m_has_input=true; }
  virtual ~omniInputDevice() {}

  virtual void run_input(WDL_FastString &textOut) { }
  virtual void run_output(WDL_FastString &textOut) { }
  virtual const char *get_type() { return "OMNI"; }

  const char *getOmniType() { return m_required_mode; }
  void copyRecsTo(ioDevice *dest)
  {
    int x;
    for(x=0;x<m_instances.GetSize();x++)
    {
      rec *r=m_instances.Get()+x;
      dest->addinst(r->callback,r->data1,r->data2);
    }
  }
};



class midiOutputDevice : public ioDevice
{
public:
  midiOutputDevice(const char *namesubstr, int skipcnt, WDL_PtrList<ioDevice> *reuseDevList);

  virtual ~midiOutputDevice();

  virtual void run_input(WDL_FastString &textOut) {}
  virtual void run_output(WDL_FastString &textOut);
  virtual void midiSend(const unsigned char *buf, int len);
  virtual const char *get_type() { return "MIDI"; }

  char *m_name_substr,*m_name_used;
  int m_skipcnt;

  midiOutputDevice *m_open_would_use_altdev; // set during constructor if device already referenced in reuseDevList
  int m_last_dev_idx;

#ifdef _WIN32
  DWORD m_failed_time;
  HMIDIOUT m_handle;
#endif

#ifndef _WIN32
#ifdef __APPLE__
#ifndef NO_DEFINE_APPLE_MIDI_REFS
  typedef int MIDIEndpointRef;
  typedef int MIDIPortRef;
  struct MIDIPacketList;
#endif
  MIDIEndpointRef m_handle; 
  MIDIPortRef m_port;
#else
#ifndef NO_DEFINE_LINUX_MIDI_REFS
  struct jack_port_t;
  struct jack_ringbuffer_t;
#endif
  jack_port_t *m_port;
  jack_port_t *m_handle;
  jack_ringbuffer_t *m_ring;
  time_t m_lastmsgtime;
#endif
  time_t m_failed_time;
#endif

  void do_open(WDL_PtrList<ioDevice> *reuseDevList=NULL);
  void do_close();

};


class midiInputDevice : public ioDevice
{
public:
  midiInputDevice(const char *namesubstr, int skipcnt, WDL_PtrList<ioDevice> *reuseDevList);

  virtual ~midiInputDevice();
  virtual const char *get_type() { return "MIDI"; }

  virtual void start();
  virtual void run_input(WDL_FastString &textOut);
  virtual void run_output(WDL_FastString &textOut) { }

#ifdef _WIN32
  static void CALLBACK callbackFunc(
    HMIDIIN hMidiIn,  
    UINT wMsg,        
    LPARAM dwInstance, 
    LPARAM dwParam1,   
    LPARAM dwParam2    
  );


  HMIDIIN m_handle;
  WDL_PtrList<MIDIHDR> m_longmsgs;
  DWORD m_lastmsgtime;
#endif

#ifndef _WIN32
#ifdef __APPLE__
#ifndef NO_DEFINE_APPLE_MIDI_REFS
  typedef int MIDIEndpointRef; // these are UInt32
  typedef int MIDIPortRef;
  struct MIDIPacketList;
#endif
  MIDIEndpointRef m_handle; 
  MIDIPortRef m_port;
  static void MyReadProc(const MIDIPacketList *pktlist, void *refCon, void *connRefCon);
#else
#ifndef NO_DEFINE_LINUX_MIDI_REFS
  struct jack_port_t;
  typedef int jack_nframes_t; // UInt32
#endif
  jack_port_t *m_port;
  jack_port_t *m_handle;
  time_t m_failed_time;
#endif
  time_t m_lastmsgtime;
  bool m_running;
  int m_curstatus;
  void ProcessPacket(unsigned char *data, int length, int* laststatus);
#endif

  char *m_name_substr,*m_name_used;
  int m_input_skipcnt;

  midiInputDevice *m_open_would_use_altdev; // set during constructor if device already referenced in reuseDevList
  int m_last_dev_idx;

  void do_open(WDL_PtrList<ioDevice> *reuseDevList=NULL);
  void do_close();

};


#endif
