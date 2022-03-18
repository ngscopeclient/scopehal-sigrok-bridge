
#include "server.h"
#include "SigrokSCPIServer.h"

using namespace std;

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Construction / destruction

SigrokSCPIServer::SigrokSCPIServer(ZSOCKET sock)
	: BridgeSCPIServer(sock)
{
	LogVerbose("Client connected\n");
}

SigrokSCPIServer::~SigrokSCPIServer()
{
	LogVerbose("Client disconnected\n");
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Command parsing

size_t SigrokSCPIServer::GetChannelID(const string& subject)
{
	return subject[0] - '0';
}

bool SigrokSCPIServer::OnQuery(
		const string& line,
		const string& subject,
		const string& cmd,
		const vector<string>& args)
{
	if(BridgeSCPIServer::OnQuery(line, subject, cmd, args))
		return true;

	//TODO: handle commands not implemented by the base class
	LogWarning("Unrecognized query received: %s\n", line.c_str());

	return false;
}

string SigrokSCPIServer::GetMake()
{
	return "DreamSourceLabs (bridge)";
}

string SigrokSCPIServer::GetModel()
{
	return g_sr_device->model;
}

string SigrokSCPIServer::GetSerial()
{
	return "NOSERIAL";
}

string SigrokSCPIServer::GetFirmwareVersion()
{
	return "NOVERSION";
}

size_t SigrokSCPIServer::GetAnalogChannelCount()
{
	return g_analogChannels.size();
}

vector<size_t> SigrokSCPIServer::GetSampleRates()
{
	return {};
}

vector<size_t> SigrokSCPIServer::GetSampleDepths()
{
	return {};
}

bool SigrokSCPIServer::OnCommand(
		const string& line,
		const string& subject,
		const string& cmd,
		const vector<string>& args)
{
	(void) line; (void) subject; (void) cmd; (void) args;
	return false;
}

void SigrokSCPIServer::Stop()
{
	;
}

void SigrokSCPIServer::Start(bool force)
{
	(void) force;
}
