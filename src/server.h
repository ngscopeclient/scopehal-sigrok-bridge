
#ifndef server_h
#define server_h

#include <vector>
#include <libsigrok4DSL/libsigrok.h>
#include "xptools/Socket.h"

extern Socket g_scpiSocket;
extern Socket g_dataSocket;
extern struct sr_context* g_sr_context;
extern struct sr_dev_inst* g_sr_device;
extern struct sr_session* g_sr_session;

extern std::vector<struct sr_channel*> g_analogChannels;

void WaveformServerThread();

#endif // server_h
