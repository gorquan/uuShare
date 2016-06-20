using System;
using System.Xml;
using System.Text;
using System.Diagnostics;
using System.Security.Cryptography;

namespace Launcher
{
    class Program
    {
        static string HMACKey = "HMACHASHKEYHERE";

        static string hmacSHA256(String data, String key)
        {
            using (HMACSHA256 hmac = new HMACSHA256(Encoding.UTF8.GetBytes(key)))
            {
                return BitConverter.ToString(hmac.ComputeHash(Encoding.ASCII.GetBytes(data))).Replace("-", "").ToLower();
            }
        }

        static void Main(string[] args)
        {
            if (args.Length > 0)
            {
                args[0] = args[0].Replace("uushare:", "");
                Console.WriteLine("Starting uuShare with:");
                Console.WriteLine(args[0]);

                string[] arg = args[0].Split(';');

                if (arg.Length >= 5)
                {
                    byte[] passwordData = Convert.FromBase64String(arg[2]);
                    string password = Encoding.UTF8.GetString(passwordData);

                    string toSign = String.Format("{0}{1}{2}{3}", arg[0], arg[1], password, arg[3]);

                    if (hmacSHA256(toSign, HMACKey) == arg[4])
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

                        nodeNub.Attributes["Password"].Value = password;

                        xmlDoc.Save(AppDomain.CurrentDomain.BaseDirectory + "Settings\\Favorites.xml");

                        ProcessStartInfo start = new ProcessStartInfo();
                        start.FileName = AppDomain.CurrentDomain.BaseDirectory + "ApexDC.exe";

                        Process.Start(start);
                    }
                    else
                    {
                        Console.WriteLine("Invalid signature.");
                    }
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
