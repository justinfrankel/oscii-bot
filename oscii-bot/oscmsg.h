#ifndef _M2O_OSCMSG_H_
#define _M2O_OSCMSG_H_

#define MAX_OSC_MSG_LEN 1024

static void OSC_BSWAPINTMEM(void *buf)
{
  char *p=(char *)buf;
  char tmp=p[0]; p[0]=p[3]; p[3]=tmp;
  tmp=p[1]; p[1]=p[2]; p[2]=tmp;
}

#ifdef __ppc__
#define OSC_MAKEINTMEM4BE(x)
#else
#define OSC_MAKEINTMEM4BE(x) OSC_BSWAPINTMEM(x)
#endif


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
    OSC_MAKEINTMEM4BE(m_arg_ptr);
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
    OSC_MAKEINTMEM4BE(m_arg_ptr);
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

#endif