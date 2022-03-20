
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
		const std::string& cmd);

	virtual bool GetChannelID(const std::string& subject, size_t& id_out, bool& digital_out);


	virtual void AcquisitionStart(bool oneShot = false);
	virtual void AcquisitionForceTrigger();
	virtual void AcquisitionStop();
	virtual void SetProbeEnabled(size_t chIndex, bool enabled);
	virtual void SetProbeCoupling(size_t chIndex, const std::string& coupling);
	virtual void SetProbeRange(size_t chIndex, double range_V);
	virtual void SetProbeOffset(size_t chIndex, double offset_V);
	virtual void SetSampleRate(uint64_t rate_hz);
	virtual void SetSampleDepth(uint64_t depth);
	virtual void SetTriggerDelay(uint64_t delay_fs);
	virtual void SetTriggerSource(size_t chIndex);
	virtual void SetTriggerLevel(double level_V);
	virtual void SetTriggerTypeEdge();
	virtual void SetEdgeTriggerEdge(const std::string& edge);
};

#endif
