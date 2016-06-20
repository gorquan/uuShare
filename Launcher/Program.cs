using System;
using System.Xml;
using System.Text;
using System.Diagnostics;

namespace Launcher
{
    class Program
    {
        static void Main(string[] args)
        {
            if (args.Length > 0) {
                args[0] = args[0].Replace("uushare:", "");
                Console.WriteLine("Starting uuShare with:");
                Console.WriteLine(args[0]);

                string[] arg = args[0].Split(';');

                if (arg.Length >= 3)
                {
                    Console.WriteLine("Nick:\t" + arg[0]);
                    Console.WriteLine("E-mail:\t" + arg[1]);

                    XmlDocument xmlDoc = new XmlDocument();

                    // Global Config

                    xmlDoc.Load(AppDomain.CurrentDomain.BaseDirectory + "Settings\\DCPlusPlus.xml");

                    XmlNode nodeNick = xmlDoc.SelectSingleNode("DCPlusPlus/Settings/Nick");
                    nodeNick.InnerText = arg[0];

                    XmlNode nodeEmail = xmlDoc.SelectSingleNode("DCPlusPlus/Settings/EMail");
                    nodeEmail.InnerText = arg[1];

                    xmlDoc.Save(AppDomain.CurrentDomain.BaseDirectory + "Settings\\DCPlusPlus.xml");

                    // Favorites

                    xmlDoc.Load(AppDomain.CurrentDomain.BaseDirectory + "Settings\\Favorites.xml");

                    XmlNode nodeNub = xmlDoc.SelectSingleNode("Favorites/Hubs/Hub");
                    nodeNub.Attributes["Nick"].Value = arg[0];
                    nodeNub.Attributes["Email"].Value = arg[1];

                    byte[] passwordData = Convert.FromBase64String(arg[2]);
                    string password = Encoding.UTF8.GetString(passwordData);

                    nodeNub.Attributes["Password"].Value = password;

                    xmlDoc.Save(AppDomain.CurrentDomain.BaseDirectory + "Settings\\Favorites.xml");

                    ProcessStartInfo start = new ProcessStartInfo();
                    start.FileName = AppDomain.CurrentDomain.BaseDirectory + "ApexDC.exe";

                    Process.Start(start);
                }
                else
                {
                    Console.WriteLine("Invalid arguments.");
                }
            }
            else
            {
                Console.WriteLine("No arguments passed.");
            }
        }
    }
}
