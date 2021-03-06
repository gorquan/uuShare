// Launcher.cpp : 定义控制台应用程序的入口点。
//

#include "stdafx.h"
#include <WinSock2.h>
#include <iostream>
#include <fstream>

#include "base64.h"
#include "sha256.h"
#include "hmac.h"

#include "rapidxml/rapidxml.hpp"
#include "rapidxml/rapidxml_print.hpp"

#define BUFFERSIZE 1024

using namespace std;
using namespace rapidxml;

struct Version
{
	explicit Version(string versionStr)
	{
		sscanf(versionStr.c_str(), "%d.%d.%d.%d", &major, &minor, &revision, &build);
	}

	bool operator<(const Version &otherVersion) const
	{
		if (major < otherVersion.major)
			return true;
		if (minor < otherVersion.minor)
			return true;
		if (revision < otherVersion.revision)
			return true;
		if (build < otherVersion.build)
			return true;
		return false;
	}

	int major, minor, revision, build;
};

void die_with_wserror(char *errorMessage)
{
	cerr << errorMessage << ": " << WSAGetLastError() << endl;
	exit(1);
}

bool check_update(string mainFile)
{
	string request;
	string response;
	int resp_leng;

	char buffer[BUFFERSIZE];
	struct sockaddr_in serveraddr;
	int sock;

	WSADATA wsaData;
	char *ipaddress = "10.129.0.218";
	auto port = 80;

	request += "GET /version.xml HTTP/1.0\r\n";
	request += "Host: share.xuulm.com\r\n";
	request += "\r\n";

	//init winsock
	if (WSAStartup(MAKEWORD(2, 0), &wsaData) != 0)
		die_with_wserror("WSAStartup() failed");

	//open socket
	if ((sock = socket(PF_INET, SOCK_STREAM, IPPROTO_TCP)) < 0)
		die_with_wserror("socket() failed");

	//connect
	memset(&serveraddr, 0, sizeof(serveraddr));
	serveraddr.sin_family = AF_INET;
	// ReSharper disable once CppDeprecatedEntity
	serveraddr.sin_addr.s_addr = inet_addr(ipaddress);
	serveraddr.sin_port = htons(static_cast<unsigned short>(port));
	if (connect(sock, reinterpret_cast<struct sockaddr *>(&serveraddr), sizeof(serveraddr)) < 0)
		die_with_wserror("connect() failed");

	//send request
	if (send(sock, request.c_str(), request.length(), 0) != request.length())
		die_with_wserror("send() sent a different number of bytes than expected");

	//get response
	response = "";
	resp_leng = BUFFERSIZE;
	while (resp_leng == BUFFERSIZE)
	{
		resp_leng = recv(sock, reinterpret_cast<char*>(&buffer), BUFFERSIZE, 0);
		if (resp_leng>0)
			response += string(buffer).substr(0, resp_leng);
		//note: download lag is not handled in this code
	}

	//display response
	response = response.substr(response.find("\r\n\r\n"));

	//disconnect
	closesocket(sock);

	//cleanup
	WSACleanup();

	xml_document<> doc;
	doc.parse<parse_no_data_nodes>(&response[0u]);
	string latest = doc.first_node("DCUpdate")->first_node("Version")->value();
	latest += ".0";

	cout << "最新版本：" << latest << endl;

	wstring filename(mainFile.begin(), mainFile.end());
	DWORD handle = 0;
	auto size = GetFileVersionInfoSize(filename.c_str(), &handle);
	auto versionInfo = new BYTE[size];
	if (!GetFileVersionInfo(filename.c_str(), handle, size, versionInfo))
	{
		delete[] versionInfo;
		return false;
	}
	// we have version information
	UINT                len = 0;
	VS_FIXEDFILEINFO*   vsfi = nullptr;
	VerQueryValue(versionInfo, L"\\", reinterpret_cast<void**>(&vsfi), &len);
	char version[BUFFERSIZE];
	sprintf(version, "%d.%d.%d.%d",
		HIWORD(vsfi->dwFileVersionMS),
		LOWORD(vsfi->dwFileVersionMS),
		HIWORD(vsfi->dwFileVersionLS),
		LOWORD(vsfi->dwFileVersionLS)
	);
	delete[] versionInfo;

	cout << "当前版本：" << version << endl;

	return Version(version) < Version(latest);
}

int main(int argc, char **argv)
{
    ios::sync_with_stdio(false);

    if (argc > 1)
    {
	    auto i = 0;
	    auto p = strtok(argv[1], ";");
        char *array[3];

        while (p != nullptr)
        {
            array[i++] = p;
            p = strtok(nullptr, ";");
        }

    	if (i >= 5)
        {
            // array[0] Nick
            // array[1] E-mail
            // array[2] password(base64-encoded)
            // array[3] timestamp
            // array[4] signature

            strtok(array[0], ":");
            array[0] = strtok(nullptr, ";");

	        auto decoded = base64_decode(array[2]);
            string password(decoded.begin(), decoded.end());

            string toSign;
            toSign.append(array[1]);
            toSign.append(password);
            toSign.append(array[3]);

            string signature(array[4]);

	        auto sha2hmac = hmac<SHA256>(toSign, "jCklegaBT8j3WcjdzaNi5iNz8KnHT9mi");

            if (sha2hmac == signature) {
                cout << "Nick Name:\t" << array[0] << endl
                    << "E-mail:\t\t" << array[1] << endl;

                TCHAR NPath[MAX_PATH];
                GetModuleFileName(nullptr, NPath, MAX_PATH);
	            auto iLen = WideCharToMultiByte(CP_ACP, 0, NPath, -1, nullptr, 0, nullptr, nullptr);
	            auto chRtn = new char[iLen*sizeof(char)];
                WideCharToMultiByte(CP_ACP, 0, NPath, -1, chRtn, iLen, nullptr, nullptr);

	            auto pos = string(chRtn).find_last_of("\\/");
                string path(chRtn);
                path = path.substr(0, pos);

				if (!check_update(path + "\\ApexDC.exe"))
				{
					// Update DCPlusPlus.xml
					auto dcppPath = path + "\\Settings\\DCPlusPlus.xml";

					ifstream ifile(dcppPath);
					string str((istreambuf_iterator<char>(ifile)),
						istreambuf_iterator<char>());
					ifile.close();

					xml_document<> doc;
					doc.parse<parse_no_data_nodes>(&str[0u]);

					string nick_string(array[0]);
					const char * nick_text = doc.allocate_string(nick_string.c_str(), strlen(nick_string.c_str()));
					doc.first_node("DCPlusPlus")->first_node("Settings")->first_node("Nick")->value(nick_text);

					string email_string(array[1]);
					const char * email_text = doc.allocate_string(email_string.c_str(), strlen(email_string.c_str()));
					doc.first_node("DCPlusPlus")->first_node("Settings")->first_node("EMail")->value(email_text);

					string ostr;
					print(back_inserter(ostr), doc);
					ofstream ofile(dcppPath);
					ofile << ostr;
					ofile.close();

					// Update Favorites.xml
					auto favPath = path + "\\Settings\\Favorites.xml";

					ifstream favfile(favPath);
					string favstr((istreambuf_iterator<char>(favfile)),
						istreambuf_iterator<char>());
					favfile.close();

					doc.parse<parse_no_data_nodes>(&favstr[0u]);

					nick_text = doc.allocate_string(nick_string.c_str(), strlen(nick_string.c_str()));
					doc.first_node("Favorites")->first_node("Hubs")->first_node("Hub")->first_attribute("Nick")->value(nick_text);

					email_text = doc.allocate_string(email_string.c_str(), strlen(email_string.c_str()));
					doc.first_node("Favorites")->first_node("Hubs")->first_node("Hub")->first_attribute("Email")->value(email_text);

					const char * password_text = doc.allocate_string(password.c_str(), strlen(password.c_str()));
					doc.first_node("Favorites")->first_node("Hubs")->first_node("Hub")->first_attribute("Password")->value(password_text);

					string ofavstr;
					print(back_inserter(ofavstr), doc);
					ofstream ofavfile(favPath);
					ofavfile << ofavstr;
					ofavfile.close();

					auto dcPath = "pushd " + path + " && start ApexDC.exe";
					system(dcPath.c_str());
				}
            	else
				{
					string dcPath = "explorer \"http://soft.xuulm.com/?p=3530\"";
					system(dcPath.c_str());

					cout << "客户端太旧" << endl;
					getchar();
				}
            }
            else
            {
                cout << "参数签名无效" << endl;
				getchar();
            }
        }
        else
        {
            cout << "参数无效" << endl;
			getchar();
        }
    }
    else
    {
        cout << "无参数传入" << endl;
		getchar();
    }
    return 0;
}
