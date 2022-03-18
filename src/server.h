
#ifndef server_h
#define server_h

#include <vector>
#include <mutex>

#include <libsigrok4DSL/libsigrok.h>
#include "xptools/Socket.h"

using std::vector;
using std::mutex;

extern Socket g_scpiSocket;
extern Socket g_dataSocket;
extern struct sr_context* g_sr_context;
extern struct sr_dev_inst* g_sr_device;
extern struct sr_session* g_sr_session;

extern vector<struct sr_channel*> g_channels;
extern vector<uint64_t> vdiv_options;
extern uint64_t g_rate, g_depth, g_trigfs;
extern uint8_t g_trigpct;

extern bool g_quit;
extern bool g_run;
extern bool g_running;
extern bool g_oneShot;
extern bool g_capturedFirstFrame;

extern int g_selectedTriggerChannel;
extern int g_numdivs;

int init_and_find_device();
void compute_scale_and_offset(struct sr_channel* ch, float& scale, float& offset);
int count_enabled_channels();
void set_trigger_channel(int ch);
void force_correct_config();
void set_rate(uint64_t rate);
void set_depth(uint64_t depth);
void set_trigfs(uint64_t fs);

bool stop_capture_sync();
void restart_capture();

void WaveformServerThread();

#endif // server_h
