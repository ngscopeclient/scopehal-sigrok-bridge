
#include "server.h"

#include "log/log.h"
#include "xptools/Socket.h"

#include <string>
#include <vector>

using std::string;
using std::vector;

Socket g_scpiSocket(AF_INET6, SOCK_STREAM, IPPROTO_TCP);
Socket g_dataSocket(AF_INET6, SOCK_STREAM, IPPROTO_TCP);

bool ScpiSend(Socket& sock, const string& cmd)
{
	string tempbuf = cmd + "\n";

	LogDebug("Send: %s", tempbuf.c_str());
	return sock.SendLooped((unsigned char*)tempbuf.c_str(), tempbuf.length());
}

bool ScpiRecv(Socket& sock, string& str)
{
	int sockid = sock;

	char tmp = ' ';
	str = "";
	while(true)
	{
		if(1 != recv(sockid, &tmp, 1, MSG_WAITALL))
			return false;

		if( (tmp == '\n') || ( (tmp == ';') ) )
			break;
		else
			str += tmp;
	}

	return true;
}

void ParseScpiLine(const string& line, string& subject, string& cmd, bool& query, vector<string>& args)
{
	//Reset fields
	query = false;
	subject = "";
	cmd = "";
	args.clear();

	string tmp;
	bool reading_cmd = true;
	for(size_t i=0; i<line.length(); i++)
	{
		//If there's no colon in the command, the first block is the command.
		//If there is one, the first block is the subject and the second is the command.
		//If more than one, treat it as freeform text in the command.
		if( (line[i] == ':') && subject.empty() )
		{
			subject = tmp;
			tmp = "";
			continue;
		}

		//Detect queries
		if(line[i] == '?')
		{
			query = true;
			continue;
		}

		//Comma delimits arguments, space delimits command-to-args
		if(!(isspace(line[i]) && cmd.empty()) && line[i] != ',')
		{
			tmp += line[i];
			continue;
		}

		//merge multiple delimiters into one delimiter
		if(tmp == "")
			continue;

		//Save command or argument
		if(reading_cmd)
			cmd = tmp;
		else
			args.push_back(tmp);

		reading_cmd = false;
		tmp = "";
	}

	//Stuff left over at the end? Figure out which field it belongs in
	if(tmp != "")
	{
		if(cmd != "")
			args.push_back(tmp);
		else
			cmd = tmp;
	}
}

void run_server(struct sr_dev_inst *device, int scpi_port) {
	//Launch the control plane socket server
	g_scpiSocket.Bind(scpi_port);
	g_scpiSocket.Listen();

	//Configure the data plane socket
	g_dataSocket.Bind(scpi_port + 1);
	g_dataSocket.Listen();

	LogNotice("Started SCPI server at %d\n", scpi_port);

	while(true)
	{
		Socket client = g_scpiSocket.Accept();
		Socket dataClient(-1);
		LogVerbose("Client connected to control plane socket\n");

		if(!client.IsValid())
			break;
		if(!client.DisableNagle())
			LogWarning("Failed to disable Nagle on socket, performance may be poor\n");

		//Main command loop
		while(true)
		{
			string line;
			if(!ScpiRecv(client, line))
				break;

			LogDebug("Receive: %s\n", line.c_str());

			string subject, cmd;
			bool query;
			vector<string> args;
			ParseScpiLine(line, subject, cmd, query, args);

			LogDebug("Parsed: s='%s', c='%s', q=%d, a=[", subject.c_str(), cmd.c_str(), query);
			for (auto i : args) {
				LogDebug("'%s',", i.c_str());
			}
			LogDebug("]\n");

			if(query)
			{
				//Read ID code
				if(cmd == "*IDN")
					ScpiSend(client, string("DreamSourceLabs (bridge),") + device->model + ",NOSERIAL,NOVERSION");
				else
					LogWarning("Unknown command: %s\n", line.c_str());
			}
		}
	}
}

