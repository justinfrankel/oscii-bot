// midi2osc v0.1
// Copyright (C) 2013 Cockos Incorporated
// License: GPL
#include <windows.h>
#include <ctype.h>
#include <math.h>

#include "resource.h"
#include "../WDL/eel2/ns-eel-int.h"
#include "../WDL/wdlcstring.h"
#include "../WDL/dirscan.h"
#include "../WDL/queue.h"
#include "../WDL/mutex.h"
#include "../WDL/lineparse.h"

#include "device.h"
#include "oscmsg.h"

#if defined(_MSC_VER) && defined(strcasecmp)
#undef strcasecmp
#endif

#include "../WDL/jnetlib/jnetlib.h"

HINSTANCE g_hInstance;

#define WM_SYSTRAY              WM_USER + 0x200
BOOL systray_add(HWND hwnd, UINT uID, HICON hIcon, LPSTR lpszTip);
BOOL systray_del(HWND hwnd, UINT uID);


int g_recent_events[4];
const char *g_code_names[4] = { "@init", "@timer", "@msg", "@oscmsg" };


class scriptInstance 
{
  public:
    scriptInstance(const char *fn) 
    { 
      m_fn.Set(fn);
      m_vm=0;
      memset(m_code,0,sizeof(m_code));
      clear();
    }
    ~scriptInstance() 
    {
      clear();
    }
    void reloadScript(WDL_FastString &results);
    void clear()
    {
      m_in_devs.Empty();
      m_out_devs.Empty();
      m_formats.Empty(true);
      int x;
      for (x=0;x<sizeof(m_code)/sizeof(m_code[0]); x++) 
      {
        if (m_code[x]) NSEEL_code_free(m_code[x]);
        m_code[x]=0;
      }
      if (m_vm) NSEEL_VM_free(m_vm);
      m_vm=0;
      m_incoming_events.Resize(0,false);

      m_var_time = 0;
      memset(m_var_msgs,0,sizeof(m_var_msgs));
      memset(m_var_oscfmt,0,sizeof(m_var_oscfmt));
    }


    void compileCode(int parsestate, const WDL_FastString &curblock, WDL_FastString &results, int lineoffs);
    bool run(double curtime, WDL_FastString &results);
    static void messageCallback(void *d1, void *d2, char type, int msglen, void *msg);

    WDL_String m_fn;

    struct incomingEvent
    {
      EEL_F *dev_ptr;
      int sz; // size of msg[], 1..3 for midi, anything for OSC
      char type; // 0=midi, 1=OSC
      unsigned char msg[3];
    };


    // these are non-owned refs
    WDL_PtrList<inputDevice> m_in_devs;
    WDL_PtrList<outputDevice> m_out_devs;

    WDL_HeapBuf m_incoming_events;  // incomingEvent list, each is 8-byte aligned
    WDL_Mutex m_incoming_events_mutex;

    class formatStringRec
    {
      public:
        formatStringRec() { }
        ~formatStringRec() { values.Empty(true); }

        WDL_PtrList<WDL_FastString> values; // first is mandatory, following are optional
    };

    WDL_PtrList<formatStringRec> m_formats;

    EEL_F *m_var_time, *m_var_msgs[4], *m_var_oscfmt[10];
    NSEEL_VMCTX m_vm;
    NSEEL_CODEHANDLE m_code[4]; // init, timer, message code, oscmsg code

    enum {
        FORMAT_INDEX_BASE=0x10000,
        INPUT_INDEX_BASE =0x40000,
        OUTPUT_INDEX_BASE=0x50000
    };
    static EEL_F * NSEEL_CGEN_CALL _send_oscevent(void *opaque, EEL_F *dest_device, EEL_F *fmt_index, EEL_F *value);
    static EEL_F * NSEEL_CGEN_CALL _send_midievent(void *opaque, EEL_F *dest_device);
};


WDL_PtrList<scriptInstance> g_scripts;
WDL_PtrList<inputDevice> g_inputs; // these are owned here, scriptInstances reference them
WDL_PtrList<outputDevice> g_outputs;



class oscOutputDevice : public outputDevice
{
public:
  oscOutputDevice(const char *dest, int maxpacket, int sendsleep) 
  {
    memset(&m_sendaddr, 0, sizeof(m_sendaddr));

    m_dest.Set(dest);

    WDL_String tmp(dest);
    int sendport=0;
    char *p=strstr(tmp.Get(),":");
    if (p) 
    {
      *p++=0;
      sendport=atoi(p);
    }
    if (!sendport) sendport=8000;

    m_sendaddr.sin_family=AF_INET;
    m_sendaddr.sin_addr.s_addr=inet_addr(tmp.Get());
    m_sendaddr.sin_port=htons(sendport);

    m_maxpacketsz = maxpacket> 0 ? maxpacket:1024;
    m_sendsleep = sendsleep >= 0 ? sendsleep : 10;
    m_sendsock=socket(AF_INET, SOCK_DGRAM, 0);
    if (m_sendsock >= 0)
    {
      int on=1;
      setsockopt(m_sendsock, SOL_SOCKET, SO_BROADCAST, (char*)&on, sizeof(on));
    }

  }
  virtual ~oscOutputDevice() 
  { 
    if (m_sendsock >= 0)
    {
      shutdown(m_sendsock, SHUT_RDWR);
      closesocket(m_sendsock);
      m_sendsock=-1;
    }
  } 

  virtual void run()
  {
    static char hdr[16] = { '#', 'b', 'u', 'n', 'd', 'l', 'e', 0, 0, 0, 0, 0, 1, 0, 0, 0 };

    // send m_sendq as UDP blocks
    if (m_sendq.Available()<1) return;

    if (m_sendq.Available()<=16)
    {
      m_sendq.Clear();
      return;
    }
    // m_sendq should begin with a 16 byte pad, then messages in OSC

    char* packetstart=(char*)m_sendq.Get();
    int packetlen=16;
    bool hasbundle=false;
    m_sendq.Advance(16); // skip bundle for now, but keep it around

    while (m_sendq.Available() >= sizeof(int))
    {
      int len=*(int*)m_sendq.Get(); // not advancing
      OSC_MAKEINTMEM4BE((char*)&len);

      if (len < 1 || len > MAX_OSC_MSG_LEN || len > m_sendq.Available()) break;             
        
      if (packetlen > 16 && packetlen+sizeof(int)+len > m_maxpacketsz)
      {
        // packet is full
        if (!hasbundle)
        {
          packetstart += 20;
          packetlen -= 20;
        }
        else
        {
          memcpy(packetstart,hdr,16);
        }

        sendto(m_sendsock, packetstart, packetlen, 0, (struct sockaddr*)&m_sendaddr, sizeof(struct sockaddr));
        if (m_sendsleep>0) Sleep(m_sendsleep);

        packetstart=(char*)m_sendq.Get()-16; // safe since we padded the queue start
        packetlen=16;
        hasbundle=false;
      }
     
      if (packetlen > 16) hasbundle=true;
      m_sendq.Advance(sizeof(int)+len);
      packetlen += sizeof(int)+len;
    }

    if (packetlen > 16)
    {
      if (!hasbundle)
      {
        packetstart += 20;
        packetlen -= 20;
      }
      else
      {
        memcpy(packetstart,hdr,16);
      }
      sendto(m_sendsock, packetstart, packetlen, 0, (struct sockaddr*)&m_sendaddr, sizeof(struct sockaddr));
      if (m_sendsleep>0) Sleep(m_sendsleep);
    }

    m_sendq.Clear();
  }

  virtual void oscSend(const char *src, int len)
  {
    if (!m_sendq.GetSize()) m_sendq.Add(NULL,16);

    int tlen=len;
    OSC_MAKEINTMEM4BE(&tlen);
    m_sendq.Add(&tlen,sizeof(tlen));
    m_sendq.Add(src,len);
  }
  virtual const char *get_type() { return "OSC"; }


  int m_sendsock;
  int m_maxpacketsz, m_sendsleep;
  struct sockaddr_in m_sendaddr;
  WDL_Queue m_sendq;
  WDL_String m_dest;
};



void scriptInstance::compileCode(int parsestate, const WDL_FastString &curblock, WDL_FastString &results, int lineoffs)
{
  if (parsestate<0 || parsestate >= sizeof(m_code)/sizeof(m_code[0])) return;

  if (m_code[parsestate])
  {
    results.AppendFormatted(1024,"Warning: duplicate %s sections, ignoring later copy\r\n",g_code_names[parsestate]);
    return;
  }


  WDL_FastString procOut;
  const char *rdptr = curblock.Get();
  // preprocess to get formats from { }, and replace with an index of FORMAT_INDEX_BASE+m_formats.GetSize()
  int state=0; 
  // states:
  // 1 = comment to end of line
  // 2=comment til */
  while (*rdptr)
  {
    switch (state)
    {
      case 0:
        if (*rdptr == '/')
        {
          if (rdptr[1] == '/') state=1;
          else if (rdptr[1] == '*') state=2;
        }

        if (*rdptr == '{')
        {
          // scan tokens and replace with (idx)
          formatStringRec *r = new formatStringRec;
          rdptr++; // skip '{'

          while (*rdptr && *rdptr != '}')
          {
            while (*rdptr == ' ' || *rdptr == '\t' || *rdptr == '\r' || *rdptr == '\n') rdptr++;
            if (*rdptr == '}') break;
            const char *tokstart = rdptr;
            const bool hasq = *tokstart == '\"';
            if (hasq) 
            {
              rdptr++; 
              tokstart++;
              int esc_state=0;
              WDL_FastString *val = new WDL_FastString;
              while (*rdptr)
              {
                if (esc_state==0)
                {
                  if (*rdptr == '\\') 
                  {
                    esc_state=1;
                  }
                  else if (*rdptr == '"') break;
                }
                else esc_state=0;

                if (!esc_state) val->Append(rdptr,1);
                rdptr++;
              }
              r->values.Add(val);
              if (*rdptr) rdptr++; // skip trailing quote
            }
            else
            {
              while (*rdptr && *rdptr != ' ' && *rdptr != '\r' && *rdptr != '\n' && *rdptr != '\t' && *rdptr != '}') rdptr++;
              if (rdptr > tokstart)
              {
                r->values.Add(new WDL_FastString(tokstart,rdptr-tokstart));
              }
            }
           

          }

          if (!r->values.GetSize())
          {
            results.AppendFormatted(1024,"Warning: in %s: { } specified with no strings\r\n",g_code_names[parsestate]);
          }

          procOut.AppendFormatted(128,"(%d)",FORMAT_INDEX_BASE+m_formats.GetSize());
          m_formats.Add(r);
          if (*rdptr) rdptr++;

          continue;
        }
      break;
      case 1:
        if (*rdptr == '\n') state=0;
      break;
      case 2:
        if (*rdptr == '*' && rdptr[1] == '/') 
        {
          procOut.Append(rdptr++,1);
          state=0;
        }
      break;
    }
    if (state<3) procOut.Append(rdptr,1);

    rdptr++;
  }

  m_code[parsestate]=NSEEL_code_compile_ex(m_vm,procOut.Get(),lineoffs,NSEEL_CODE_COMPILE_FLAG_COMMONFUNCS);
  if (!parsestate && m_code[0])
  {
    if (m_var_time) *m_var_time = timeGetTime()/1000.0;
    NSEEL_code_execute(m_code[0]);
  }

  char *err;
  if (!m_code[parsestate] && (err=NSEEL_code_getcodeerror(m_vm)))
  {
    results.AppendFormatted(1024,"Error: in %s: %s\r\n",g_code_names[parsestate], err);
  }
}

static int validate_format(const char *fmt)
{
  // only allow %f
  int state=0;
  int cnt=0;
  while (*fmt)
  {
    if (state==0)
    {
      if (*fmt == '%') state=1;
    }
    else
    {
      if (*fmt == 'f'||*fmt=='e' || *fmt=='E' || *fmt=='g' || *fmt=='G') 
      {
        cnt++;
        state=0; // completed!
      }
      else if (*fmt == '.') 
      {
        if (state&2) return -1;
        state |= 2;
      }
      else if (*fmt == '+') 
      {
        if (state&(32|16|8|4)) return -1;
        state |= 8;
      }
      else if (*fmt == '-') 
      {
        if (state&(32|16|8|4)) return -1;
        state |= 16;
      }
      else if (*fmt == ' ') 
      {
        if (state&(32|16|8|4)) return -1;
        state |= 32;
      }
      else if (*fmt >= '0' && *fmt <= '9') 
      {
        state|=4;
      }
      else 
      {
        return -1; // %unknown-char
      }
    }
    
    fmt++;
  }
  return state? -1 : cnt;
}

EEL_F * NSEEL_CGEN_CALL scriptInstance::_send_oscevent(void *opaque, EEL_F *dest_device, EEL_F *fmt_index, EEL_F *value)
{
  scriptInstance *_this = (scriptInstance*)opaque;
  if (_this)
  {
    int output_idx = (int) floor(*dest_device+0.5);
    outputDevice *output = _this->m_out_devs.Get(output_idx - OUTPUT_INDEX_BASE);
    if (output || output_idx == -1 || output_idx==-100)
    {
      formatStringRec *rec = _this->m_formats.Get((int) (*fmt_index + 0.5) - FORMAT_INDEX_BASE );
      if (rec && rec->values.GetSize())
      {
        const char *fmt = rec->values.Get(0)->Get();
        char fmt_type = 0;
        if (fmt[0] && fmt[0] != '/') fmt_type = *fmt++;

        int fmtcnt=validate_format(fmt);
        if (fmtcnt >= 0 && fmtcnt < 10)
        {
          char buf[1024];
  #define FOO(x) (_this->m_var_oscfmt[x] ? _this->m_var_oscfmt[x][0]:0.0)
          WDL_snprintf(buf,sizeof(buf),fmt, FOO(0),FOO(1),FOO(2),FOO(3),FOO(4),FOO(5),FOO(6),FOO(7),FOO(8),FOO(9));
  #undef FOO

          OscMessageWrite wr;
          wr.PushWord(buf);
        
          if (fmt_type == 'b') wr.PushIntArg(!!(int) (*value));
          else if (fmt_type =='i') wr.PushIntArg((int) (*value));
          else if (fmt_type == 's')
          {
            int idx=(int) (*value);
            if (idx>=0 && idx < rec->values.GetSize()-1)
            {
              wr.PushStringArg(rec->values.Get(idx+1)->Get());
            }
            else
            {
              char tmp[64];
              sprintf(tmp,"%.2f",*value);
              wr.PushStringArg(tmp);
            }         
          }
          else if (fmt_type == 't')
          {
            // no parameter, just toggle
          }
          else 
          {
            // default to float
            wr.PushFloatArg((float)*value);
          }

          int l=0;
          const char *ret=wr.GetBuffer(&l);
          if (ret && l>0) 
          {
            if (output)
            {
              output->oscSend(ret,l);
            }
            else if (output_idx==-100)
            {
              int n;
              for (n=0;n<g_outputs.GetSize();n++)
                g_outputs.Get(n)->oscSend(ret,l);
            }
            else 
            {
              int n;
              for (n=0;n<_this->m_out_devs.GetSize();n++)
                _this->m_out_devs.Get(n)->oscSend(ret,l);
            }
          }
        }
      }
    }
  }
  return value;
}


EEL_F * NSEEL_CGEN_CALL scriptInstance::_send_midievent(void *opaque, EEL_F *dest_device)
{
  scriptInstance *_this = (scriptInstance*)opaque;
  if (_this)
  {
    int output_idx = (int) floor(*dest_device+0.5);
    outputDevice *output = _this->m_out_devs.Get(output_idx - OUTPUT_INDEX_BASE);
    if (output || output_idx == -1 || output_idx==-100)
    {
      unsigned char msg[3];
      EEL_F **vms = _this->m_var_msgs;
      msg[0] = vms[0] ? (int) (vms[0][0]+0.5) : 0;
      msg[1] = vms[1] ? (int) (vms[1][0]+0.5) : 0;
      msg[2] = vms[2] ? (int) (vms[2][0]+0.5) : 0;

      if (output)
      {
        output->midiSend(msg,3);
      }
      else if (output_idx==-100)
      {
        int n;
        for (n=0;n<g_outputs.GetSize();n++)
          g_outputs.Get(n)->midiSend(msg,3);
      }
      else 
      {
        int n;
        for (n=0;n<_this->m_out_devs.GetSize();n++)
          _this->m_out_devs.Get(n)->midiSend(msg,3);
      }
    }
  }
  return dest_device;
}


void scriptInstance::reloadScript(WDL_FastString &results)
{
  clear();

  m_vm = NSEEL_VM_alloc();
  NSEEL_VM_SetCustomFuncThis(m_vm,this);

  m_var_time = NSEEL_VM_regvar(m_vm,"time");
  m_var_msgs[0] = NSEEL_VM_regvar(m_vm,"msg1");
  m_var_msgs[1] = NSEEL_VM_regvar(m_vm,"msg2");
  m_var_msgs[2] = NSEEL_VM_regvar(m_vm,"msg3");
  m_var_msgs[3] = NSEEL_VM_regvar(m_vm,"msgdev");

  int x;
  for (x=0;x<sizeof(m_var_oscfmt)/sizeof(m_var_oscfmt[0]);x++)
  {
    char tmp[32];
    sprintf(tmp,"oscfmt%d",x);
    m_var_oscfmt[x] = NSEEL_VM_regvar(m_vm,tmp);
  }

  results.Append("Loading: ");
  results.Append(m_fn.Get());
  results.Append("\r\n");

  FILE *fp = fopen(m_fn.Get(),"r");
  if (!fp)
  {
    results.Append("Error: failed opening: ");
    results.Append(m_fn.Get());
    results.Append("\r\n");
    return;
  }

  bool comment_state=false;
  int parsestate=-1,cursec_lineoffs=0,lineoffs=0;
  WDL_FastString curblock;
  for (;;)
  {
    char linebuf[8192];
    linebuf[0]=0;
    fgets(linebuf,sizeof(linebuf),fp);
    lineoffs++;

    if (!linebuf[0]) break;

    {
      char *p=linebuf;
      while (*p) p++;
      p--;
      while (p >= linebuf && (*p == '\r' || *p == '\n')) { *p=0; p--; }
    }

    LineParser lp(comment_state);
    if (linebuf[0] && !lp.parse(linebuf) && lp.getnumtokens()> 0 && lp.gettoken_str(0)[0] == '@')
    {
      const char *tok=lp.gettoken_str(0);
      int x;
      for (x=0;x<sizeof(g_code_names)/sizeof(g_code_names[0]) && strcmp(tok,g_code_names[x]);x++);

      if (x < sizeof(g_code_names)/sizeof(g_code_names[0]))
      {
        compileCode(parsestate,curblock,results,cursec_lineoffs);
        parsestate=x;
        cursec_lineoffs=lineoffs;
        curblock.Set("");
      }
      else if (!strcmp(tok,"@input"))
      {
        if (lp.getnumtokens()<3 || !lp.gettoken_str(1)[0])
        {
          results.Append("Usage: @input devicehandle \"substring devicename match\" [skip_count]\r\n");
        }
        else
        {
          if (NSEEL_VM_get_var_refcnt(m_vm,lp.gettoken_str(1))>=0)
          {
            results.AppendFormatted(1024,"Warning: device name '%s' already in use, skipping @input line\r\n",lp.gettoken_str(1));
          }
          else
          {
            const char *dp = lp.gettoken_str(2);
            if (!strnicmp(dp,"OSC:",4))
            {
              dp += 4;
              while (*dp == ' ') dp++;
              results.AppendFormatted(1024,"Warning: todo: @input MIDI: support");

            }
            else
            {
              if (!strnicmp(dp,"MIDI:",5))
              {
                dp += 5;
                while (*dp == ' ') dp++;
              }
              const char *substr = dp;
              int skipcnt = lp.getnumtokens()>=3 ? lp.gettoken_int(3) : 0;

              midiInputDevice *rec = new midiInputDevice(substr,skipcnt, &g_inputs);
              bool is_reuse=false;
              if (!rec->m_handle)
              {
                if (rec->m_open_would_use_altdev)
                {
                  midiInputDevice *tmp=rec->m_open_would_use_altdev;
                  delete rec;
                  rec=tmp;
                  is_reuse=true;

                  if (!rec->m_handle)
                    results.AppendFormatted(1024,"Warning: attached to un-opened device '%s'\r\n",rec->m_name_used);
                  else
                    results.AppendFormatted(1024,"Attached to already-opened device '%s'\r\n",rec->m_name_used);
                }
                else if (rec->m_name_used)
                  results.AppendFormatted(1024,"Warning: tried to open device '%s' but failed\r\n",rec->m_name_used);
                else
                  results.AppendFormatted(1024,"Warning: tried to open device matching '%s'(%d) but failed, will retry\r\n",substr,skipcnt);
              }
              EEL_F *dev_idx = NSEEL_VM_regvar(m_vm,lp.gettoken_str(1));
              if (dev_idx) dev_idx[0] = m_in_devs.GetSize() + INPUT_INDEX_BASE;
              rec->addinst(messageCallback,this,dev_idx);
              m_in_devs.Add(rec);

              if (!is_reuse) g_inputs.Add(rec);
            }
          }
        }
      }
      else if (!strcmp(tok,"@output"))
      {
        if (lp.getnumtokens()<3 || !lp.gettoken_str(1)[0])
        {
          results.Append("Usage: @output devicehandle \"host:port\" [maxpacketsize (def=1024)] [sleepinMS (def=10)]\r\n");
        }
        else
        {
          if (NSEEL_VM_get_var_refcnt(m_vm,lp.gettoken_str(1))>=0)
          {
            results.AppendFormatted(1024,"Warning: device name '%s' already in use, skipping @output line\r\n",lp.gettoken_str(1));
          }
          else
          {
            const char *dp=lp.gettoken_str(2);
            if (!strnicmp(dp,"MIDI:",5))
            {
              dp += 5;
              while (*dp == ' ') dp++;
              const char *substr = dp;
              int skipcnt = lp.getnumtokens()>=3 ? lp.gettoken_int(3) : 0;

              bool is_reuse=false;
              midiOutputDevice *rec = new midiOutputDevice(substr,skipcnt, &g_outputs);
              if (!rec->m_handle)
              {
                if (rec->m_open_would_use_altdev)
                {
                  midiOutputDevice *tmp=rec->m_open_would_use_altdev;
                  delete rec;
                  rec=tmp;
                  is_reuse=true;

                  if (!rec->m_handle)
                    results.AppendFormatted(1024,"Warning: attached to un-opened device '%s'\r\n",rec->m_name_used);
                  else
                    results.AppendFormatted(1024,"Attached to already-opened device '%s'\r\n",rec->m_name_used);
                }
                else if (rec->m_name_used)
                  results.AppendFormatted(1024,"Warning: tried to open device '%s' but failed\r\n",rec->m_name_used);
                else
                  results.AppendFormatted(1024,"Warning: tried to open device matching '%s'(%d) but failed, will retry\r\n",substr,skipcnt);
              }

              EEL_F *var=NSEEL_VM_regvar(m_vm,lp.gettoken_str(1));
              if (var) *var=m_out_devs.GetSize() + OUTPUT_INDEX_BASE;
              m_out_devs.Add(rec);

              if (!is_reuse) g_outputs.Add(rec);


            }
            else
            {
              if (!strnicmp(dp,"OSC:",4)) 
              {
                dp+=4;
                while (*dp == ' ') dp++;
              }
              oscOutputDevice *r = NULL;
              int x;
              bool is_reuse=false;
              for (x=0;x<g_outputs.GetSize();x++)
              {
                outputDevice *d = g_outputs.Get(x);
                if (d && !strcmp(d->get_type(),"OSC"))
                {
                  oscOutputDevice *p = (oscOutputDevice *)d;
                  if (!strcmp(p->m_dest.Get(),dp))
                  {
                    is_reuse=true;
                    r=p; // reuse!
                    break;
                  }
                }
              }

              if (!r) 
              {
                is_reuse=false;
                r = new oscOutputDevice(dp, lp.getnumtokens()>=4 ? lp.gettoken_int(3) : 0,
                                            lp.getnumtokens()>=5 ? lp.gettoken_int(4) : -1);
                if (r->m_sendsock<0)
                {
                  results.AppendFormatted(1024,"Warning: failed creating destination for @output '%s' -> '%s'\r\n",lp.gettoken_str(1),lp.gettoken_str(2));
                  delete r;
                  r=NULL;
                }
              }

              if (r)
              {
                EEL_F *var=NSEEL_VM_regvar(m_vm,lp.gettoken_str(1));
                if (var) *var=m_out_devs.GetSize() + OUTPUT_INDEX_BASE;
                m_out_devs.Add(r);

                if (!is_reuse) g_outputs.Add(r);
              }
            }
          }
        }
      }
      else
      {
        results.AppendFormatted(1024,"Warning: Unknown directive: %s\r\n",tok);
      }
    }
    else
    {
      const char *p=linebuf;
      if (parsestate==-3)
      {
        while (*p)
        {
          if (p[0] == '*' && p[1]== '/')
          {
            parsestate=-1; // end of comment!
            p+=2;
            break;
          }
        }
      }
      if (parsestate==-1 && p[0])
      {
        while (*p == ' ' || *p =='\t') p++;
        if (!*p || (p[0] == '/' && p[1] == '/'))
        {
        }
        else
        {
          if (*p == '/' && p[1] == '*')
          {
            parsestate=-3;
          }
          else
          {
            results.AppendFormatted(1024,"Warning: line '%.100s' (and possibly more)' are not in valid section and may be ignored\r\n", linebuf);
            parsestate=-2;
          }
        }
      }
      if (parsestate>=0)
      {
        curblock.Append(linebuf);
        curblock.Append("\n");
      }
    }
  }
  compileCode(parsestate,curblock,results,cursec_lineoffs);


  fclose(fp);

  results.Append("Loaded: ");
  results.Append(m_fn.Get());
  results.Append("\r\n");

  if (!m_in_devs.GetSize()) results.Append("Warning: No @input opened\r\n");
  if (!m_out_devs.GetSize()) results.Append("Warning: No @output opened\r\n");

  results.AppendFormatted(512,"\r\n%d inputs, %d outputs, and %d formats opened\r\n",
      m_in_devs.GetSize(),m_out_devs.GetSize(),m_formats.GetSize());

}

void scriptInstance::messageCallback(void *d1, void *d2, char type, int len, void *msg)
{
  scriptInstance *_this  = (scriptInstance *)d1;
  if (_this && msg)
  {
    // MIDI
    if (_this->m_incoming_events.GetSize() < 65536)
    {
      const int this_sz = ((sizeof(incomingEvent) + (len-3)) + 7) & ~7;

      _this->m_incoming_events_mutex.Enter();
      const int oldsz = _this->m_incoming_events.GetSize();
      _this->m_incoming_events.Resize(oldsz + this_sz,false);
      if (_this->m_incoming_events.GetSize() == oldsz+this_sz)
      {
        incomingEvent *item=(incomingEvent *) ((char *)_this->m_incoming_events.Get() + oldsz);
        item->dev_ptr = (EEL_F*)d2;
        item->sz = len;
        item->type = type;
        memcpy(item->msg,msg,len);
      }
      _this->m_incoming_events_mutex.Leave();
    }
  }
}

bool scriptInstance::run(double curtime, WDL_FastString &results)
{
  bool rv=false;
  if (m_var_time) *m_var_time = curtime;


  if (m_incoming_events.GetSize())
  {
    static WDL_HeapBuf tmp;

    m_incoming_events_mutex.Enter();
    tmp.CopyFrom(&m_incoming_events,false);
    m_incoming_events.Resize(0,false);
    m_incoming_events_mutex.Leave();

    int pos=0;
    const int endpos = tmp.GetSize();
    while (pos < endpos+1 - sizeof(incomingEvent))
    {
      const incomingEvent *evt = (incomingEvent*) ((char *)tmp.Get()+pos);
      
      const int this_sz = ((sizeof(incomingEvent) + (evt->sz-3)) + 7) & ~7;

      if (pos+this_sz > endpos) break;
      pos += this_sz;

      switch (evt->type)
      {
        case 0:
          if (evt->sz == 3)
          {
            int asInt = (evt->msg[0] << 16) | (evt->msg[1] << 8) | evt->msg[2];
            if (g_recent_events[0] != asInt)
            {
              memmove(g_recent_events+1,g_recent_events,sizeof(g_recent_events)-sizeof(g_recent_events[0]));
              g_recent_events[0]=asInt;
              rv=true;
            }

            if (m_var_msgs[0]) m_var_msgs[0][0] = evt->msg[0];
            if (m_var_msgs[1]) m_var_msgs[1][0] = evt->msg[1];
            if (m_var_msgs[2]) m_var_msgs[2][0] = evt->msg[2];
            if (m_var_msgs[3]) m_var_msgs[3][0] = evt->dev_ptr ? *evt->dev_ptr : -1.0;
            NSEEL_code_execute(m_code[2]);
          }
        break;
      }
    }
  }
  if (m_vm && m_code[1]) NSEEL_code_execute(m_code[1]); // timer follows messages
  return rv;
}



void NSEEL_HOSTSTUB_EnterMutex() { }
void NSEEL_HOSTSTUB_LeaveMutex() { }



void load_all_scripts(WDL_FastString &results)
{
  g_inputs.Empty(true);
  g_outputs.Empty(true);

  int x;
  for (x=0;x<g_scripts.GetSize(); x++) g_scripts.Get(x)->reloadScript(results);

  results.AppendFormatted(512,"\r\nTotal: %d scripts, %d inputs, %d outputs\r\n", g_scripts.GetSize(), g_inputs.GetSize(),g_outputs.GetSize());

  for (x=0;x<g_inputs.GetSize();x++)
  {
    inputDevice *rec=g_inputs.Get(x);
    if (rec) rec->start();
  }
}

WDL_DLGRET mainProc(HWND hwndDlg, UINT uMsg, WPARAM wParam, LPARAM lParam)
{
  static WDL_FastString results;
  switch (uMsg)
  {
    case WM_INITDIALOG:
      // lParam = config file
      {
        HICON icon=LoadIcon(g_hInstance,MAKEINTRESOURCE(IDI_ICON1));
        SetClassLong(hwndDlg,GCL_HICON,(LPARAM)icon);
        systray_add(hwndDlg, 0, (HICON)icon, "Cockos midi2osc");

        SendMessage(hwndDlg,WM_COMMAND,IDC_BUTTON1,0);
        SetTimer(hwndDlg,1,10,NULL);
      }
    return 1;
    case WM_CLOSE:
      ShowWindow(hwndDlg,SW_HIDE);
    return 1;
    case WM_TIMER:
      if (wParam == 1)
      {
        // periodically update IDC_LASTMSG with new message(s?), if changed
        int x;
        int asz=results.GetLength();
        for (x=0;x<g_inputs.GetSize();x++) 
        {
          g_inputs.Get(x)->run(results);
        }
        if (results.GetLength() != asz)
        {
          SetDlgItemText(hwndDlg,IDC_EDIT1,results.Get());
        }

        bool needUIupdate=false;

        double curtime = timeGetTime()/1000.0;
        for (x=0;x<g_scripts.GetSize();x++)
        {
          if (g_scripts.Get(x)->run(curtime,results)) needUIupdate=true;
        }

        for (x=0;x<g_outputs.GetSize();x++) g_outputs.Get(x)->run();  // send queued messages

        if (needUIupdate)
        {
          static WDL_FastString s;
          s.Set("");
          int x;
          for(x=0;x<sizeof(g_recent_events)/sizeof(g_recent_events[0]);x++)
          {
            int a = g_recent_events[x];
            if (!a) break;
            s.AppendFormatted(64,"%02x:%02x:%02x ",(a>>16)&0xff,(a>>8)&0xff,a&0xff);
          }
          SetDlgItemText(hwndDlg,IDC_LASTMSG,s.Get());
        }
      }
    return 0;
    case WM_COMMAND:
      switch (LOWORD(wParam))
      {
        case IDCANCEL:
          systray_del(hwndDlg,0);
          EndDialog(hwndDlg,1);
        break;
        case IDC_BUTTON1:
          {
            results.Set("");
            load_all_scripts(results);
            SetDlgItemText(hwndDlg,IDC_EDIT1,results.Get());
          }
        break;
      }
    return 0;
    case WM_SYSTRAY:
      switch (LOWORD(lParam))
      {
        case WM_LBUTTONDBLCLK:
          ShowWindow(hwndDlg,SW_SHOW);
          SetForegroundWindow(hwndDlg);
        return 0;
      }
    return 0;
  }
  return 0;
}

void load_scripts_for_path(const char *path)
{
  WDL_DirScan ds;
  WDL_String s;
  if (!ds.First(path))
  {
    do
    {
      const char *fn = ds.GetCurrentFN();
      if (fn[0] != '.' && strlen(fn)>4 && !stricmp(fn+strlen(fn)-4,".cfg"))
      {
        ds.GetCurrentFullFN(&s);
        g_scripts.Add(new scriptInstance(s.Get()));
      }
    }
    while (!ds.Next());
  }

}
int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR lpCmdLine, int nShowCmd)
{
  g_hInstance = hInstance;

  char exepath[2048];
  exepath[0]=0;
  GetModuleFileName(NULL,exepath,sizeof(exepath));
  char *p=exepath;
  while (*p) p++;
  while (p >= exepath && *p != '\\') p--; *++p=0;

  {
    int state=0;
    char *p=lpCmdLine;
    while (*p)
    {
      char parm[2048];
      int parm_pos=0,qs=0;

      while (isspace(*p)) p++;
      if (!*p) break;

      while (*p && (!isspace(*p) || qs))
      {
        if (*p == '\"') qs=!qs;
        else if (parm_pos < (int)sizeof(parm)-1) parm[parm_pos++]=*p;       
        p++;
      }
      parm[parm_pos]=0;

      if (!state) if (parm[0] == '/') parm[0]='-';

      switch (state)
      {
        case 0:
          if (parm[0] == '-')
          {
            if (!strcmp(parm,"-dir"))
            {
              state=1;
            }
            else
            {
              state=-1;
            }
          }
          else
          {
            if (strstr(parm,"/") || strstr(parm,"\\"))
            {
              g_scripts.Add(new scriptInstance(parm));
            }
            else
            {
              WDL_FastString s(exepath);
              s.Append(parm);
              g_scripts.Add(new scriptInstance(s.Get()));
            }
          }
        break;
        case 1:
          if (strstr(parm,"/") || strstr(parm,"\\"))
          {
            load_scripts_for_path(parm);
          }
          else
          {
            WDL_FastString s(exepath);
            s.Append(parm);
            load_scripts_for_path(s.Get());
          }
          state=0;
          // dir
        break;

      }

      if (state < 0) break;
    }
    if (state)
    {
      MessageBox(NULL,
        "Usage:\r\n"
        "midi2osc [filename.cfg ...] [-dir pathwithcfgfiles]\r\n"
        "if no config files specified, default will be all cfg files in program directory","Usage",MB_OK);
      return 0;
    }
  }

  if (!g_scripts.GetSize()) load_scripts_for_path(exepath);


  JNL::open_socketlib();

  NSEEL_init();
  NSEEL_addfunctionex("oscsend",3,(char *)_asm_generic3parm,(char *)_asm_generic3parm_end-(char *)_asm_generic3parm,NSEEL_PProc_THIS,(void *)&scriptInstance::_send_oscevent);
  NSEEL_addfunctionex("midisend",1,(char *)_asm_generic1parm,(char *)_asm_generic1parm_end-(char *)_asm_generic1parm,NSEEL_PProc_THIS,(void *)&scriptInstance::_send_midievent);
  // todo: midisend(), oscmatch(), oscpop()

  DialogBox(hInstance,MAKEINTRESOURCE(IDD_DIALOG1),GetDesktopWindow(),mainProc);

  g_inputs.Empty(true);
  g_outputs.Empty(true);
  g_scripts.Empty(true);


  ExitProcess(0);
  return 0;
}


BOOL systray_add(HWND hwnd, UINT uID, HICON hIcon, LPSTR lpszTip)
{
  NOTIFYICONDATA tnid;
  tnid.cbSize = sizeof(NOTIFYICONDATA);
  tnid.hWnd = hwnd;
  tnid.uID = uID;
  tnid.uFlags = NIF_ICON | NIF_TIP | NIF_MESSAGE;
  tnid.uCallbackMessage = WM_SYSTRAY;
  tnid.hIcon = hIcon;
  lstrcpyn(tnid.szTip,lpszTip,sizeof(tnid.szTip)-1);
  return (Shell_NotifyIcon(NIM_ADD, &tnid));
}

BOOL systray_del(HWND hwnd, UINT uID) {
  NOTIFYICONDATA tnid;
  tnid.cbSize = sizeof(NOTIFYICONDATA);
  tnid.hWnd = hwnd;
  tnid.uID = uID;
  return(Shell_NotifyIcon(NIM_DELETE, &tnid));
}
