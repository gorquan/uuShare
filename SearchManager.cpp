/* 
 * Copyright (C) 2001-2012 Jacek Sieka, arnetheduck on gmail point com
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA 02111-1307, USA.
 */

#include "stdinc.h"
#include "SearchManager.h"

#include <boost/range/algorithm/for_each.hpp>
#include <boost/range/algorithm_ext/for_each.hpp>

#include "ConnectivityManager.h"
#include "format.h"
#include "UploadManager.h"
#include "ClientManager.h"
#include "ShareManager.h"
#include "SearchResult.h"
#include "ResourceManager.h"
#include "QueueManager.h"
#include "AutoSearchManager.h"
#include "StringTokenizer.h"
#include "FinishedManager.h"
#include "AdcHub.h"
#include "SimpleXML.h"
#include "ScopedFunctor.h"

#include <openssl/evp.h>
#include <openssl/rand.h>

namespace dcpp {

using boost::range::for_each;

const char* SearchManager::types[TYPE_LAST] = {
	CSTRING(ANY),
	CSTRING(AUDIO),
	CSTRING(COMPRESSED),
	CSTRING(DOCUMENT),
	CSTRING(EXECUTABLE),
	CSTRING(PICTURE),
	CSTRING(VIDEO),
	CSTRING(DIRECTORY),
	"TTH"
};
const char* SearchManager::getTypeStr(int type) {
	return types[type];
}

bool SearchManager::isDefaultTypeStr(const string& type) {
	 return type.size() == 1 && type[0] >= '0' && type[0] <= '8';
}

SearchManager::SearchManager() :
	stop(false)
{
	setSearchTypeDefaults();
	TimerManager::getInstance()->addListener(this);
	SettingsManager::getInstance()->addListener(this);
}

SearchManager::~SearchManager() {
	TimerManager::getInstance()->removeListener(this);
	SettingsManager::getInstance()->removeListener(this);
	if(socket.get()) {
		stop = true;
		socket->disconnect();
#ifdef _WIN32
		join();
#endif
	}

	for_each(searchKeys, DeleteFunction());
}

string SearchManager::normalizeWhitespace(const string& aString){
	string::size_type found = 0;
	string normalized = aString;
	while((found = normalized.find_first_of("\t\n\r", found)) != string::npos) {
		normalized[found] = ' ';
		found++;
	}
	return normalized;
}

void SearchManager::search(const string& aName, int64_t aSize, TypeModes aTypeMode /* = TYPE_ANY */, SizeModes aSizeMode /* = SIZE_ATLEAST */, const string& aToken /* = Util::emptyString */, Search::searchType sType, void* aOwner /* = NULL */) {
	StringList who;
	ClientManager::getInstance()->getOnlineClients(who);
	search(who, aName, aSize, aTypeMode, aSizeMode, aToken, StringList(), sType, aOwner);
}

uint64_t SearchManager::search(StringList& who, const string& aName, int64_t aSize /* = 0 */, TypeModes aTypeMode /* = TYPE_ANY */, SizeModes aSizeMode /* = SIZE_ATLEAST */, const string& aToken /* = Util::emptyString */, const StringList& aExtList, Search::searchType sType, void* aOwner /* = NULL */) {
	StringPairList tokenHubList;
	//generate a random key and store it so we can check the results
	uint8_t* key = new uint8_t[16];
	RAND_bytes(key, 16);

	{
		WLock l (cs);
		searchKeys.push_back(key);
		for_each(who, [&](string& hub) {
			string hubToken = Util::toString(Util::rand());
			searches[hubToken] = (SearchItem)(make_tuple(GET_TICK(), aToken, hub));
			tokenHubList.push_back(make_pair(hubToken, hub));
		});
	}
	string keyStr = Encoder::toBase32(key, 16);

	uint64_t estimateSearchSpan = 0;
	for_each(tokenHubList, [&](StringPair& sp) {
		auto s = new Search;
		s->fileType = sType;
		s->size     = aSize;
		s->query    = aName;
		s->sizeType = aSizeMode;
		s->token    = sp.first;
		s->exts	   = aExtList;
		if (strnicmp("adcs://", sp.second.c_str(), 7) == 0)
			s->key		= keyStr;

		if (aOwner)
			s->owners.insert(aOwner);
		s->type		= sType;

		uint64_t ret = ClientManager::getInstance()->search(sp.second, s);
		estimateSearchSpan = max(estimateSearchSpan, ret);			
	});

	return estimateSearchSpan;
}

void SearchManager::listen() {
	disconnect();

	try {
		socket.reset(new Socket(Socket::TYPE_UDP));
		socket->setLocalIp4(CONNSETTING(BIND_ADDRESS));
		socket->setLocalIp6(CONNSETTING(BIND_ADDRESS6));
		port = socket->listen(Util::toString(CONNSETTING(UDP_PORT)));
		start();
	} catch(...) {
		socket.reset();
		throw;
	}
}

void SearchManager::disconnect() noexcept {
	if(socket.get()) {
		stop = true;
		socket->disconnect();
		port.clear();

		join();

		socket.reset();

		stop = false;
	}
}

#define BUFSIZE 8192
int SearchManager::run() {
	boost::scoped_array<uint8_t> buf(new uint8_t[BUFSIZE]);
	int len;
	string remoteAddr;

	while(!stop) {
		try {
			if(!socket->wait(400, true, false).first) {
				continue;
			}

			if((len = socket->read(&buf[0], BUFSIZE, remoteAddr)) > 0) {
				onData(&buf[0], len, remoteAddr);
				continue;
			}
		} catch(const SocketException& e) {
			dcdebug("SearchManager::run Error: %s\n", e.getError().c_str());
		}

		bool failed = false;
		while(!stop) {
			try {
				socket->disconnect();
				port = socket->listen(Util::toString(CONNSETTING(UDP_PORT)));
				if(failed) {
					LogManager::getInstance()->message("Search enabled again", LogManager::LOG_INFO);
					failed = false;
				}
				break;
			} catch(const SocketException& e) {
				dcdebug("SearchManager::run Stopped listening: %s\n", e.getError().c_str());

				if(!failed) {
					LogManager::getInstance()->message(str(boost::format("Search disabled: %1%") % e.getError()), LogManager::LOG_ERROR);
					failed = true;
				}

				// Spin for 60 seconds
				for(auto i = 0; i < 60 && !stop; ++i) {
					Thread::sleep(1000);
				}
			}
		}
	}
	return 0;
}

void SearchManager::onData(const uint8_t* buf, size_t aLen, const string& remoteIp) {
	string x((char*)buf, aLen);

	//check if this packet has been encrypted
	if (aLen >= 32 && ((aLen & 15) == 0)) {
		RLock l (cs);
		for(auto i = searchKeys.cbegin(); i != searchKeys.cend(); ++i) {
			uint8_t out[BUFSIZE];

			uint8_t ivd[16] = { };

			/*AES_KEY key;
			AES_set_encrypt_key(tmp, 128, &key);
			AES_cbc_encrypt(buf, out, aLen, &key, ivd, AES_DECRYPT);*/

			EVP_CIPHER_CTX ctx;
			EVP_CIPHER_CTX_init(&ctx);

			int len = 0, tmpLen=0;
			EVP_DecryptInit_ex(&ctx, EVP_aes_128_cbc(), NULL, *i, ivd);
			EVP_DecryptUpdate(&ctx, out, &len, buf, aLen);
			EVP_DecryptFinal_ex(&ctx, out + aLen, &tmpLen);
			EVP_CIPHER_CTX_cleanup(&ctx);

			// Validate padding and replace with 0-bytes.
			int padlen = out[aLen-1];
			if(padlen < 1 || padlen > 16) {
				continue;
			}

			for(auto r=0; r<padlen; r++) {
				if(out[aLen-padlen+r] != padlen)
					break;
				else
					out[aLen-padlen+r] = 0;
			}

			x = (char*)out+16;
			break;
		}
	}

	COMMAND_DEBUG(x, DebugManager::TYPE_CLIENT_UDP, DebugManager::INCOMING, remoteIp);
	if(x.compare(0, 4, "$SR ") == 0) {
		string::size_type i, j;
		// Directories: $SR <nick><0x20><directory><0x20><free slots>/<total slots><0x05><Hubname><0x20>(<Hubip:port>)
		// Files:		$SR <nick><0x20><filename><0x05><filesize><0x20><free slots>/<total slots><0x05><Hubname><0x20>(<Hubip:port>)
		i = 4;
		if( (j = x.find(' ', i)) == string::npos) {
			return;
		}
		string nick = x.substr(i, j-i);
		i = j + 1;

		// A file has 2 0x05, a directory only one
		size_t cnt = count(x.begin() + j, x.end(), 0x05);

		SearchResult::Types type = SearchResult::TYPE_FILE;
		string file;
		int64_t size = 0;

		if(cnt == 1) {
			// We have a directory...find the first space beyond the first 0x05 from the back
			// (dirs might contain spaces as well...clever protocol, eh?)
			type = SearchResult::TYPE_DIRECTORY;
			// Get past the hubname that might contain spaces
			if((j = x.rfind(0x05)) == string::npos) {
				return;
			}
			// Find the end of the directory info
			if((j = x.rfind(' ', j-1)) == string::npos) {
				return;
			}
			if(j < i + 1) {
				return;
			}
			file = x.substr(i, j-i) + '\\';
		} else if(cnt == 2) {
			if( (j = x.find((char)5, i)) == string::npos) {
				return;
			}
			file = x.substr(i, j-i);
			i = j + 1;
			if( (j = x.find(' ', i)) == string::npos) {
				return;
			}
			size = Util::toInt64(x.substr(i, j-i));
		}
		i = j + 1;

		if( (j = x.find('/', i)) == string::npos) {
			return;
		}
		uint8_t freeSlots = (uint8_t)Util::toInt(x.substr(i, j-i));
		i = j + 1;
		if( (j = x.find((char)5, i)) == string::npos) {
			return;
		}
		uint8_t slots = (uint8_t)Util::toInt(x.substr(i, j-i));
		i = j + 1;
		if( (j = x.rfind(" (")) == string::npos) {
			return;
		}
		string hubName = x.substr(i, j-i);
		i = j + 2;
		if( (j = x.rfind(')')) == string::npos) {
			return;
		}

		string hubIpPort = x.substr(i, j-i);
		string url = ClientManager::getInstance()->findHub(hubIpPort);

		string encoding = ClientManager::getInstance()->findHubEncoding(url);
		nick = Text::toUtf8(nick, encoding);
		file = Text::toUtf8(file, encoding);
		hubName = Text::toUtf8(hubName, encoding);

		UserPtr user = ClientManager::getInstance()->findUser(nick, url);
		if(!user) {
			// Could happen if hub has multiple URLs / IPs
			user = ClientManager::getInstance()->findLegacyUser(nick);
			if(!user)
				return;
		}
		ClientManager::getInstance()->setIPUser(user, remoteIp);

		string tth;
		if(hubName.compare(0, 4, "TTH:") == 0) {
			tth = hubName.substr(4);
			StringList names = ClientManager::getInstance()->getHubNames(user->getCID(), Util::emptyString);
			hubName = names.empty() ? STRING(OFFLINE) : Util::toString(names);
		}

		if(tth.empty() && type == SearchResult::TYPE_FILE && !SettingsManager::lanMode) {
			return;
		}


		SearchResultPtr sr(new SearchResult(user, type, slots, freeSlots, size,
			file, hubName, url, remoteIp, SettingsManager::lanMode ? TTHValue() : TTHValue(tth), Util::emptyString));
		fire(SearchManagerListener::SR(), sr);

	} else if(x.compare(1, 4, "RES ") == 0 && x[x.length() - 1] == 0x0a) {
		AdcCommand c(x.substr(0, x.length()-1));
		if(c.getParameters().empty())
			return;
		string cid = c.getParam(0);
		if(cid.size() != 39)
			return;

		UserPtr user = ClientManager::getInstance()->findUser(CID(cid));
		if(!user)
			return;

		// This should be handled by AdcCommand really...
		c.getParameters().erase(c.getParameters().begin());

		onRES(c, user, remoteIp);
	} else if(x.compare(1, 4, "DSR ") == 0 && x[x.length() - 1] == 0x0a) {
		AdcCommand c(x.substr(0, x.length()-1));
		if(c.getParameters().empty())
			return;
		string cid = c.getParam(0);
		if(cid.size() != 39)
			return;

		c.getParameters().erase(c.getParameters().begin());
		onDSR(c);
	} else if (x.compare(1, 4, "PSR ") == 0 && x[x.length() - 1] == 0x0a) {
		AdcCommand c(x.substr(0, x.length()-1));
		if(c.getParameters().empty())
			return;
		string cid = c.getParam(0);
		if(cid.size() != 39)
			return;

		UserPtr user = ClientManager::getInstance()->findUser(CID(cid));
		// when user == NULL then it is probably NMDC user, check it later
			
		c.getParameters().erase(c.getParameters().begin());			
			
		SearchManager::getInstance()->onPSR(c, user, remoteIp);
		
	} else if (x.compare(1, 4, "PBD ") == 0 && x[x.length() - 1] == 0x0a) {
		if (!SETTING(USE_PARTIAL_SHARING)) {
			return;
		}
		//LogManager::getInstance()->message("GOT PBD UDP: " + x);
		AdcCommand c(x.substr(0, x.length()-1));
		if(c.getParameters().empty())
			return;
		string cid = c.getParam(0);
		if(cid.size() != 39)
			return;

		UserPtr user = ClientManager::getInstance()->findUser(CID(cid));
			
		c.getParameters().erase(c.getParameters().begin());			
			
		if (user)
			SearchManager::getInstance()->onPBD(c, user);
		
	} else if ((x.compare(1, 4, "UBD ") == 0 || x.compare(1, 4, "UBN ") == 0) && x[x.length() - 1] == 0x0a) {
		AdcCommand c(x.substr(0, x.length()-1));
		if(c.getParameters().empty())
			return;
			
		c.getParameters().erase(c.getParameters().begin());			
			
		if (x.compare(1, 4, "UBN ") == 0) {
			//LogManager::getInstance()->message("GOT UBN UDP: " + x);
			UploadManager::getInstance()->onUBN(c);
		} else {
			//LogManager::getInstance()->message("GOT UBD UDP: " + x);
			UploadManager::getInstance()->onUBD(c);
		}
	}
	/*else if(x.compare(1, 4, "SCH ") == 0 && x[x.length() - 1] == 0x0a) {
		try {
			respond(AdcCommand(x.substr(0, x.length()-1)));
		} catch(ParseException& ) {
		}
	}*/ // Needs further DoS investigation
}

void SearchManager::onDSR(const AdcCommand& cmd) {
	string token;
	if (cmd.hasFlag("ED1", 0)) {
		cmd.getParam("TO", 1, token);
		fire(SearchManagerListener::DirectSearchEnd(), token);
		return;
	}

	string folderName;
	for(auto i = cmd.getParameters().begin(); i != cmd.getParameters().end(); ++i) {
		const string& str = *i;
		if(str.compare(0, 2, "FN") == 0) {
			folderName = str.substr(2);
		} else if(str.compare(0, 2, "TO") == 0) {
			token = str.substr(2);
		}
	}

	if (!folderName.empty() && !token.empty()) {
		DirectSearchResultPtr dsr(new DirectSearchResult(folderName));
		dsr->setToken(token);
		fire(SearchManagerListener::DSR(), dsr);
	}
}

void SearchManager::onRES(const AdcCommand& cmd, const UserPtr& from, const string& remoteIp) {
	int freeSlots = -1;
	int64_t size = -1;
	string file;
	string tth;
	string token;

	for(auto i = cmd.getParameters().begin(); i != cmd.getParameters().end(); ++i) {
		const string& str = *i;
		if(str.compare(0, 2, "FN") == 0) {
			file = Util::toNmdcFile(str.substr(2));
		} else if(str.compare(0, 2, "SL") == 0) {
			freeSlots = Util::toInt(str.substr(2));
		} else if(str.compare(0, 2, "SI") == 0) {
			size = Util::toInt64(str.substr(2));
		} else if(str.compare(0, 2, "TR") == 0) {
			tth = str.substr(2);
		} else if(str.compare(0, 2, "TO") == 0) {
			token = str.substr(2);
		}
	}

	if(!file.empty() && freeSlots != -1 && size != -1) {
		TTHValue th;
		/// @todo get the hub this was sent from, to be passed as a hint? (eg by using the token?)
		StringList names = ClientManager::getInstance()->getHubNames(from->getCID());
		string hubName = names.empty() ? STRING(OFFLINE) : Util::toString(names);
		string hub, localToken;

		{
			RLock l (cs);
			auto i = searches.find(token);
			if (i != searches.end()) {
				localToken = get<LOCALTOKEN>((*i).second);
				hub = get<HUBURL>((*i).second);
			}
		}

		auto type = (file[file.length() - 1] == '\\' ? SearchResult::TYPE_DIRECTORY : SearchResult::TYPE_FILE);
		if(type == SearchResult::TYPE_FILE && tth.empty())
			return;

		if (type == SearchResult::TYPE_DIRECTORY || SettingsManager::lanMode) {
			//calculate a TTH from the directory name and size
			TigerHash tmp;
			string tmp2 = (type == SearchResult::TYPE_FILE ? Util::getFileName(file) : Util::getLastDir(file)) + Util::toString(size);
			tmp.update(Text::toLower(tmp2).c_str(), tmp2.length());
			th = TTHValue(tmp.finalize());
		} else {
			th = TTHValue(tth);
		}
		
		uint8_t slots = ClientManager::getInstance()->getSlots(from->getCID());
		SearchResultPtr sr(new SearchResult(from, type, slots, (uint8_t)freeSlots, size,
			file, hubName, hub, remoteIp, th, localToken));
		fire(SearchManagerListener::SR(), sr);
	}
}

void SearchManager::on(TimerManagerListener::Minute, uint64_t aTick) noexcept {
	WLock l (cs);
	for (auto i = searches.begin(); i != searches.end();) {
		if (get<SEARCHTIME>((*i).second) + 1000*60 <  aTick) {
			searches.erase(i);
			i = searches.begin();
		} else {
			++i;
		}
	}
}

void SearchManager::onPBD(const AdcCommand& cmd, UserPtr from) {
	string remoteBundle;
	string hubIpPort;
	string tth;
	bool add=false, update=false, reply=false, notify = false, remove = false;

	for(auto i = cmd.getParameters().begin(); i != cmd.getParameters().end(); ++i) {
		const string& str = *i;
		if(str.compare(0, 2, "HI") == 0) {
			hubIpPort = str.substr(2);
		} else if(str.compare(0, 2, "BU") == 0) {
			remoteBundle = str.substr(2);
		} else if(str.compare(0, 2, "TH") == 0) {
			tth = str.substr(2);
		} else if(str.compare(0, 2, "UP") == 0) { //new notification of a finished TTH
			update=true;
		} else if (str.compare(0, 2, "AD") == 0) { //add TTHList
			add=true;
		} else if (str.compare(0, 2, "RE") == 0) { //require reply
			reply=true;
		} else if (str.compare(0, 2, "NO") == 0) { //notify only, don't download TTHList
			notify=true;
		} else if (str.compare(0, 2, "RM") == 0) { //remove notifications for a selected user and bundle
			remove=true;
		} else {
			//LogManager::getInstance()->message("ONPBD UNKNOWN PARAM: " + str);
		}
	}

	if (remove && !remoteBundle.empty()) {
		//LogManager::getInstance()->message("ONPBD REMOVE");
		QueueManager::getInstance()->removeBundleNotify(from, remoteBundle);
	}

	if (tth.empty()) {
		//LogManager::getInstance()->message("ONPBD EMPTY TTH");
		return;
	}

	string url = ClientManager::getInstance()->findHub(hubIpPort);

	if (update) {
		//LogManager::getInstance()->message("PBD UPDATE TTH");
		QueueManager::getInstance()->updatePBD(HintedUser(from, url), TTHValue(tth));
		return;
	} else if (remoteBundle.empty()) {
		//LogManager::getInstance()->message("ONPBD EMPTY BUNDLE");
		return;
	}

	HintedUser u = HintedUser(from, url);
	if (notify) {
		//LogManager::getInstance()->message("PBD NOTIFY");
		QueueManager::getInstance()->addFinishedNotify(u, TTHValue(tth), remoteBundle);
	} else if (reply) {
		//LogManager::getInstance()->message("PBD REQUIRE REPLY");

		string localBundle;
		bool notify = false, add = false;
		if (QueueManager::getInstance()->checkPBDReply(u, TTHValue(tth), localBundle, notify, add, remoteBundle)) {
			//LogManager::getInstance()->message("PBD REPLY: ACCEPTED");
			AdcCommand cmd = toPBD(hubIpPort, localBundle, tth, false, add, notify);
			ClientManager::getInstance()->send(cmd, from->getCID(), false, true);
		} else {
			//LogManager::getInstance()->message("PBD REPLY: QUEUEMANAGER FAIL");
		}
	}

	if (add) {
		try {
			QueueManager::getInstance()->addBundleTTHList(u, remoteBundle, TTHValue(tth));
		}catch(const Exception&) { }
	}
}

void SearchManager::onPSR(const AdcCommand& cmd, UserPtr from, const string& remoteIp) {

	string udpPort;
	uint32_t partialCount = 0;
	string tth;
	string hubIpPort;
	string nick;
	PartsInfo partialInfo;

	for(auto i = cmd.getParameters().begin(); i != cmd.getParameters().end(); ++i) {
		const string& str = *i;
		if(str.compare(0, 2, "U4") == 0) {
			udpPort = static_cast<uint16_t>(Util::toInt(str.substr(2)));
		} else if(str.compare(0, 2, "NI") == 0) {
			nick = str.substr(2);
		} else if(str.compare(0, 2, "HI") == 0) {
			hubIpPort = str.substr(2);
		} else if(str.compare(0, 2, "TR") == 0) {
			tth = str.substr(2);
		} else if(str.compare(0, 2, "PC") == 0) {
			partialCount = Util::toUInt32(str.substr(2))*2;
		} else if(str.compare(0, 2, "PI") == 0) {
			StringTokenizer<string> tok(str.substr(2), ',');
			for(auto i = tok.getTokens().begin(); i != tok.getTokens().end(); ++i) {
				partialInfo.push_back((uint16_t)Util::toInt(*i));
			}
		}
	}

	string url = ClientManager::getInstance()->findHub(hubIpPort);
	if(!from || from == ClientManager::getInstance()->getMe()) {
		// for NMDC support
		
		if(nick.empty() || hubIpPort.empty()) {
			return;
		}
		
		from = ClientManager::getInstance()->findUser(nick, url);
		if(!from) {
			// Could happen if hub has multiple URLs / IPs
			from = ClientManager::getInstance()->findLegacyUser(nick);
			if(!from) {
				dcdebug("Search result from unknown user");
				return;
			}
		}
	}
	
	ClientManager::getInstance()->setIPUser(from, remoteIp, udpPort);

	if(partialInfo.size() != partialCount) {
		// what to do now ? just ignore partial search result :-/
		return;
	}

	PartsInfo outPartialInfo;
	QueueItem::PartialSource ps(from->isNMDC() ? ClientManager::getInstance()->getMyNick(url) : Util::emptyString, hubIpPort, remoteIp, udpPort);
	ps.setPartialInfo(partialInfo);

	QueueManager::getInstance()->handlePartialResult(HintedUser(from, url), TTHValue(tth), ps, outPartialInfo);
	
	if(!udpPort.empty() && !outPartialInfo.empty()) {
		try {
			AdcCommand cmd = SearchManager::getInstance()->toPSR(false, ps.getMyNick(), hubIpPort, tth, outPartialInfo);
			ClientManager::getInstance()->send(cmd, from->getCID(), false, true);
		} catch(...) {
			dcdebug("Partial search caught error\n");
		}
	}

}

void SearchManager::respondDirect(const AdcCommand& aCmd, const CID& from, bool isUdpActive, const string& hubIpPort, ProfileToken aProfile) {
	AdcSearch sch(aCmd.getParameters());

	string directory;
	aCmd.getParam("PA", 0, directory);

	DirectSearchResultList results;

	try {
		ShareManager::getInstance()->directSearch(results, sch, 30, aProfile, directory);
	} catch(...) {
		//ignore
	}

	string token;
	aCmd.getParam("TO", 0, token);

	for(auto i = results.begin(); i != results.end(); ++i) {
		(*i)->setToken(token);
		AdcCommand cmd = (*i)->toDSR(AdcCommand::TYPE_UDP);
		ClientManager::getInstance()->send(cmd, from);
	}


	AdcCommand cmd(AdcCommand::CMD_DSR, AdcCommand::TYPE_UDP);
	cmd.addParam("ED1");
	cmd.addParam("TO", token);
	ClientManager::getInstance()->send(cmd, from);
}

void SearchManager::respond(const AdcCommand& adc, const CID& from, bool isUdpActive, const string& hubIpPort, ProfileToken aProfile) {
	// Filter own searches
	if(from == ClientManager::getInstance()->getMe()->getCID())
		return;

	UserPtr p = ClientManager::getInstance()->findUser(from);
	if(!p)
		return;

	SearchResultList results;
	ShareManager::getInstance()->search(results, adc.getParameters(), isUdpActive ? 10 : 5, aProfile, from);

	string token;

	adc.getParam("TO", 0, token);

	// TODO: don't send replies to passive users
	if(results.empty() && SETTING(USE_PARTIAL_SHARING)) {
		string tth;
		if(!adc.getParam("TR", 0, tth))
			return;
			
		PartsInfo partialInfo;
		string bundle;
		bool reply = false, add = false;
		QueueManager::getInstance()->handlePartialSearch(p, TTHValue(tth), partialInfo, bundle, reply, add);

		if (!partialInfo.empty()) {
			//LogManager::getInstance()->message("SEARCH RESPOND: PARTIALINFO NOT EMPTY");
			AdcCommand cmd = toPSR(true, Util::emptyString, hubIpPort, tth, partialInfo);
			ClientManager::getInstance()->send(cmd, from, false, true);
		}
		
		if (!bundle.empty()) {
			//LogManager::getInstance()->message("SEARCH RESPOND: BUNDLE NOT EMPTY");
			AdcCommand cmd = toPBD(hubIpPort, bundle, tth, reply, add);
			ClientManager::getInstance()->send(cmd, from, false, true);
		}

		return;
	}

	string key;
	adc.getParam("KY", 0, key);

	for(auto i = results.begin(); i != results.end(); ++i) {
		AdcCommand cmd = (*i)->toRES(AdcCommand::TYPE_UDP);
		if(!token.empty())
			cmd.addParam("TO", token);
		ClientManager::getInstance()->send(cmd, from, false, false, key);
	}
}

string SearchManager::getPartsString(const PartsInfo& partsInfo) const {
	string ret;

	for(auto i = partsInfo.begin(); i < partsInfo.end(); i+=2){
		ret += Util::toString(*i) + "," + Util::toString(*(i+1)) + ",";
	}

	return ret.substr(0, ret.size()-1);
}


AdcCommand SearchManager::toPSR(bool wantResponse, const string& myNick, const string& hubIpPort, const string& tth, const vector<uint16_t>& partialInfo) const {
	AdcCommand cmd(AdcCommand::CMD_PSR, AdcCommand::TYPE_UDP);
		
	if(!myNick.empty())
		cmd.addParam("NI", Text::utf8ToAcp(myNick));
		
	cmd.addParam("HI", hubIpPort);
	cmd.addParam("U4", (wantResponse && ClientManager::getInstance()->isActive(hubIpPort)) ? getPort() : "0");
	cmd.addParam("TR", tth);
	cmd.addParam("PC", Util::toString(partialInfo.size() / 2));
	cmd.addParam("PI", getPartsString(partialInfo));
	
	return cmd;
}

AdcCommand SearchManager::toPBD(const string& hubIpPort, const string& bundle, const string& aTTH, bool reply, bool add, bool notify) const {
	AdcCommand cmd(AdcCommand::CMD_PBD, AdcCommand::TYPE_UDP);

	cmd.addParam("HI", hubIpPort);
	cmd.addParam("BU", bundle);
	cmd.addParam("TH", aTTH);
	if (notify) {
		cmd.addParam("NO1");
	} else if (reply) {
		cmd.addParam("RE1");
	}

	if (add) {
		cmd.addParam("AD1");
	}
	return cmd;
}

void SearchManager::validateSearchTypeName(const string& name) const {
	if(name.empty() || (name.size() == 1 && name[0] >= '0' && name[0] <= '8')) {
		throw SearchTypeException("Invalid search type name"); // TODO: localize
	}
	for(int type = TYPE_ANY; type != TYPE_LAST; ++type) {
		if(getTypeStr(type) == name) {
			throw SearchTypeException("This search type already exists"); // TODO: localize
		}
	}
}

void SearchManager::setSearchTypeDefaults() {
	WLock l(cs);
	{
		searchTypes.clear();

		// for conveniency, the default search exts will be the same as the ones defined by SEGA.
		const auto& searchExts = AdcHub::getSearchExts();
		for(size_t i = 0, n = searchExts.size(); i < n; ++i)
			searchTypes[string(1, '1' + i)] = searchExts[i];
	}

	fire(SearchManagerListener::SearchTypesChanged());
}

void SearchManager::addSearchType(const string& name, const StringList& extensions, bool validated) {
	if(!validated) {
		validateSearchTypeName(name);
	}

	{
		WLock l(cs);
		if(searchTypes.find(name) != searchTypes.end()) {
			throw SearchTypeException("This search type already exists"); // TODO: localize
		}

		searchTypes[name] = extensions;
	}
	fire(SearchManagerListener::SearchTypesChanged());
}

void SearchManager::delSearchType(const string& name) {
	validateSearchTypeName(name);
	{
		WLock l(cs);
		searchTypes.erase(name);
	}
	fire(SearchManagerListener::SearchTypesChanged());
}

void SearchManager::renameSearchType(const string& oldName, const string& newName) {
	validateSearchTypeName(newName);
	StringList exts = getSearchType(oldName)->second;
	addSearchType(newName, exts, true);
	{
		WLock l(cs);
		searchTypes.erase(oldName);
	}
	fire(SearchManagerListener::SearchTypeRenamed(), oldName, newName);
}

void SearchManager::modSearchType(const string& name, const StringList& extensions) {
	{
		WLock l(cs);
		getSearchType(name)->second = extensions;
	}
	fire(SearchManagerListener::SearchTypesChanged());
}

const StringList& SearchManager::getExtensions(const string& name) {
	return getSearchType(name)->second;
}

SearchManager::SearchTypesIter SearchManager::getSearchType(const string& name) {
	SearchTypesIter ret = searchTypes.find(name);
	if(ret == searchTypes.end()) {
		throw SearchTypeException("No such search type"); // TODO: localize
	}
	return ret;
}

void SearchManager::getSearchType(int pos, int& type, StringList& extList, string& name) {
	// Any, directory or TTH
	if (pos < 3) {
		if (pos == 0) {
			name = SEARCH_TYPE_ANY;
			type = SearchManager::TYPE_ANY;
		} else if (pos == 1) {
			name = SEARCH_TYPE_DIRECTORY;
			type = SearchManager::TYPE_DIRECTORY;
		} else if (pos == 2) {
			name = SEARCH_TYPE_TTH;
			type = SearchManager::TYPE_TTH;
		}
		return;
	}
	pos = pos-3;

	int counter = 0;
	for(auto i = searchTypes.begin(); i != searchTypes.end(); ++i) {
		if (counter++ == pos) {
			if(i->first.size() > 1 || i->first[0] < '1' || i->first[0] > '6') {
				// custom search type
				type = SearchManager::TYPE_ANY;
			} else {
				type = i->first[0] - '0';
			}
			name = i->first;
			extList = i->second;
			return;
		}
	}

	throw SearchTypeException("No such search type"); 
}

void SearchManager::getSearchType(const string& aName, int& type, StringList& extList, bool lock) {
	if (aName.empty())
		throw SearchTypeException("No such search type"); 

	// Any, directory or TTH
	if (aName[0] == SEARCH_TYPE_ANY[0] || aName[0] == SEARCH_TYPE_DIRECTORY[0] || aName[0] == SEARCH_TYPE_TTH[0]) {
		type = aName[0] - '0';
		return;
	}

	if (lock) {
		cs.lock_shared();
	}
	ScopedFunctor([&] { if (lock) cs.unlock_shared(); });

	auto p = searchTypes.find(aName);
	if (p != searchTypes.end()) {
		extList = p->second;
		if(aName[0] < '1' || aName[0] > '6') {
			// custom search type
			type = SearchManager::TYPE_ANY;
		} else {
			type = aName[0] - '0';
		}
		return;
	}

	throw SearchTypeException("No such search type"); 
}


void SearchManager::on(SettingsManagerListener::Save, SimpleXML& xml) noexcept {
	xml.addTag("SearchTypes");
	xml.stepIn();
	{
		for(auto i = searchTypes.begin(); i != searchTypes.end(); ++i) {
			xml.addTag("SearchType", Util::toString(";", i->second));
			xml.addChildAttrib("Id", i->first);
		}
	}
	xml.stepOut();
}

void SearchManager::on(SettingsManagerListener::Load, SimpleXML& xml) {
	xml.resetCurrentChild();
	if(xml.findChild("SearchTypes")) {
		searchTypes.clear();
		xml.stepIn();
		while(xml.findChild("SearchType")) {
			const string& extensions = xml.getChildData();
			if(extensions.empty()) {
				continue;
			}
			const string& name = xml.getChildAttrib("Id");
			if(name.empty()) {
				continue;
			}
			searchTypes[name] = StringTokenizer<string>(extensions, ';').getTokens();
		}
		xml.stepOut();
	}
}

} // namespace dcpp