#include <windows.h>
#include <ctype.h>
#include <math.h>

#include "resource.h"
#include "../WDL/eel2/ns-eel-int.h"
#include "../WDL/wdlcstring.h"
#include "../WDL/wdlstring.h"
#include "../WDL/ptrlist.h"
#include "../WDL/queue.h"
#include "../WDL/mutex.h"
#include "../WDL/lineparse.h"

#if defined(_MSC_VER) && defined(strcasecmp)
#undef strcasecmp
#endif

#include "../WDL/jnetlib/jnetlib.h"

HINSTANCE g_hInstance;

static void BSWAPINTMEM(void *buf)
{
  char *p=(char *)buf;
  char tmp=p[0]; p[0]=p[3]; p[3]=tmp;
  tmp=p[1]; p[1]=p[2]; p[2]=tmp;
}

#ifdef __ppc__
#define MAKEINTMEM4BE(x)
#else
#define MAKEINTMEM4BE(x) BSWAPINTMEM(x)
#endif

#define MAX_OSC_MSG_LEN 1024


#define WM_SYSTRAY              WM_USER + 0x200
BOOL systray_add(HWND hwnd, UINT uID, HICON hIcon, LPSTR lpszTip);
BOOL systray_del(HWND hwnd, UINT uID);

struct incomingEvent 
{
  EEL_F *dev_ptr;
  unsigned char msg[3];
};

int g_recent_events[4];

WDL_TypedQueue<incomingEvent> g_incoming_events; // format: 
WDL_Mutex g_incoming_events_mutex;

struct formatStringRec
{
  formatStringRec() { }
  ~formatStringRec() { values.Empty(true); }

  WDL_PtrList<WDL_FastString> values; // first is mandatory, following are optional
};


class OscMessageWrite
{
public:

  OscMessageWrite()
  {
    m_msg[0]=0;
    m_types[0]=0;
    m_args[0]=0;

    m_msg_ptr=m_msg;
    m_type_ptr=m_types;
    m_arg_ptr=m_args;
  }

  bool PushWord(const char* word)
  {
    int len=strlen(word);
    if (m_msg_ptr+len+1 >= m_msg+sizeof(m_msg)) return false;

    strcpy(m_msg_ptr, word);
    m_msg_ptr += len;
    return true;
  }

  bool PushIntArg(int val)
  {
    if (m_type_ptr+1 > m_types+sizeof(m_types)) return false;
    if (m_arg_ptr+sizeof(int) > m_args+sizeof(m_args)) return false;

    *m_type_ptr++='i'; 
    *m_type_ptr=0;

    *(int*)m_arg_ptr=val;
    MAKEINTMEM4BE(m_arg_ptr);
    m_arg_ptr += sizeof(int);
  
    return true;
  }

  bool PushFloatArg(float val)
  {
    if (m_type_ptr+1 > m_types+sizeof(m_types)) return false;
    if (m_arg_ptr+sizeof(float) > m_args+sizeof(m_args)) return false;

    *m_type_ptr++='f';
    *m_type_ptr=0;

    *(float*)m_arg_ptr=val;
    MAKEINTMEM4BE(m_arg_ptr);
    m_arg_ptr += sizeof(float);
  
    return true;
  }

  bool PushStringArg(const char* val)
  {
    int len=strlen(val);
    int padlen=pad4(len);

    if (m_type_ptr+1 > m_types+sizeof(m_types)) return false;
    if (m_arg_ptr+padlen > m_args+sizeof(m_args)) return false;

    *m_type_ptr++='s';
    *m_type_ptr=0;

    strcpy(m_arg_ptr, val);
    memset(m_arg_ptr+len, 0, padlen-len);
    m_arg_ptr += padlen;

    return true;
  }
  const char* GetBuffer(int* len)
  {
    int msglen=m_msg_ptr-m_msg;
    int msgpadlen=pad4(msglen);

    int typelen=m_type_ptr-m_types+1; // add the comma
    int typepadlen=pad4(typelen);

    int arglen=m_arg_ptr-m_args; // already padded

    if (msgpadlen+typepadlen+arglen > sizeof(m_msg)) 
    {
      if (len) *len=0;
      return "";
    }

    char* p=m_msg;
    memset(p+msglen, 0, msgpadlen-msglen);
    p += msgpadlen;
  
    *p=',';
    strcpy(p+1, m_types);
    memset(p+typelen, 0, typepadlen-typelen);
    p += typepadlen;

    memcpy(p, m_args, arglen);
    p += arglen;

    if (len) *len=p-m_msg;
    return m_msg;
  }
  
private:
  
  static int pad4(int len)
  {
    return (len+4)&~3;
  }

  char m_msg[MAX_OSC_MSG_LEN];
  char m_types[MAX_OSC_MSG_LEN];
  char m_args[MAX_OSC_MSG_LEN];

  char* m_msg_ptr;
  char* m_type_ptr;
  char* m_arg_ptr;
};


struct oscOutputRec
{
  oscOutputRec(const char *dest, int maxpacket, int sendsleep) 
  { 
    memset(&m_sendaddr, 0, sizeof(m_sendaddr));

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
  ~oscOutputRec() 
  { 
    if (m_sendsock >= 0)
    {
      shutdown(m_sendsock, SHUT_RDWR);
      closesocket(m_sendsock);
      m_sendsock=-1;
    }
  }

  void run()
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
      MAKEINTMEM4BE((char*)&len);

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
  void doSend(const char *src, int len)
  {
    if (!m_sendq.GetSize()) m_sendq.Add(NULL,16);

    int tlen=len;
    MAKEINTMEM4BE(&tlen);
    m_sendq.Add(&tlen,sizeof(tlen));
    m_sendq.Add(src,len);
  }

  int m_sendsock;
  int m_maxpacketsz, m_sendsleep;
  struct sockaddr_in m_sendaddr;
  WDL_Queue m_sendq;
};



struct midiInputRec
{
  midiInputRec(int inputidx) 
  { 
    // found device!
    dev_idx=0;
    m_handle=0;
    if (midiInOpen(&m_handle,inputidx,(LPARAM)callbackFunc,(LPARAM)this,CALLBACK_FUNCTION) != MMSYSERR_NOERROR )
    {
      m_handle=0;
    }
    else
    {
      int x;
      for (x = 0; x < 8; x ++)
      {
        const int bufsz=1024;
        MIDIHDR *hdr=(MIDIHDR *)malloc(sizeof(MIDIHDR)+bufsz);
        memset(hdr,0,sizeof(MIDIHDR));
        hdr->dwBufferLength = bufsz;
        hdr->lpData = (char *)(hdr+1);
        m_longmsgs.Add(hdr);
      }
    }
  }
  ~midiInputRec() 
  { 
    if (m_handle) 
    {
      midiInReset(m_handle);
      int x;
      int n=500;
      Sleep(100);
      for (x = 0; x < m_longmsgs.GetSize(); x ++)
      {
        MIDIHDR *hdr=m_longmsgs.Get(x);
        while (!(hdr->dwFlags & MHDR_DONE) && (hdr->dwFlags & MHDR_INQUEUE) && n-->0)
          Sleep(10);
      }
      for (x = 0; x < m_longmsgs.GetSize(); x ++)
      {
        MIDIHDR *hdr=m_longmsgs.Get(x);
        midiInUnprepareHeader(m_handle,hdr,sizeof(MIDIHDR));
      }
      m_longmsgs.Empty(true,free);

      midiInClose(m_handle); 
    }
  }
  void start() 
  { 
    if (m_handle) 
    {
      midiInStop(m_handle);
      int x;
      for (x = 0; x < m_longmsgs.GetSize(); x ++)
      {
        if (!(m_longmsgs.Get(x)->dwFlags & MHDR_INQUEUE) || (m_longmsgs.Get(x)->dwFlags & MHDR_DONE))
        {
          midiInUnprepareHeader(m_handle,m_longmsgs.Get(x),sizeof(MIDIHDR));
          m_longmsgs.Get(x)->dwFlags=0;
          m_longmsgs.Get(x)->dwBytesRecorded=0;
          midiInPrepareHeader(m_handle,m_longmsgs.Get(x),sizeof(MIDIHDR));
          midiInAddBuffer(m_handle,m_longmsgs.Get(x),sizeof(MIDIHDR));
        }
      }

      midiInStart(m_handle); 
    }
  }
  void run()
  {
    int x;
    if (m_handle) for (x = 0; x < m_longmsgs.GetSize(); x ++)
    {
      MIDIHDR *hdr=m_longmsgs.Get(x);
      if (hdr->dwFlags & MHDR_DONE)
      {
        midiInUnprepareHeader(m_handle,m_longmsgs.Get(x),sizeof(MIDIHDR));
        m_longmsgs.Get(x)->dwFlags=0;
        m_longmsgs.Get(x)->dwBytesRecorded=0;
      }
      if (!(hdr->dwFlags & MHDR_PREPARED))
      {
        midiInPrepareHeader(m_handle,m_longmsgs.Get(x),sizeof(MIDIHDR));
      }
      if (!(hdr->dwFlags & MHDR_INQUEUE))
      {
        midiInAddBuffer(m_handle,m_longmsgs.Get(x),sizeof(MIDIHDR));
      }
    }

  }

  HMIDIIN m_handle;
  EEL_F *dev_idx;
  WDL_PtrList<MIDIHDR> m_longmsgs;


  static void CALLBACK callbackFunc(
    HMIDIIN hMidiIn,  
    UINT wMsg,        
    LPARAM dwInstance, 
    LPARAM dwParam1,   
    LPARAM dwParam2    
  )
  {
    midiInputRec *_this = (midiInputRec*)dwInstance;
    if (wMsg == MIM_DATA )
    {

      if (g_incoming_events.Available() < 2048)
      {
        incomingEvent item;
        item.dev_ptr = _this ? _this->dev_idx : NULL;
        item.msg[0]=(unsigned char)(dwParam1&0xff);
        item.msg[1]=(unsigned char)((dwParam1>>8)&0xff);
        item.msg[2]=(unsigned char)((dwParam1>>16)&0xff);
        g_incoming_events_mutex.Enter();
        g_incoming_events.Add(&item,1);
        g_incoming_events_mutex.Leave();
      }
    }
  }
};


WDL_PtrList<formatStringRec> g_formats;
WDL_PtrList<midiInputRec> g_inputs;
WDL_PtrList<oscOutputRec> g_outputs;

#define FORMAT_INDEX_BASE 0x10000
#define INPUT_INDEX_BASE  0x40000
#define OUTPUT_INDEX_BASE 0x50000

EEL_F *g_var_time, *g_var_msgs[4], *g_var_oscfmt[10];
NSEEL_VMCTX g_vm;
const char *g_code_names[3] = { "@init", "@timer", "@msg" };
NSEEL_CODEHANDLE g_code[3]; // init, timer, message code

void compileCode(int parsestate, const WDL_FastString &curblock, WDL_FastString &results, int lineoffs)
{
  if (parsestate<0 || parsestate >= sizeof(g_code)/sizeof(g_code[0])) return;

  if (g_code[parsestate])
  {
    results.AppendFormatted(1024,"Warning: duplicate %s sections, ignoring later copy\r\n",g_code_names[parsestate]);
    return;
  }


  WDL_FastString procOut;
  const char *rdptr = curblock.Get();
  // preprocess to get formats from { }, and replace with an index of FORMAT_INDEX_BASE+g_formats.GetSize()
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

          procOut.AppendFormatted(128,"(%d)",FORMAT_INDEX_BASE+g_formats.GetSize());
          g_formats.Add(r);
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

  g_code[parsestate]=NSEEL_code_compile(g_vm,procOut.Get(),lineoffs);
  if (!parsestate && g_code[0])
  {
    if (g_var_time) *g_var_time = timeGetTime()/1000.0;
    NSEEL_code_execute(g_code[0]);
  }

  char *err;
  if (!g_code[parsestate] && (err=NSEEL_code_getcodeerror(g_vm)))
  {
    results.AppendFormatted(1024,"Error: in %s: %s\r\n",g_code_names[parsestate], err);
  }

}

// outputs


void NSEEL_HOSTSTUB_EnterMutex() { }
void NSEEL_HOSTSTUB_LeaveMutex() { }

WDL_FastString cfgfile;

void reloadScript(WDL_FastString &results)
{
  g_inputs.Empty(true);
  g_formats.Empty(true);
  g_outputs.Empty(true);

  // no other threads should be going now
  g_incoming_events.Clear();

  int x;
  for (x=0;x<sizeof(g_code)/sizeof(g_code[0]); x++) 
  {
    if (g_code[x]) NSEEL_code_free(g_code[x]);
    g_code[x]=0;
  }

  if (g_vm) NSEEL_VM_free(g_vm);
  g_vm = NSEEL_VM_alloc();
  g_var_time = NSEEL_VM_regvar(g_vm,"time");
  g_var_msgs[0] = NSEEL_VM_regvar(g_vm,"msg1");
  g_var_msgs[1] = NSEEL_VM_regvar(g_vm,"msg2");
  g_var_msgs[2] = NSEEL_VM_regvar(g_vm,"msg3");
  g_var_msgs[3] = NSEEL_VM_regvar(g_vm,"msgdev");
  for (x=0;x<sizeof(g_var_oscfmt)/sizeof(g_var_oscfmt[0]);x++)
  {
    char tmp[32];
    sprintf(tmp,"oscfmt%d",x);
    g_var_oscfmt[x] = NSEEL_VM_regvar(g_vm,tmp);
  }

  // todo: register some vars: time, msg1, msg2, msg3, msgdev, oscfmt0-10

  FILE *fp = fopen(cfgfile.Get(),"r");
  if (!fp)
  {
    results.Append("Error: failed opening: ");
    results.Append(cfgfile.Get());
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
          if (NSEEL_VM_get_var_refcnt(g_vm,lp.gettoken_str(1))>=0)
          {
            results.AppendFormatted(1024,"Warning: device name '%s' already in use, skipping @input line\r\n",lp.gettoken_str(1));
          }
          else
          {
            const char *substr = lp.gettoken_str(2);
            int skipcnt = lp.getnumtokens()>=3 ? lp.gettoken_int(3) : 0;

            const int n=midiInGetNumDevs();
            for(x=0;x<n;x++)
            {
              MIDIINCAPS caps;
              if (midiInGetDevCaps(x,&caps,sizeof(caps)) == MMSYSERR_NOERROR)
              {
                if ((!substr[0] || strstr(caps.szPname,substr)) && !skipcnt--)
                {
                  midiInputRec *rec = new midiInputRec(x);
                  if (!rec->m_handle)
                  {
                    results.AppendFormatted(1024,"Warning: tried to open device '%s' but failed\r\n",caps.szPname);
                    delete rec;
                  }
                  else
                  {
                    rec->dev_idx = NSEEL_VM_regvar(g_vm,lp.gettoken_str(1));
                    if (rec->dev_idx) rec->dev_idx[0] = g_inputs.GetSize() + INPUT_INDEX_BASE;
                    g_inputs.Add(rec);
                  }

                  break;
                }
              }
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
          if (NSEEL_VM_get_var_refcnt(g_vm,lp.gettoken_str(1))>=0)
          {
            results.AppendFormatted(1024,"Warning: device name '%s' already in use, skipping @output line\r\n",lp.gettoken_str(1));
          }
          else
          {
            // oscOutputRec
            oscOutputRec *r = new oscOutputRec(lp.gettoken_str(2), 
              lp.getnumtokens()>=4 ? lp.gettoken_int(3) : 0,
              lp.getnumtokens()>=5 ? lp.gettoken_int(4) : -1);

            if (r->m_sendsock<0)
            {
              results.AppendFormatted(1024,"Warning: failed creating destination for @output '%s' -> '%s'\r\n",lp.gettoken_str(1),lp.gettoken_str(2));
              delete r;
            }
            else
            {
              EEL_F *var=NSEEL_VM_regvar(g_vm,lp.gettoken_str(1));
              if (var) *var=g_outputs.GetSize() + OUTPUT_INDEX_BASE;
              g_outputs.Add(r);
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
            results.AppendFormatted(1024,"Warning: line '%.100s' (and possibly more)' are not in valid section and may be ignored\r\n");
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

  if (!g_inputs.GetSize()) results.Append("Warning: No @input opened\r\n");
  if (!g_outputs.GetSize()) results.Append("Warning: No @output opened\r\n");
  if (!g_formats.GetSize()) results.Append("Warning: No formats appear to be defined in code using { }\r\n");

  results.AppendFormatted(512,"\r\n%d inputs, %d outputs, and %d formats opened\r\n",g_inputs.GetSize(),g_outputs.GetSize(),g_formats.GetSize());

  for (x=0;x<g_inputs.GetSize();x++)
  {
    midiInputRec *rec=g_inputs.Get(x);
    if (rec) rec->start();
  }

}


WDL_DLGRET mainProc(HWND hwndDlg, UINT uMsg, WPARAM wParam, LPARAM lParam)
{
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
        for (x=0;x<g_inputs.GetSize();x++) g_inputs.Get(x)->run();

        if (g_var_time) *g_var_time = timeGetTime()/1000.0;

        bool needUIupdate=false;

        if (g_incoming_events.Available())
        {
          static WDL_TypedBuf<incomingEvent> tmp;

          g_incoming_events_mutex.Enter();
          tmp.Resize(g_incoming_events.Available(),false);
          if (tmp.GetSize()==g_incoming_events.Available())
          {
            memcpy(tmp.Get(),g_incoming_events.Get(),tmp.GetSize()*sizeof(incomingEvent));
          }
          else
          {
            tmp.Resize(0,false);
          }
          g_incoming_events.Clear();
          g_incoming_events_mutex.Leave();

          for (x=0;x<tmp.GetSize();x++)
          {
            incomingEvent *evt = tmp.Get()+x;

            int asInt = (evt->msg[0] << 16) | (evt->msg[1] << 8) | evt->msg[2];
            if (g_recent_events[0] != asInt)
            {
              memmove(g_recent_events+1,g_recent_events,sizeof(g_recent_events)-sizeof(g_recent_events[0]));
              g_recent_events[0]=asInt;
              needUIupdate=true;
            }


            if (g_var_msgs[0]) g_var_msgs[0][0] = evt->msg[0];
            if (g_var_msgs[1]) g_var_msgs[1][0] = evt->msg[1];
            if (g_var_msgs[2]) g_var_msgs[2][0] = evt->msg[2];
            if (g_var_msgs[3]) g_var_msgs[3][0] = evt->dev_ptr ? *evt->dev_ptr : -1.0;
            NSEEL_code_execute(g_code[2]);
          }
        }
        if (g_vm && g_code[1]) NSEEL_code_execute(g_code[1]); // timer follows messages

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
            WDL_FastString results;
            reloadScript(results);
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

static EEL_F * NSEEL_CGEN_CALL _send_oscevent(void *opaque, EEL_F *dest_device, EEL_F *fmt_index, EEL_F *value)
{
  int output_idx = (int) floor(*dest_device+0.5);
  oscOutputRec *output = g_outputs.Get(output_idx - OUTPUT_INDEX_BASE);
  if (output || output_idx == -1)
  {
    formatStringRec *rec = g_formats.Get((int) (*fmt_index + 0.5) - FORMAT_INDEX_BASE );
    if (rec && rec->values.GetSize())
    {
      const char *fmt = rec->values.Get(0)->Get();
      char fmt_type = 'f';
      if (fmt[0] == 's' || fmt[0] == 'b' || fmt[0] == 'i') fmt_type = fmt[0];

      if (fmt[0] && fmt[0] != '/') fmt++;

      int fmtcnt=validate_format(fmt);
      if (fmtcnt >= 0 && fmtcnt < 10)
      {
        char buf[1024];
#define FOO(x) (g_var_oscfmt[x] ? g_var_oscfmt[x][0]:0.0)
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
        else wr.PushFloatArg(*value);

        wr.PushFloatArg(*value);


        int l=0;
        const char *ret=wr.GetBuffer(&l);
        if (ret && l>0) 
        {
          if (output)
          {
            output->doSend(ret,l);
          }
          else
          {
            int n;
            for (n=0;n<g_outputs.GetSize();n++)
              g_outputs.Get(n)->doSend(ret,l);
          }
        }
      }

    }
  }
  return value;
}

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR lpCmdLine, int nShowCmd)
{
  g_hInstance = hInstance;
  if (lpCmdLine && *lpCmdLine) 
  {
    cfgfile.Set(lpCmdLine);
  }
  else
  {
    char exepath[2048];
    exepath[0]=0;
    GetModuleFileName(NULL,exepath,sizeof(exepath));
    char *p=exepath;
    while (*p) p++;
    while (p >= exepath && *p != '\\') p--; *++p=0;
    cfgfile.Set(exepath);
    cfgfile.Append("midi2osc.cfg");
  }

  JNL::open_socketlib();

  NSEEL_init();
  NSEEL_addfunctionex("oscsend",3,(char *)_asm_generic3parm,(char *)_asm_generic3parm_end-(char *)_asm_generic3parm,NSEEL_PProc_THIS,(void *)&_send_oscevent);
  // add oscsend() function

  DialogBox(hInstance,MAKEINTRESOURCE(IDD_DIALOG1),GetDesktopWindow(),mainProc);

  g_inputs.Empty(true);
  g_outputs.Empty(true);

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