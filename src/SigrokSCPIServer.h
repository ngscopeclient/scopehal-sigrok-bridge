
#ifndef SigrokSCPIServer_h
#define SigrokSCPIServer_h

#include "scpi-server-tools/BridgeSCPIServer.h"
#include "log/log.h"

/**
	@brief SCPI server for managing control plane traffic to a single client
 */
class SigrokSCPIServer : public BridgeSCPIServer
{
public:
	SigrokSCPIServer(ZSOCKET sock);
	virtual ~SigrokSCPIServer();

	static void Start(bool force = false);

protected:
	virtual std::string GetMake();
	virtual std::string GetModel();
	virtual std::string GetSerial();
	virtual std::string GetFirmwareVersion();
	virtual size_t GetAnalogChannelCount();
	virtual std::vector<size_t> GetSampleRates();
	virtual std::vector<size_t> GetSampleDepths();

	virtual bool OnCommand(
		const std::string& line,
		const std::string& subject,
		const std::string& cmd,
		const std::vector<std::string>& args);

	virtual bool OnQuery(
		const std::string& line,
		const std::string& subject,
		const std::string& cmd,
		const std::vector<std::string>& args);

	virtual size_t GetChannelID(const std::string& subject);

	void Stop();
};

#endif
