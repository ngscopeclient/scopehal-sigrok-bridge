
#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <assert.h>

#include <vector>
#include <thread>

#include <libsigrok4DSL/libsigrok.h>

#include "log/log.h"

#include "server.h"
#include "srbinding.h"
#include "SigrokSCPIServer.h"

Socket g_scpiSocket(AF_INET6, SOCK_STREAM, IPPROTO_TCP);
Socket g_dataSocket(AF_INET6, SOCK_STREAM, IPPROTO_TCP);
struct sr_context* g_sr_context = NULL;
struct sr_dev_inst* g_sr_device = NULL;
struct sr_session* g_sr_session = NULL;
std::vector<struct sr_channel*> g_analogChannels{};

int main(int argc, char* argv[])
{
	(void) argc; (void) argv;

	Severity console_verbosity = Severity::DEBUG;
	g_log_sinks.emplace(g_log_sinks.begin(), new ColoredSTDLogSink(console_verbosity));

	chdir("/usr/local/share/DSView/res/");

	LogNotice("libsigrok4DSL ver: '%s'\n", sr_package_version_string_get());
	// sr_log_loglevel_set(4);
	   //    0   None
	   //    1   Error
	   //    2   Warnings
	   //    3   Informational
	   //    4   Debug
	   //    5   Spew

	int err;
	if ((err = sr_init(&g_sr_context)) != SR_OK) {
		LogError("Failed to initialize libsigrok4DSL: %d\n", err);
		return err;
	}

	struct sr_dev_driver* sel_driver;

	const char* wanted_driver = "DSCope";
	// virtual-demo, DSLogic, DSCope

	sr_dev_driver **const drivers = sr_driver_list();
	for (sr_dev_driver **driver = drivers; *driver; driver++) {
		if (strcmp((*driver)->name, wanted_driver) == 0) {
			sel_driver = *driver;
			break;
		}
	}

	if (sr_driver_init(g_sr_context, sel_driver) != SR_OK) {
		LogError("Failed to initialize driver %s\n", sel_driver->name);
		return 1;
	}

	LogNotice("Selected driver %s, scanning...\n", sel_driver->name);

	GSList *const devices = sr_driver_scan(sel_driver, NULL);

	for (GSList *l = devices; l; l = l->next) {
        g_sr_device = (sr_dev_inst*)l->data;
    }

    if (!g_sr_device) {
    	LogError("Found no device\n");
    	return 1;
    }
     
	g_slist_free(devices);

	LogNotice("Found device: %s - %s\n", g_sr_device->vendor, g_sr_device->model);

	if ((err = sr_dev_open(g_sr_device)) != SR_OK) {
		LogError("Failed to sr_dev_open device: %d\n", err);
		return 1;
	}

	g_sr_session = sr_session_new();

	if ((err = sr_session_dev_add(g_sr_device)) != SR_OK) {
		LogError("Failed to add device to session\n");
		return 1;
	}

	ds_trigger_init();

	int bitdepth = get_dev_config<uint8_t>(g_sr_device, SR_CONF_UNIT_BITS).value();
	LogDebug("Bit depth: %d\n", bitdepth);

	int scpi_port = 5025;
	int waveform_port = scpi_port+1;

	//Configure the data plane socket
	g_dataSocket.Bind(waveform_port);
	g_dataSocket.Listen();

	//Launch the control plane socket server
	g_scpiSocket.Bind(scpi_port);
	g_scpiSocket.Listen();

	while(true)
	{
		Socket scpiClient = g_scpiSocket.Accept();
		if(!scpiClient.IsValid())
			break;

		//Create a server object for this connection
		SigrokSCPIServer server(scpiClient.Detach());

		//Launch the data-plane thread
		std::thread dataThread(WaveformServerThread);

		//Process connections on the socket
		server.MainLoop();

		// g_waveformThreadQuit = true;
		dataThread.join();
		// g_waveformThreadQuit = false;
	}

	return 0;
}
