// Launcher.cpp : 定义控制台应用程序的入口点。
//

#include "stdafx.h"
#include <windows.h>
#include <tchar.h>
#include <iostream>
#include <fstream>
#include <locale>
#include <string>

#include <VersionHelpers.h>

#include "base64.h"
#include "sha256.h"
#include "hmac.h"

#include "rapidxml\rapidxml.hpp"
#include "rapidxml\rapidxml_print.hpp"

using namespace std;
using namespace rapidxml;

int main(int argc, char **argv)
{
    ios::sync_with_stdio(false);
    locale loc("chs");
    wcout.imbue(loc);

    if (argc > 1)
    {
        int i = 0;
        char *p = strtok(argv[1], ";");
        char *array[3];

        while (p != NULL)
        {
            array[i++] = p;
            p = strtok(NULL, ";");
        }

        if (i >= 5)
        {
            // array[0] Nick
            // array[1] E-mail
            // array[2] password(base64-encoded)
            // array[3] timestamp
            // array[4] signature

            p = strtok(array[0], ":");
            array[0] = strtok(NULL, ";");

            vector<BYTE> decoded = base64_decode(array[2]);
            string password(decoded.begin(), decoded.end());

            string toSign;
            toSign.append(array[1]);
            toSign.append(password);
            toSign.append(array[3]);

            string signature(array[4]);
            
            string sha2hmac = hmac<SHA256>(toSign, "__YOUR_KEY_HERE__");

            if (sha2hmac == signature) {
                cout << "Nick Name:\t" << array[0] << endl
                    << "E-mail:\t\t" << array[1] << endl;

                TCHAR NPath[MAX_PATH];
                GetModuleFileName(NULL, NPath, MAX_PATH);
                int iLen = WideCharToMultiByte(CP_ACP, 0, NPath, -1, NULL, 0, NULL, NULL);
                char* chRtn = new char[iLen*sizeof(char)];
                WideCharToMultiByte(CP_ACP, 0, NPath, -1, chRtn, iLen, NULL, NULL);

                string::size_type pos = string(chRtn).find_last_of("\\/");
                string path(chRtn);
                path = path.substr(0, pos);

                // Update DCPlusPlus.xml
                string dcppPath = path + "\\Settings\\DCPlusPlus.xml";

                std::ifstream ifile(dcppPath);
                ifile.imbue(loc);
                string str((istreambuf_iterator<char>(ifile)),
                    std::istreambuf_iterator<char>());
                ifile.close();

                xml_document<> doc;
                doc.parse<rapidxml::parse_no_data_nodes>(&str[0u]);

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
                string favPath = path + "\\Settings\\Favorites.xml";

                std::ifstream favfile(favPath);
                favfile.imbue(loc);
                string favstr((istreambuf_iterator<char>(favfile)),
                    std::istreambuf_iterator<char>());
                favfile.close();

                doc.parse<rapidxml::parse_no_data_nodes>(&favstr[0u]);

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

                string appFile;
                if (IsWindowsVistaOrGreater())
                {
                    BOOL f64 = FALSE;
                    f64 = IsWow64Process(GetCurrentProcess(), &f64) && f64;
                    appFile = f64 ? "ApexDC-x64.exe" : "ApexDC.exe";
                }
                else
                {
                    appFile = "ApexDC-XP.exe";
                }

                string dcPath = "pushd " + path + " && start " + appFile;
                system(dcPath.c_str());
            }
            else
            {
                cout << "参数签名无效" << endl;
            }
        }
        else
        {
            cout << "参数无效" << endl;
        }
    }
    else
    {
        cout << "无参数传入" << endl;
    }
    return 0;
}
