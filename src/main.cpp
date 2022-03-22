
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

int main(int argc, char* argv[])
{
	char* drivername;

	if (argc != 2) {
		printf("Usage: %s <driver name>\n", argv[0]);
		return 1;
	}

	drivername = argv[1];
	int req_bus = -1;
	int req_dev = -1;

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
	// virtual-demo, DSLogic, DSCope

	if (init_and_find_device(drivername, req_bus, req_dev) != 0) return 1;

	int scpi_port = 5025;
	int waveform_port = scpi_port+1;

	//Configure the data plane socket
	g_dataSocket.SetReuseaddr();
	g_dataSocket.Bind(waveform_port);
	g_dataSocket.Listen();

	//Launch the control plane socket server
	g_scpiSocket.SetReuseaddr();
	g_scpiSocket.Bind(scpi_port);
	g_scpiSocket.Listen();

	while(true)
	{
		Socket scpiClient = g_scpiSocket.Accept();
		if(!scpiClient.IsValid())
			break;

		g_quit = false;
		g_run = false;

		//Create a server object for this connection
		SigrokSCPIServer server(scpiClient.Detach());

		//Launch the data-plane thread
		std::thread dataThread(WaveformServerThread);

		//Process connections on the socket
		server.MainLoop();

		g_quit = true;

		dataThread.join();
	}

	return 0;
}
