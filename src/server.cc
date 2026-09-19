/**
 * server.cc
 * This is the implementation file for the xServer class.
 * This class is the entity which is the GNUWorld server
 * proper.  It manages network I/O, parsing and distributing
 * incoming messages, notifying attached clients of
 * system events, on, and on, and on.
 *
 * Copyright (C) 2002 Daniel Karrels <dan@karrels.com>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111-1307,
 * USA.
 *
 * $Id: server.cc,v 1.226 2010/08/31 21:16:46 denspike Exp $
 */

#include <sys/time.h>
#include <unistd.h>

#include <new>
#include <exception>
#include <memory>
#include <string>
#include <optional>
#include <charconv>
#include <string_view>
#include <span>
#include <map>
#include <list>
#include <vector>
#include <algorithm>
#include <sstream>
#include <fstream>
#include <stack>
#include <iostream>
#include <utility>

#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <cassert>
#include <cerrno>
#include <csignal>
#include <cstdarg>

#include "gnuworld_config.h"
#include "misc.h"
#include "events.h"
#include "ip.h"

#include "server.h"
#include "Network.h"
#include "iServer.h"
#include "iClient.h"
#include "EConfig.h"
#include "match.h"
#include "IrcLogSink.h"
#include "LogConfig.h"
#include "LogManager.h"
#include "LogSink.h"
#include "LogSinks.h"
#include "logger.h"
#include "pushover.h"
#include "StringTokenizer.h"
#include "xparameters.h"
#include "moduleLoader.h"
#include "ServerTimerHandlers.h"
#include "LoadClientTimerHandler.h"
#include "UnloadClientTimerHandler.h"
#include "ConnectionManager.h"
#include "ConnectionHandler.h"
#include "Connection.h"

/* The logger this file writes to: the server itself.  Core is one binary and
 * not one module, so this stands once per .cc file rather than in a header */
GNUWORLD_CORE_LOGGER(Core);

namespace gnuworld {

using std::clog;
using std::cout;
using std::endl;
using std::list;
using std::make_pair;
using std::min;
using std::pair;
using std::stack;
using std::string;
using std::stringstream;
using std::vector;

/// The object containing the network data structures
xNetwork* Network = 0;

// Allocate the static std::string in xServer representing
// all channels.
const string xServer::CHANNEL_ALL("*");

void xServer::initializeSystem() {
    initializeVariables();

    clog << "*** Parsing configuration file " << configFileName << "..." << endl;
    if (!readConfigFile(configFileName)) {
        LOG(FATAL, "Error reading config file: {}", configFileName);
        ::exit(-1);
    }

    // Output the information to the console.
    LOG(INFO, "Numeric: {} ({})", getIntYY(), getCharYY());
    LOG(INFO, "Max Clients: {} ({})", getIntXXX(), getCharXXX());
    LOG(INFO, "Uplink Name: {}", UplinkName);
    LOG(INFO, "Uplink Port: {}", Port);
    LOG(INFO, "Server Name: {}", ServerName);
    LOG(INFO, "Server Description: {}", ServerDescription);

    // elog	<< "xServer::charYY> " << getCharYY() << endl ;
    // elog	<< "xServer::charXXX> " << getCharXXX() << endl ;
    // elog	<< "xServer::intYY> " << getIntYY() << endl ;
    // elog	<< "xServer::intXXX> " << getIntXXX() << endl ;

    me = new (std::nothrow) iServer(getIntYYXXX(), getCharYYXXX(), ServerName, ::time(0));
    assert(me != 0);

    if (!Network->addServer(me)) {
        LOG(FATAL, "Failed to add (me) to the system tables");
        ::exit(-1);
    }

    Network->setServer(this);

    if (!loadCommandHandlers()) {
        LOG_CORE(Modules, FATAL, "Failed to load command handlers");
        ::exit(-1);
    }

    if (!loadClients(configFileName)) {
        LOG_CORE(Modules, FATAL, "Failed in loading one or more modules");
        ::exit(-1);
    }

    registerServerTimers();
}

/**
 * Deallocate this xServer instance.
 */
xServer::~xServer() {
    // A channel sink of the logging configuration outlives this server: it must
    // not write through it any more
    IrcLogSink::forgetServer(this);

    // All deallocations are performed in doShutdown()
    if (logSocket) {
        socketFile.close();
    }
} // ~xServer()

void xServer::initializeVariables() {
    // Initialize more variables
    keepRunning = true;
    bursting = false;
    sendEA = true;
    sendEB = true;
    lastLoop = false;
    useHoldBuffer = false;
    autoConnect = false;
    StartTime = ::time(NULL);
    shutDownReason = string("Server Shutdown");

    serverConnection = 0;
    burstStart = burstEnd = 0;
    Uplink = NULL;
    me = NULL;
    lastTimerID = 1;
    glineUpdateInterval = pingUpdateInterval = 0;

    Network = new (std::nothrow) xNetwork;
    assert(Network != 0);
}

bool xServer::readConfigFile(const string& fileName) {
    // Read the configuration file and set our
    // data members.
    EConfig conf(fileName);

    // Parse out the required data fields
    UplinkName = conf.Require("uplink")->second;
    ServerName = conf.Require("name")->second;
    ServerDescription = conf.Require("description")->second;
    Password = conf.Require("password")->second;
    Port = atoi(conf.Require("port")->second.c_str());
    setIntYY(atoi(conf.Require("numeric")->second.c_str()));
    setIntXXX(atoi(conf.Require("maxclients")->second.c_str()));
    commandMapFileName = conf.Require("command_map")->second;

    // autoConnect initialized to false
    string strAutoConnect = conf.Require("auto_reconnect")->second;
    if ((strAutoConnect == "yes") || (strAutoConnect == "true")) {
        autoConnect = true;
    }

    iClient::setHiddenHostSuffix(conf.Require("hidden_host_suffix")->second);

    libPrefix = conf.Require("libdir")->second;
    if (libPrefix[libPrefix.size() - 1] != '/') {
        libPrefix += '/';
    }

    // Load the control nickname(s), if any
    EConfig::const_iterator cnItr = conf.Find("controlnick");
    for (; (cnItr != conf.end()) && (cnItr->first == "controlnick"); ++cnItr) {
        //	elog	<< "xServer> Adding control nickname: "
        //		<< cnItr->second
        //		<< endl ;
        controlNickSet.insert(cnItr->second);
    }

    // Load the control access list, AC usernames, if any
    EConfig::const_iterator acItr = conf.Find("allowcontrol");
    for (; (acItr != conf.end()) && (acItr->first == "allowcontrol"); ++acItr) {
        //	elog	<< "xServer> Adding control authorization for username: "
        //		<< acItr->second
        //		<< endl ;

        allowControlSet.insert(acItr->second);
    }

    glineUpdateInterval =
        static_cast<time_t>(atoi(conf.Require("glineupdateinterval")->second.c_str()));
    pingUpdateInterval =
        static_cast<time_t>(atoi(conf.Require("pingupdateinterval")->second.c_str()));

// Check TLS configuration settings
#ifdef HAVE_LIBSSL
    EConfig::const_iterator tlsItr = conf.Find("tls");
    if (tlsItr == conf.end() || tlsItr->second != "yes") {
        tlsEnabled = false;
    } else {
        tlsEnabled = true;

        // If TLS is enabled, parse the other configuration options
        tlsKeyFile = conf.Require("tlsKeyFile")->second;
        tlsCertFile = conf.Require("tlsCertFile")->second;

        if (!initTls()) {
            LOG(FATAL, "TLS initialization error. Exiting.");
            ::exit(1);
        }
    }
#endif
    return true;
}

/**
 * This function is only called if TLS is enabled in config. We exit if gnuworld
 * is not compiled with TLS support.
 */
#ifdef HAVE_LIBSSL
bool xServer::initTls() {
    LOG(INFO, "Spinning up TLS");
    SSL_library_init();
    SSL_load_error_strings();
    sslCtx = SSL_CTX_new(TLS_method());

    SSL_CTX_set_min_proto_version(sslCtx, TLS1_2_VERSION); // Allow TLS 1.2 and above
    SSL_CTX_set_max_proto_version(sslCtx, TLS1_3_VERSION); // Allow up to TLS 1.3
    SSL_CTX_set_verify(sslCtx, SSL_VERIFY_NONE, NULL);

    int res = SSL_CTX_use_certificate_chain_file(sslCtx, tlsCertFile.c_str());
    if (res != 1) {
        LOG(ERROR, "Could not load certificate file");
        SSL_CTX_free(sslCtx);
        return false;
    }

    res = SSL_CTX_use_PrivateKey_file(sslCtx, tlsKeyFile.c_str(), SSL_FILETYPE_PEM);
    if (res != 1) {
        LOG(ERROR, "Could not load key file");
        SSL_CTX_free(sslCtx);
        return false;
    }

    res = SSL_CTX_check_private_key(sslCtx);
    if (res != 1) {
        LOG(ERROR, "Private key validation failed");
        SSL_CTX_free(sslCtx);
        return false;
    }

    SSL_CTX_set_cipher_list(sslCtx, SSL_DEFAULT_CIPHER_LIST);
    return true;
}
#endif

bool xServer::loadCommandHandlers() {
    std::ifstream commandMapFile(commandMapFileName.c_str());
    if (!commandMapFile) {
        LOG_CORE(Modules, ERROR, "Unable to open command map file: {}", commandMapFileName);
        return false;
    }

    string line;
    size_t lineNumber = 0;
    bool returnVal = true;

    while (std::getline(commandMapFile, line)) {
        ++lineNumber;

        if (line.empty() || '#' == line[0]) {
            continue;
        }

        // module_file_name module_loader_symbol command_key
        StringTokenizer st(line);
        if (st.size() != 3) {
            LOG_CORE(Modules, ERROR, "{}:{}> Invalid syntax, 3 tokens expected, {} tokens found",
                     commandMapFileName, lineNumber, st.size());
            returnVal = false;
            break;
        }

        // st[ 0 ] is the module file name
        // st[ 1 ] is the symbol name to lookup, minus the preceeding
        // _gnuwinit_
        // st[ 2 ] is the command key to which the handler will
        //  be registered

        // Let's make sure that the filename is correct
        string fileName(st[0]);

        // We need the entire path to the command handler in the
        // fileName.
        if (string::npos == fileName.find(libPrefix)) {
            // Need to put the command handler path prefix in
            // the filename
            // libPrefix has a trailing '/'
            fileName = libPrefix + fileName;
        }

        // All module names end with ".la" for libtool libraries
        if (string::npos == fileName.find(".la")) {
            // Need to append ".la" to fileName
            fileName += ".la";
        }

        if (!loadCommandHandler(fileName, st[1], st[2])) {
            LOG_CORE(
                Modules, ERROR,
                "Failed to load handler for message token {}, from module file: {}, with symbol "
                "suffix: {}",
                st[2], fileName, st[1]);
            returnVal = false;
            break;
        }

        //	elog	<< "xServer::loadCommandHandlers> Loaded handler for "
        //		<< st[ 2 ]
        //		<< endl ;

    } // while()

    LOG_CORE(Modules, INFO, "Loaded {} command handlers", commandMap.size());

    commandMapFile.close();

    return returnVal;
}

bool xServer::loadCommandHandler(const string& fileName, const string& symbolName,
                                 const string& commandKey) {
    // Let's first check to see if the module is already open
    // It is possible that a single module handler may be
    // registered to handle multiple commands (NOOP for example)
    bool foundExistingModule = true;
    commandModuleType* ml = lookupCommandModule(fileName);
    if (NULL == ml) {
        foundExistingModule = false;
        ml = new (std::nothrow) commandModuleType(fileName);
        assert(ml != 0);
    } else {
        //	elog	<< "xServer::loadCommandHandler> Found existing module: "
        //		<< ml->getModuleName()
        //		<< endl ;
    }

    string symbolSuffix = string("_") + symbolName;
    // elog	<< "xServer::loadCommandHandler> fileName: "
    //	<< fileName
    //	<< ", symbolSuffix: "
    //	<< symbolSuffix
    //	<< ", commandKey: "
    //	<< commandKey
    //	<< endl ;

    ServerCommandHandler* sch = ml->loadObject(this, symbolSuffix);
    if (NULL == sch) {
        LOG_CORE(Modules, ERROR,
                 "Failed to load handler for message token {}, from module file: {}, with symbol "
                 "suffix: {}",
                 commandKey, fileName, symbolName);

        delete (ml);
        ml = 0;

        return false;
    }

    // Successfully loaded a module
    // Put it in the list of modules
    if (!foundExistingModule) {
        // No sense in adding the same module multiple times
        commandModuleList.push_back(ml);
    }

    // Add the command handler to the handler map
    if (!commandMap.insert(commandMapType::value_type(commandKey, sch)).second) {
        LOG_CORE(Modules, ERROR, "Unable to add handler for message {} to commandMap", commandKey);

        delete ml;
        ml = 0;
        delete sch;
        sch = 0;

        return false;
    }

    return true;
}

xServer::commandModuleType* xServer::lookupCommandModule(const string& moduleName) const {
    for (commandModuleListType::const_iterator ptr = commandModuleList.begin();
         ptr != commandModuleList.end(); ++ptr) {
        if (!strcasecmp((*ptr)->getModuleName(), moduleName)) {
            // Found the module
            return *ptr;
        }
    }
    return 0;
}

bool xServer::loadClients(const string& fileName) {
    // Load the config file
    EConfig conf(fileName);

    /*
     * Load and attach any modules specified in the config.
     */
    EConfig::const_iterator ptr = conf.Find("module");
    for (; ptr != conf.end() && ptr->first == "module"; ++ptr) {
        StringTokenizer modInfo(ptr->second);

        if (2 != modInfo.size()) {
            LOG_CORE(Modules, ERROR,
                     "modules require two arguments, modulename followed by config file name");

            return false;
        }

        //	elog	<< "xServer::loadClients> Found module: "
        //		<< modInfo[0]
        //		<< " (Config: "
        //		<< modInfo[1]
        //		<< ")"
        //		<< endl;

        string fileName = modInfo[0];
        if ('/' != fileName[0]) {
            // Relative path, prepend the libPrefix to the fileName
            // libPrefix is guaranteed to end with '/'
            fileName = libPrefix + modInfo[0];
        }

        // The AttachClient method will load the client from the
        // file.
        if (!AttachClient(fileName, modInfo[1])) {
            // No need for error output here because AttachClient()
            // will do that for us
            LOG_CORE(Modules, ERROR, "Failed to attach client: {}", fileName);

            return false;
        }
    }

    return true;
}

/**
 * Initiate a server shutdown.
 */
void xServer::Shutdown(const string& reason) {
    lastLoop = true;
    autoConnect = false;

    // Set the shutdown reason before notifying clients, so each
    // client can access the reason
    setShutDownReason(reason);

    // Notify all xClients that a shutdown is being processed
    for (xNetwork::localClientIterator itr = Network->localClient_begin();
         itr != Network->localClient_end(); ++itr) {
        itr->second->OnShutdown(reason);
    }

    // Can't call removeClients() here because it is likely one of the
    // clients that has invoked this call, that would be bad.
    // Instead, the code that will halt the system is located in the
    // main for() loop, which simply calls doShutdown() (private method).
}

// This function parses and distributes incoming lines
// of data
void xServer::Process(char* s) {
    if ((nullptr == s) || (0 == s[0]) || (' ' == s[0])) {
        return;
    }

    // Tokenizing cuts the buffer up
    currentLine = s;

    // IRCv3 message-tags: optional "@key=value;key2=value2 " prefix.
    // Strip and parse them before the classic sender/command tokenization
    // so existing handlers see an unchanged argument layout.
    xParameters::tagListType messageTags;
    if ('@' == s[0]) {
        ++s; // skip '@'
        char* tagStart = s;
        while (*s && (' ' != *s)) {
            ++s;
        }
        if (!*s) {
            // Tags with no following command
            return;
        }
        *s++ = 0;
        while (*s && (' ' == *s)) {
            ++s;
        }
        if (!*s) {
            return;
        }
        xParameters::parseTags(tagStart, messageTags);
    }

    // isNumeric indicates whether or not the first
    // argument in the (s) array is a numeric
    // n2k complicates things in this way...
    bool isNumeric = true;
    char YXX[10] = {0};
    char *Command = NULL, *Sender = NULL;

    // Check for incoming numeric
    if (s[0] != ':') {

        // The first token is a numeric (at most 5 characters) or a short
        // bare command such as SERVER.  It comes straight off the wire, so
        // never copy more than fits: a longer one cannot be valid.
        char* yxxPtr = YXX;
        const char* const yxxEnd = YXX + sizeof(YXX) - 1;
        while (*s && (' ' != *s)) {
            if (yxxPtr == yxxEnd) {
                LOG(WARN, "Dropping line with oversized first token");
                return;
            }
            *yxxPtr++ = *s++;
        }

        // s now points to ' ' or is no longer valid
        // Increment past any white space, assigning
        // each as a NULL terminator as we pass.
        while (*s && (' ' == *s)) {
            *s++ = 0;
        }

        // s now points to a non white space character,
        // or is not valid
        if (!s || !(*s)) {
            return;
        }

        // Extract the command
        Command = strtok(s, " ");

        // According to my calculations, all
        // non-numeric commands that do not
        // begin with ':' have ':' as the
        // beginning of the second argument,
        // Im probably wrong though.
        //
        // Example:
        // ERROR :blah blah
        // PASS :blah
        //
        // Exception:
        // SERVER message, sent once, whose args do not
        // begin with ':'
        //
        if ((Command && ':' == *Command) || !strncmp(YXX, "SERV", 4)) {
            // It's not a command, the YXX
            // is not a numeric
            isNumeric = false;
        }

        // Point Sender to the numeric of the sender,
        // whether it be server or client
        Sender = YXX;

    }

    // :ripper.ufl.edu 442 EuWorld3 #nowhere :You're not on that channel
    else
    // s[ 0 ] == ':'
    {
        // Sender will be the first argument, likely
        // the server from which this command
        // originates
        Sender = strtok(s, " ");

        // Skip over the ':'
        ++Sender;

        // Extract the command
        Command = strtok(NULL, " ");
    }

    if (NULL == Sender) {
        LOG(WARN, "NULL == Sender... *shrug*");
        Command = strtok(s, " ");
    }

    // If commands like ERROR and PASS slipped
    // into the first if structure above (as
    // they should), then the Command will be bogus.
    // Let's adjust the Command parameter.
    if (!isNumeric) {
        // This is a bit of a hack
        // Sender right now points to YXX
        // Redirect it to point to the first
        // argument, so it will be the first
        // token put into the xParameters object
        // below.
        Sender = Command;

        // Command becomes the original YXX
        Command = YXX;
    }

    // elog << "Sender: " << Sender << endl ;
    // elog << "Command: " << Command << endl ;
    // elog << "strlen( Command ): " << strlen( Command ) << endl ;
    // elog << "isNumeric: " << isNumeric << endl ;

    // Lookup the handler for this command
    commandMapType::iterator pairPtr = commandMap.find(Command);
    if (pairPtr != commandMap.end()) {
        // Found a command handler for this
        // command.
        // Prepare the arguments for the command.

        char* x = nullptr;
        xParameters Param;
        if (!messageTags.empty()) {
            Param.setTags(std::move(messageTags));
        }

        // Was this command was sent from a server
        // or client?
        if (Sender) {
            Param << Sender;
        }

        // Extract the argument list
        x = strtok(NULL, "\n");

        // Continue while there are characters
        // yet to parse
        while (x && *x) {

            // Set to NULL any space characters
            while (*x == ' ') {
                *x++ = 0;
            }

            // Have we reached the end of the line?
            if (*x == 0) {
                break;
            }

            // x now points to a non-whitespace
            // character

            // Some arguments to commands are preceeded
            // by ':', skip it, but only if the preceeding
            // character was a white space (now NULL)
            // character.
            if (*x == ':' && *(x - 1) == 0) {

                // Skip the ':'
                x++;

                // Put the rest of the string into
                // the xParameters instance
                // -> It is an argument string
                Param << x;

                // We have reached the end of the
                // line, it has just been put into
                // the end of the xParameters instance
                // as a whole.
                break;
            }

            // Add this argument to the xParameters
            // instance.
            Param << x;

            // Skip the token just added to the xParameters.
            while (*x && *x != ' ') {
                x++;
            }

        } // close while

        // Arguments are set.
        // Go ahead and call the handler method
        if (!pairPtr->second->Execute(Param)) {
            //		elog	<< "xServer::Process> Handler failed for command: "
            //			<< Command
            //			<< ", args: "
            //			<< Param
            //			<< endl ;
        }

    } else {
        LOG(WARN, "Unable to find handler for: {}", nullptr == Command ? "" : Command);
    }
}

/**
 * Squit another server as a server.
 * 0 SQ server.name.com timestamp :reason
 */
bool xServer::SquitServer(const string& serverName, const string& reason) {
    // Is it our server?
    if (!strcasecmp(serverName, this->ServerName)) {
        // I don't see that happening
        LOG(WARN, "Attempt to squit myself!");
        return false;
    }

    // elog	<< "xServer::SquitServer> Searching for server "
    //	<< serverName
    //	<< endl ;

    /* Post Event
     * (Added Sept.26th 2011 by Hidden - Should fix a lag report bug in mod.ccontrol when a JUPE is
     * issued)
     */
    iServer* tmpServer = Network->findServerName(serverName);
    if (NULL == tmpServer) {
        // The server doesn't exist.
        LOG(WARN, "Unable to find server: {}", serverName);
        return false;
    }
    string source(getCharYY());
    string nreason(reason);
    PostEvent(EVT_NETBREAK, static_cast<void*>(tmpServer), static_cast<void*>(&source),
              static_cast<void*>(&nreason));

    iServer* theServer = Network->removeServer(tmpServer->getIntYY(), true);
    if (NULL == theServer) {
        // The server doesn't exist.
        LOG(WARN, "Unable to find server: {}", serverName);
        return false;
    }

    // Prepare the output buffer that will squit the server.
    stringstream s;
    s << getCharYY() << " SQ " << serverName << ' ' << theServer->getStartTime() << " :" << reason;

    // Notify the rest of the network of the SQUIT.
    Write(s);

    // Deallocate the memory it occupies.
    delete theServer;
    theServer = 0;

    // TODO: Log event
    // TODO: Post message

    // Squit successful.
    return true;
}

/**
 * Attach a server.  This could be either a jupe, or some fictitious
 * server from which to host virtual clients.
 */
bool xServer::AttachServer(iServer* fakeServer, xClient* owningClient) {
    assert(owningClient != 0);
    assert(fakeServer != NULL);

    // Make sure a server of the same name is not already connected.
    iServer* existingServer = Network->findServerName(fakeServer->getName());
    if (existingServer != NULL) {
        // The server is already on the network.
        // Steal it's numeric :)
        fakeServer->setIntYY(existingServer->getIntYY());

        // Squit the old server and remove it from the internal tables.
        // This will also remove the server if it is already juped.
        SquitServer(existingServer->getName(), fakeServer->getDescription());

        // SquitServer() will also deallocate the server.
        // Make sure not to attempt to use the bogus tmp pointer.
        existingServer = 0;
    }

    if (!Network->addFakeServer(fakeServer, owningClient)) {
        LOG_MSG(ERROR, "Failed to attach fake server: {server}").with("server", fakeServer).log();
        return false;
    }

    // Set the intXXX/charXXX to the max possible
    fakeServer->setIntXXX(64 * 64 * 64 - 1);

    BurstServer(fakeServer);

    PostEvent(EVT_NETJOIN, static_cast<void*>(fakeServer));

    return true;
}

void xServer::BurstServer(iServer* fakeServer) {
    assert(fakeServer != 0);

    if (fakeServer->isJupe()) {
        // source_numeric JU +servername * expiration_time lastmod :reason
        // expiration: 604800 (max)
        Write("{} JU * +{} 604800 {} :{}", getCharYY(), fakeServer->getName(), ::time(0),
              fakeServer->getDescription());
    } else {
        // Burst the new server's info./
        // IRCu checks for "JUPE " as being the beginning of the
        // reason as a jupe server.  This was because before servers
        // couldn't link without [ip] being added to their realname
        // field unless they were juped by uworld.  Now anyone can
        // link with that name, oh well.
        // <YY> S <name> <hops> 0 <link-ts> J<protocol> <YYXXX> +<flags> :<description>
        Write("{} S {} {} {} {} J{:02} {} + :{}\n", getCharYY(), fakeServer->getName(), 2, 0,
              fakeServer->getConnectTime(),
              Version, // a server of ours speaks what we speak; this was a literal 10
              fakeServer->getCharYYXXX(), fakeServer->getDescription());
        fakeServer->setProtocol(static_cast<unsigned int>(Version));

        // Write burst acknowledgements.
        Write("{} EB\n", fakeServer->getCharYY());
        Write("{} EA\n", fakeServer->getCharYY());
    }
}

/**
 * Attach an xClient to the server.
 * If this method fails, the xClient pointer passed to it is
 * returned to its state when the method was called.
 */
bool xServer::AttachClient(xClient* Client, bool doBurst) {
    // Make sure the pointer is valid.
    assert(NULL != Client);

    // The xClient will be attached to this server.
    Client->setIntYY(getIntYY());

    // addClient() will allocate a new XXX and
    // update Client.
    if (!Network->addClient(Client)) {
        LOG_CORE(Modules, ERROR, "Failed to update network tables");
        return false;
    }

    Client->MyUplink = this;

    // Let the client know it has been added to
    // the server and its tables.
    Client->OnAttach();

    // Stealth modules stay in localClients for nick@server routing but
    // never get an iClient or a network N burst.
    if (Client->IsStealth()) {
        if (doBurst) {
            Client->OnConnect();
        }

        LOG_CORE(Modules, INFO, "Loaded stealth client, nickname: {}, with config file: {}",
                 Client->getNickName(), Client->getConfigFileName());
        return true;
    }

    // TODO: Remove any existing iClient from the xClient

    // Create a new iClient representation for this xClient
    iClient* theIClient = new (std::nothrow)
        iClient(getIntYY(), Client->getCharYYXXX(), Client->getNickName(), Client->getUserName(),
                "AAAAAA", Client->getHostName(), Client->getHostName(), Client->getModes(),
                string(), 0, 0, string(), string(), string(), Client->getDescription(), ::time(0));
    assert(theIClient != 0);

    // Notify the xClient of its iClient instance
    Client->setInstance(theIClient);

    // Add the iClient to the network tables
    if (!Network->addClient(theIClient)) {
        // Failed to add the iClient to the network tables
        LOG_CORE(Modules, ERROR, "Unable to add theIClient to the Network table");

        // We have already reserved a numeric for this client,
        // go ahead and remove it
        Network->removeLocalClient(Client);

        // Do some cleanup
        delete theIClient;
        theIClient = 0;
        Client->resetInstance();

        // Failed to complete the procedure
        return false;
    }

    PostEvent(EVT_NICK, static_cast<void*>(theIClient));

    if (doBurst) {
        BurstClient(Client);
        Client->BurstChannels();
        Client->BurstGlines();
    }

    LOG_MSG_CORE(Modules, INFO, "Loaded client, nickname: {client}, with config file: {}",
                 Client->getConfigFileName())
        .with("client", theIClient)
        .log();

    // Success
    return true;
}

bool xServer::AttachClient(const string& moduleName, const string& configFileName, bool doBurst) {
    // Create a moduleLoader instance, based on the given moduleName
    moduleLoader<xClient*>* ml = new (std::nothrow) moduleLoader<xClient*>(moduleName);
    assert(ml != 0);
    xClient* clientPtr = NULL;

    // The module's logger is named after its library file, and the xClient the
    // module creates picks the name up from here
    LogManager::setLoadingModule(LogManager::moduleNameFromLibrary(moduleName));

    try {
        // Attempt to instantiate an xClient instance from the module
        clientPtr = ml->loadObject(configFileName);
    } catch (...) {
    }

    // Whether the module took the name or not, it is not the next one's
    LogManager::takeLoadingModule();

    // Check if the object was loaded successfully
    if (NULL == clientPtr) {
        // Failed to load the object
        LOG_CORE(Modules, ERROR, "Failed to instantiate module: {}", moduleName);

        // Deallocate the module, this will also close the module file
        delete ml;
        ml = 0;

        // Return failure
        return false;
    }

    // Attempt to attach the client to the server
    if (!AttachClient(clientPtr, doBurst)) {
        // Failed to attach the client
        LOG_CORE(Modules, ERROR, "Failed to attach new xClient: {}", moduleName);

        // Deallocate the client and its encapsulating module
        delete clientPtr;
        clientPtr = 0;
        delete ml;
        ml = 0;

        // Return failure
        return false;
    }

    // The client module is successfully loaded from the file, and
    // has been successfully attached to the server

    // Add moduleLoader to the clientModuleList
    clientModuleList.push_back(ml);

    // success
    return true;
}

/**
 * Attach an iClient to a juped server.
 *
 * AQ N ripper_ 1 952038834 ~dan 127.0.0.1 +owg B]AAAB AQAAA :Dan Karrels
 */
bool xServer::AttachClient(iClient* fakeClient, xClient* ownerClient) {
    assert(fakeClient != NULL);
    assert(ownerClient != 0);

    // Verify that the iClient is in good order
    if (fakeClient->getNickName().empty() || fakeClient->getUserName().empty() ||
        fakeClient->getInsecureHost().empty() || fakeClient->getDescription().empty()) {
        LOG_MSG_CORE(Modules, WARN, "Missing data in iClient: {client}")
            .with("client", fakeClient)
            .log();
        return false;
    }

    // Let the xNetwork class handle filling in the information about the
    // iClient.
    if (!Network->addFakeClient(fakeClient, ownerClient)) {
        LOG_CORE(Modules, ERROR, "addFakeClient() failed");
        return false;
    }

    // Burst the client
    BurstClient(fakeClient);

    return true;
}

void xServer::BurstClient(iClient* fakeClient) {
    iServer* fakeServer = me;
    int hopCount = 1;
    if (fakeClient->getIntYY() != getIntYY()) {
        hopCount = 2;
        fakeServer = Network->findFakeServer(fakeClient->getIntYY());
        assert(fakeServer != 0);
    }

    string description("Clone");
    if (!fakeClient->getDescription().empty()) {
        description = fakeClient->getDescription();
    }

    string accountString{};
    if (fakeClient->getMode(iClient::MODE_REGISTERED))
        accountString += " " + fakeClient->getAccount() + ":" +
                         std::to_string(fakeClient->getAccountID()) + ":" +
                         std::to_string(fakeClient->getAccountFlags());

    // <YY> N <nick> <hops> <nick-ts> <user> <host> <+modes>[ <account>] <base64-ip> <YYXXX>
    // :<description>
    Write("{} N {} {} {} {} {} {}{} {} {} :{}\n",
          fakeServer->getCharYY(),                // <YY>
          fakeClient->getNickName(),              // <nick>
          hopCount,                               // <hops>
          fakeClient->getNickTS(),                // <nick-ts>
          fakeClient->getUserName(),              // <user>
          fakeClient->getRealInsecureHost(),      // <host>
          fakeClient->getStringModes(),           // <+modes>
          accountString,                          // [ <account>], with its own leading space
          xIP(fakeClient->getIP()).GetBase64IP(), // <base64-ip>
          fakeClient->getCharYYXXX(),             // <YYXXX>
          description);

    PostEvent(EVT_NICK, static_cast<void*>(fakeClient));
}

/**
 * Detach an xClient from the xServer.
 */
bool xServer::DetachClient(xClient* Client, const string& reason) {
    if (NULL == Client) {
        return false;
    }

    // Notify the client that it is being detached.
    Client->OnDetach(reason);

    // removeClient() does all of the internal updates
    removeClient(Client);

    return true;
}

/**
 * Detach an xClient by moduleName
 */
bool xServer::DetachClient(const string& moduleName, const string& reason) {
    for (clientModuleListType::const_iterator ptr = clientModuleList.begin();
         ptr != clientModuleList.end(); ++ptr) {
        //	elog	<< "xServer::DetachClient> moduleName: "
        //		<< (*ptr)->getModuleName()
        //		<< endl ;

        if (!strcasecmp((*ptr)->getModuleName(), moduleName)) {
            // Found one
            return DetachClient((*ptr)->getObject(), reason);
        }
    }

    LOG_CORE(Modules, WARN, "Unable to find client moduleName: {}", moduleName);

    return false;
}

void xServer::LoadClient(const string& moduleName, const string& configFileName) {
    // elog	<< "xServer::LoadClient("
    //	<< moduleName
    //	<< ", "
    //	<< configFileName
    //	<< ")"
    //	<< endl ;

    string fileName = moduleName;
    if ('/' != fileName[0]) {
        // Relative path, prepend the libPrefix to the fileName
        // libPrefix is guaranteed to end with '/'
        fileName = libPrefix + moduleName;
    }

    // Next, queue the load request
    LoadClientTimerHandler* handler =
        new (std::nothrow) LoadClientTimerHandler(this, fileName, configFileName);
    assert(handler != 0);

    RegisterTimer(::time(0) + 2, handler, 0);
}

void xServer::UnloadClient(const string& moduleName, const string& reason) {
    // elog	<< "xServer::UnloadClient("
    //	<< moduleName
    //	<< ","
    //	<< reason
    //	<< ")> "
    //	<< moduleName
    //	<< endl ;

    UnloadClientTimerHandler* handler =
        new (std::nothrow) UnloadClientTimerHandler(this, moduleName, reason);
    assert(handler != 0);

    RegisterTimer(::time(0), handler, 0);
}

void xServer::UnloadClient(xClient* theClient, const string& reason) {
    // elog	<< "xServer::UnloadClient(xClient*)> "
    //	<< theClient->getNickName()
    //	<< endl ;

    for (clientModuleListType::const_iterator ptr = clientModuleList.begin();
         ptr != clientModuleList.end(); ++ptr) {
        if ((*ptr)->getObject() == theClient) {
            // Found one
            UnloadClient((*ptr)->getModuleName(), reason);
            return;
        }
    }

    LOG_CORE(Modules, WARN, "Unable to find client: {}", theClient->getNickName());
}

// This method is responsible for updating all internal
// tables and deallocating the given xClient
void xServer::removeClient(xClient* theClient) {
    // Precondition: theClient != 0

    // Remove this xClient's iClient instance (stealth modules have none)
    iClient* iClientPtr = 0;
    if (theClient->getInstance() != 0) {
        iClientPtr = Network->removeClient(theClient->getInstance());
        PostEvent(EVT_QUIT, static_cast<void*>(iClientPtr));
    }

    // Remove any fake clients and fake servers associated with this
    // xClient.
    list<iClient*> fakeClients = Network->findFakeClients(theClient);
    for (list<iClient*>::iterator cItr = fakeClients.begin(); cItr != fakeClients.end(); ++cItr) {
        iClient* fakeClient = *cItr;

        // Issue the quite message for this client
        stringstream s;
        s << fakeClient->getCharYYXXX() << " Q :Exiting";
        Write(s);

        PostEvent(EVT_QUIT, static_cast<void*>(fakeClient));

        // Remove the fake client from all internal tables and
        // deallocate.  xNetwork::removeClient() will do all but
        // the deallocation.
        delete Network->removeClient(fakeClient);
    } // for( cItr )

    // By this point, the xClient should have removed all of its
    // custom data from each iClient in the network.
    // Verify this.
    void* customData = 0;
    for (xNetwork::clientIterator clientItr = Network->clients_begin();
         clientItr != Network->clients_end(); ++clientItr) {
        customData = clientItr->second->removeCustomData(theClient);

        if (customData != 0) {
            //		elog	<< "xServer::removeClient> xClient "
            //			<< *theClient
            //			<< " forgot to remove customData for client "
            //			<< *(clientItr->second)
            //			<< endl ;
        }
    } // for()

    // Deallocate the iClient instance of the xClient
    delete iClientPtr;
    iClientPtr = 0;

    // Reset the iClient instance for good measure
    theClient->resetInstance();

    // Walk the channelEventMap, and remove the xClient from all
    // channel's in which it is registered.
    // This will include channel "*"
    for (channelEventMapType::iterator chPtr = channelEventMap.begin(),
                                       chEndPtr = channelEventMap.end();
         chPtr != chEndPtr; ++chPtr) {
        (*chPtr).second->remove(theClient);
    }

    // Remove this xClient from all other events
    for (eventListType::iterator evPtr = eventList.begin(), evEndPtr = eventList.end();
         evPtr != evEndPtr; ++evPtr) {
        (*evPtr).remove(theClient);
    }

    // Remove all remaining timers for this xClient
    removeAllTimers(theClient);

    // Close all Connections the xClient may have open/pending
    ConnectionManager::Disconnect(reinterpret_cast<ConnectionHandler*>(theClient), 0);

    // ConnectionManager::Poll() must be called to perform the action
    // that actually closes the above connections, but it is guaranteed
    // to be called eventually, even if in doShutdown().

    // Remove the client from the network data structures.
    // Be sure to do this before closing the client's module,
    // because closing the module will invalidate the client
    // pointer.
    Network->removeLocalClient(theClient);

    // Find this client in the module list
    for (clientModuleListType::iterator modPtr = clientModuleList.begin();
         modPtr != clientModuleList.end(); ++modPtr) {
        if ((*modPtr)->getObject() == theClient) {
            // We need to deallocate the client object
            // here before we close the module.
            delete theClient;
            theClient = 0;

            // Deallocating the module will close
            // the module as well.
            delete *modPtr;

            // Remove this module from the list of modules
            clientModuleList.erase(modPtr);

            break;
        }
    }
}

/**
 * Write an xClient channel part to the network, and update
 * network tables.
 */
void xServer::PartChannel(xClient* theClient, const string& chanName, const string& reason) {
    assert(theClient != NULL);

    Channel* theChan = Network->findChannel(chanName);
    if (NULL == theChan) {
        LOG(WARN, "Unable to find channel: {}", chanName);
        return;
    }

    PartChannel(theClient, theChan, reason);
}

/**
 * Write an xClient channel part to the network, and update
 * network tables.
 */
void xServer::PartChannel(xClient* theClient, Channel* theChan, const string& reason) {
    assert(theClient != 0);
    assert(theChan != 0);

    stringstream s;
    s << theClient->getCharYYXXX() << " L " << theChan->getName() << " :" << reason;

    Write(s);

    OnPartChannel(theClient, theChan);
    OnPartChannel(theClient->getInstance(), theChan);
}

/**
 * Handle an iClient channel part.  The reason for having this
 * method in addition to MSG_L() is that one of the attached
 * xClient's may KICK a client from a channel.  In that case
 * no MSG_L() or MSG_K() is read from the network.  This method
 * allows an xClient to update the internal tables without
 * needing to know exactly what needs to be done (encapsulation).
 */
void xServer::OnPartChannel(iClient* theClient, const string& chanName) {
    assert(theClient != NULL);

    Channel* theChan = Network->findChannel(chanName);
    if (NULL == theChan) {
        LOG(WARN, "Unable to find channel: {}", chanName);
        return;
    }

    OnPartChannel(theClient, theChan);
}

/**
 * Handle an iClient channel part.  The reason for having this
 * method in addition to MSG_L() is that one of the attached
 * xClient's may KICK a client from a channel.  In that case
 * no MSG_L() or MSG_K() is read from the network.  This method
 * allows an xClient to update the internal tables without
 * needing to know exactly what needs to be done (encapsulation).
 */
void xServer::OnPartChannel(iClient* theClient, Channel* theChan) {
    assert(theClient != 0);
    assert(theChan != 0);

    theClient->removeChannel(theChan);
    delete theChan->removeUser(theClient);

    PostChannelEvent(EVT_PART, theChan, static_cast<void*>(theClient));

    if (theChan->empty()) {
        // Empty channel
        delete Network->removeChannel(theChan);
    }
}

void xServer::OnPartChannel(xClient* theClient, const string& chanName) {
    assert(theClient != NULL);

    Channel* theChan = Network->findChannel(chanName);
    if (NULL == theChan) {
        LOG(WARN, "Unable to find channel: {}", chanName);
        return;
    }
    OnPartChannel(theClient, theChan);
}

void xServer::OnPartChannel(xClient* theClient, Channel* theChan) {
    assert(theClient != 0);
    assert(theChan != 0);
}

void xServer::OnXQuery(iServer* theServer, const string& Routing, const string& Message) {
    void* const thisRouting = const_cast<char*>(Routing.c_str());
    void* const thisMessage = const_cast<char*>(Message.c_str());
    PostEvent(EVT_XQUERY, static_cast<void*>(theServer), reinterpret_cast<void*>(thisRouting),
              reinterpret_cast<void*>(thisMessage));
}

void xServer::OnXReply(iServer* theServer, const string& Routing, const string& Message) {
    void* const thisRouting = const_cast<char*>(Routing.c_str());
    void* const thisMessage = const_cast<char*>(Message.c_str());
    PostEvent(EVT_XREPLY, static_cast<void*>(theServer), reinterpret_cast<void*>(thisRouting),
              reinterpret_cast<void*>(thisMessage));
}

bool xServer::JoinChannel(xClient* theClient, const string& chanName, const string& chanModes,
                          const time_t& joinTime, bool getOps) {
    // Determine the timestamp to use for the join
    time_t postJoinTime = joinTime;
    channelEventType whichEvent = EVT_JOIN;
    if (0 == postJoinTime) {
        postJoinTime = ::time(0);
    }

    Channel* theChan = Network->findChannel(chanName);
    if (theChan && (0 == joinTime)) {
        // If the channel exists, and 0 is passed as our join
        // time, then just use the channel's creation time
        postJoinTime = theChan->getCreationTime();
    }

    if ((theChan != 0) && (theChan->findUser(theClient->getInstance()))) {
        LOG_MSG(WARN, "Client attempted to join channel {chan} more than once")
            .with("chan", theChan)
            .log();
        return false;
    }

    // The modes are validated before anything is sent, and the same typed
    // changes are used for the BURST, for the MODE and to update the channel.
    std::vector<Channel::ModeChange> joinModes;
    if (string::npos != chanModes.find_first_not_of(' ')) {
        StringTokenizer st(chanModes);
        std::vector<std::string_view> tokens;
        for (StringTokenizer::size_type i = 0; i < st.size(); ++i) {
            tokens.emplace_back(st[i]);
        }

        Channel::ParsedModes parsed =
            Channel::parseModes(tokens[0], std::span(tokens).subspan(1),
                                {.protocol = (Uplink != 0) ? Uplink->getProtocol() : 11});
        for (const std::string& problem : parsed.problems) {
            LOG(WARN, "({}): {}", chanName, problem);
        }

        for (Channel::ModeChange& change : parsed.changes) {
            const Channel::ModeType type = change.mode.type;
            if (Channel::ModeType::Prefix == type || Channel::ModeType::List == type) {
                LOG(WARN, "({}): mode '{}' is not a channel mode to join with", chanName,
                    change.mode.letter);
                continue;
            }

            // A key cannot be set over another: take the old one off first,
            // in the same line.  Asking for the key already set is a no-op.
            if ('k' == change.mode.letter && change.set && theChan != 0 &&
                theChan->getMode(Channel::MODE_KEY)) {
                if (theChan->getKey() == change.arg) {
                    continue;
                }
                joinModes.push_back({false, change.mode, theChan->getKey()});
            }
            joinModes.push_back(std::move(change));
        }
    }

    // A BURST can only set modes.  Whatever clears one, and a key that
    // replaces another, follows it as a MODE.
    std::vector<Channel::ModeChange> burstModes;
    std::vector<Channel::ModeChange> afterBurst;
    for (std::size_t i = 0; i < joinModes.size(); ++i) {
        const bool replacesKey = joinModes[i].set && i > 0 && !joinModes[i - 1].set &&
                                 joinModes[i - 1].mode == joinModes[i].mode;
        (joinModes[i].set && !replacesKey ? burstModes : afterBurst).push_back(joinModes[i]);
    }
    const std::string burstBlock = Channel::burstModeBlock(burstModes);
    const std::string burstFields = burstBlock.empty() ? string() : ' ' + burstBlock;

    if ((NULL == theChan) && bursting) {
        // Need to burst the channel
        stringstream s;
        s << getCharYY() << " B " << chanName << ' ' << postJoinTime << burstFields << ' '
          << theClient->getCharYYXXX();

        if (getOps) {
            s << ":o";
        }

        Write(s);
        sendChannelModes(theClient->getCharYYXXX(), chanName, postJoinTime, afterBurst);
        whichEvent = EVT_BURST;

        // Instantiate the new channel
        theChan = new (std::nothrow) Channel(chanName, time(0));
        assert(theChan != 0);

        // Add it to the network channel table
        if (!Network->addChannel(theChan)) {
            LOG_MSG(ERROR, "addChannel() failed: {chan}").with("chan", theChan).log();

            // Prevent a memory leak
            delete theChan;
            theChan = 0;

            // Return failure
            return false;
        }

        // We have just burst a channel
    } else if (NULL == theChan) {
        // Channel doesn't exist yet, and we're NOT bursting
        // 0AT C #lksjhdlksjdlkjs 957214787

        //	elog	<< "xServer::JoinChannel> Creating new channel: "
        //		<< chanName
        //		<< endl ;

        // Create the channel
        // The client automatically gets op in this case
        {
            stringstream s;
            s << theClient->getCharYYXXX() << " C " << chanName << ' ' << postJoinTime;
            Write(s);
            whichEvent = EVT_CREATE;
        }

        sendChannelModes(theClient->getCharYYXXX(), chanName, postJoinTime, joinModes);

        // Instantiate the new channel
        theChan = new (std::nothrow) Channel(chanName, time(0));
        assert(theChan != 0);

        // Add it to the network channel table
        if (!Network->addChannel(theChan)) {
            LOG_MSG(ERROR, "addChannel() failed: {chan}").with("chan", theChan).log();

            // Prevent a memory leak
            delete theChan;
            theChan = 0;

            // Return failure
            return false;
        }

        // When we create a channel, the client automatically gets
        // ops
        getOps = true;
    } else if (bursting) {
        // Channel exists, still bursting
        // 0 B #coder-com 000031337 +tn 0AT,EAA:o,KAB,0AA

        // Is the timestamp we are bursting older than the current
        // timestamp?
        if (postJoinTime < theChan->getCreationTime()) {
            // We are bursting an older timestamp
            // Remove all modes to keep synced.
            removeAllChanModes(theChan);
        }

        if (postJoinTime > theChan->getCreationTime()) {
            // We are bursting into a channel that has an
            // older timestamp than the one we were supplied with.
            // We use the existing timestamp to remain in sync
            // (And get op'd if needs be).
            postJoinTime = theChan->getCreationTime();
        }

        stringstream s;
        s << getCharYY() << " B " << chanName << ' ' << postJoinTime << burstFields << ' '
          << theClient->getCharYYXXX();

        if (getOps) {
            s << ":o";
        }

        Write(s);
        sendChannelModes(theClient->getCharYYXXX(), chanName, postJoinTime, afterBurst);
        whichEvent = EVT_BURST;
    } else {
        // After bursting, and the channel exists
        {
            stringstream s2;
            s2 << theClient->getCharYYXXX() << " J " << chanName << " " << postJoinTime;

            Write(s2);
        }

        if (getOps) {
            // Op the bot
            const Channel::ModeChange opClient{true, *Channel::findMode('o'),
                                               theClient->getCharYYXXX()};
            sendChannelModes(getCharYY(), chanName, postJoinTime, std::span(&opClient, 1));
        }

        // Set the channel modes
        sendChannelModes(theClient->getCharYYXXX(), chanName, postJoinTime, joinModes);
    }

    if (postJoinTime < theChan->getCreationTime()) {
        theChan->setCreationTime(joinTime);
    }

    // Is the string not empty, and not only consisting of spaces?
    // Our own join does not raise mode events, so the channel is updated
    // directly rather than through ApplyChannelModes().
    applyModesSilently(theChan, joinModes);

    // An xClient has joined a channel, update its iClient instance
    iClient* theIClient = theClient->getInstance();

    // Add the channel to the iClient's info
    theIClient->addChannel(theChan);

    // Create a new ChannelUser instance for the channel's records
    // Did the xClient request ops in the channel?
    ChannelUser* theChanUser =
        new (std::nothrow) ChannelUser(theIClient, getOps ? ChannelUser::MODE_CHANOP : 0);

    // Make sure the allocation was successful
    assert(theChanUser != 0);

    // Add the ChannelUser to the channel
    if (!theChan->addUser(theChanUser)) {
        LOG_MSG(ERROR, "Unable to add xClient ({}) to channel {chan}", theClient->getNickName())
            .with("chan", theChan)
            .log();

        // TODO
        return false;
    }

    PostChannelEvent(whichEvent, theChan, static_cast<void*>(theIClient),
                     static_cast<void*>(theChanUser));

    theClient->OnJoin(theChan->getName());
    return true;
}

// K N Isomer 2 957217279 ~perry p136-tnt1.ham.ihug.co.nz DLbaCI KAC :*Unknown*
void xServer::BurstClient(xClient* theClient) {
    int hopCount = 1;
    if ((theClient->getCharYY()[0] != getCharYY()[0]) ||
        (theClient->getCharYY()[1] != getCharYY()[1])) {
        // The xClient is not on this server
        hopCount = 2;
    }

    stringstream s;
    s << theClient->getCharYY() << " N " << theClient->getNickName() << ' ' << hopCount << " 31337 "
      << theClient->getUserName() << ' ' << theClient->getHostName() << ' ' << theClient->getModes()
      << ' ';

    if (theClient->getMode(iClient::MODE_TLS))
        s << "_ ";

    s << "AAAAAA " << theClient->getCharYYXXX() << " :" << theClient->getDescription();
    Write(s);

    theClient->OnConnect();
}

void xServer::BurstClients() {
    xNetwork::localClientIterator ptr = Network->localClient_begin();
    while (ptr != Network->localClient_end()) {
        if (ptr->second->IsStealth()) {
            // No N-line; still notify the module that we are linked
            ptr->second->OnConnect();
        } else {
            BurstClient(ptr->second);
        }
        ++ptr;
    }
}

void xServer::BurstChannels() {
    xNetwork::localClientIterator ptr = Network->localClient_begin();
    while (ptr != Network->localClient_end()) {
        if (!ptr->second->IsStealth()) {
            ptr->second->BurstChannels();
        }
        ++ptr;
    }
}

// Burst all client info, but NOT channel info.
// TODO: Burst juped servers.
void xServer::Burst() {
    xNetwork::localClientIterator ptr = Network->localClient_begin(),
                                  end = Network->localClient_end();

    while (ptr != end) {
        ptr->second->OnConnect();

        // No need to add to tables, it is
        // already there
        ++ptr;
    }

    // TODO: Need to burst fake servers and clients
}

void xServer::dumpStats() {
    clog << "Number of channels: " << Network->channelList_size() << endl;
    clog << "Number of servers: " << Network->serverList_size() << endl;
    clog << "Number of clients: " << Network->clientList_size() << endl;
    clog << "Number of glines: " << glineList.size() << endl;
    clog << "Last burst duration: " << (burstEnd - burstStart) << " seconds" << endl;
    clog << "Read " << burstBytes << " bytes and processed " << burstLines
         << " commands over the last " << (::time(0) - burstStart) << " seconds." << endl;
}

namespace {

/// The name logging.conf is looked for under when the main conf says nothing
const string defaultLoggingConf("logging.conf");

/// The one sink id the command line is about: the debug file of -d and -D
const string debugLogSinkId("debuglog");

/**
 * The configuration of a process that has no logging.conf: the debug log and
 * the console, both fed by the root at INFO, and the old elog stream at the
 * DEBUG level it has always had.
 *
 * This is the same thing the file
 *
 *     sink.console.type    = console
 *     sink.debuglog.type   = file
 *     sink.debuglog.path   = debug.log
 *     sink.debuglog.format = text
 *     logger.root          = INFO, debuglog, console
 *     logger.legacy        = on
 *
 * would be, built here rather than written anywhere: an installation without a
 * logging.conf logs what it always logged.
 */
LogConfig builtInLogConfig() {
    LogConfig config;

    SinkSpec console;
    console.id = "console";
    console.type = "console";
    config.sinks.push_back(console);

    SinkSpec debugLog;
    debugLog.id = debugLogSinkId;
    debugLog.type = "file";
    debugLog.path = "debug.log";
    debugLog.json = false;
    config.sinks.push_back(debugLog);

    LoggerSpec root;
    // The root's name is empty, however a file spells it
    root.level = INFO;
    root.sinks.push_back(debugLogSinkId);
    root.sinks.push_back("console");
    config.loggers.push_back(root);

    LoggerSpec legacy;
    legacy.name = "legacy";
    legacy.level = DEBUG;
    config.loggers.push_back(legacy);

    return config;
}

/**
 * The command line on top of a configuration, whether that came from a file or
 * from builtInLogConfig(): -d puts the debug log where it said, and -D takes it
 * away altogether, so that "no debug log" keeps meaning no debug log whatever
 * logging.conf asks for.  Every other sink is the file's business alone.
 */
void adjustForCommandLine(LogConfig& config, bool doDebug, bool elogFileGiven,
                          const string& elogFileName) {
    if (elogFileGiven)
        for (SinkSpec& sink : config.sinks)
            if (debugLogSinkId == sink.id)
                sink.path = elogFileName;

    if (doDebug)
        return;

    for (std::vector<SinkSpec>::iterator sink = config.sinks.begin(); sink != config.sinks.end();)
        if (debugLogSinkId == sink->id)
            sink = config.sinks.erase(sink);
        else
            ++sink;

    // And with it every mention of it, which would otherwise be a dangling id
    for (LoggerSpec& logger : config.loggers)
        for (std::vector<string>::iterator id = logger.sinks.begin(); id != logger.sinks.end();)
            if (debugLogSinkId == *id)
                id = logger.sinks.erase(id);
            else
                ++id;
}

/// The path the debug log would be written to, empty when there is none
string debugLogPath(const LogConfig& config) {
    for (const SinkSpec& sink : config.sinks)
        if (debugLogSinkId == sink.id)
            return sink.path;

    return string();
}

/**
 * Says what went wrong with logging.conf, one record per problem, in the words
 * LogManager::loadFile() uses for the same thing.  These are records like any
 * other: they go wherever the configuration in force sends "core.config", which
 * is why the caller says them once it has applied one.
 *
 * The function is the caller's rather than this one's: what the reader of a log
 * file wants to see is where the configuration was being read, not the three
 * lines that write the message.
 */
void reportLogConfigErrors(const char* function, const std::vector<string>& errors) {
    Logger* const reporter = coreLogger(CoreLogger::Config);

    for (const string& error : errors)
        reporter->writeFunc(ERROR, function, "logging.conf: {}", error);
}

} // anonymous namespace

/**
 * The name of the logging configuration file: the optional "logging_conf" key
 * of the main configuration file, or logging.conf.  A relative name is relative
 * to the working directory, like every other file name of this process.
 *
 * The key is read with Find() and not with Require(), which exits: this runs
 * before the main configuration file has been read properly, and a main conf
 * that cannot be read is the business of the start-up code that reads it next -
 * it reports it, and it is the one that stops.
 */
string xServer::loggingConfFileName() const {
    {
        std::ifstream probe(configFileName.c_str());

        if (!probe.is_open())
            return defaultLoggingConf;
    }

    EConfig mainConf(configFileName);
    const EConfig::const_iterator named = mainConf.Find("logging_conf");

    if (mainConf.end() == named || named->second.empty())
        return defaultLoggingConf;

    return named->second;
}

void xServer::setupLogging(bool reload) {
    /* logging.conf is data, and data can neither stop this process nor take its
     * logging away.  Whatever reading one may cost - the allocation a monstrous
     * name or a monstrous list asks for - is caught here: on a reload the
     * configuration in force stays in force and the reason is logged, and at
     * start-up the process carries on with whatever it can still be given.
     * Nothing here exits, and nothing here throws on */
    string failure;

    try {
        /* The console is written to only when the process was asked to be verbose,
         * which is the condition the old logger wrote to it under.  A logging.conf
         * with a console sink in it still says nothing on a daemon's terminal */
        ConsoleSink::setEnabled(verbose);

        /* The kind of sink only core can make, because only core knows what a
         * channel is.  A reload registers the same thing again, which replaces it */
        LogManager::registerSinkType(
            "irc", [this](const SinkSpec& spec, string&) -> std::shared_ptr<LogSink> {
                return std::make_shared<IrcLogSink>(this, spec.channel, spec.highlight, spec.rate);
            });

        /* And the one the notifier makes, which is registered here rather than by
         * whoever wants to be paged: a pager is a sink of logging.conf like any
         * other, so it exists before the file that may name it is read */
        PushoverClient::registerSinkType();

        const string fileName = loggingConfFileName();

        bool haveFile = false;

        {
            std::ifstream probe(fileName.c_str());

            haveFile = probe.is_open();
        }

        /* Everything that was wrong with the file, said once at the end: reporting a
         * problem is itself logging, and said here it would go wherever the process
         * happened to be logging before - at start-up, nowhere at all */
        std::vector<string> problems;

        LogConfig config;

        // Whether the configuration that ends up in force is the one in that file
        bool fromFile = false;

        if (haveFile) {
            std::vector<string> errors;

            if (parseLogConfig(fileName, config, errors))
                fromFile = true;
            else
                problems = errors;
        }

        /* A file that is not a configuration changes nothing at all on a reload:
         * what is in force stays in force, and the process goes on logging where it
         * was logging.  At start-up there is nothing to keep, and the built-in
         * default is what a process with no usable file has */
        if (haveFile && !fromFile && reload) {
            reportLogConfigErrors(__PRETTY_FUNCTION__, problems);

            return;
        }

        if (!fromFile)
            config = builtInLogConfig();

        adjustForCommandLine(config, doDebug, elogFileGiven, elogFileName);

        std::vector<string> errors;
        bool applied = LogManager::configure(config, errors);

        if (!applied) {
            problems.insert(problems.end(), errors.begin(), errors.end());

            // As above: on a reload the configuration in force is the one to keep
            if (reload) {
                reportLogConfigErrors(__PRETTY_FUNCTION__, problems);

                return;
            }

            /* At start-up a file that cannot be applied - a sink whose file will
             * not open, a kind of sink this build has not got - is no reason to log
             * nowhere at all: the built-in default is tried instead */
            if (fromFile) {
                fromFile = false;
                config = builtInLogConfig();
                adjustForCommandLine(config, doDebug, elogFileGiven, elogFileName);

                errors.clear();
                applied = LogManager::configure(config, errors);
                problems.insert(problems.end(), errors.begin(), errors.end());
            }

            /* And when even that will not do - an unwritable debug log - the
             * process starts all the same and logs wherever it still can.  Nothing
             * here exits: writing a log file is not what this process is for */
            if (!applied)
                clog << "*** Unable to open log file: " << debugLogPath(config) << endl;
        }

        /* Said last of all, so that every one of these goes where the configuration
         * just applied says it goes rather than wherever the one before it did */
        reportLogConfigErrors(__PRETTY_FUNCTION__, problems);

        /* A file that holds a token - a pushover sink's - and that anybody on
         * this host may read is worth one warning.  Only for the file that is
         * really in force: the built-in default holds no secret */
        if (fromFile)
            LogManager::warnIfSecretsAreReadable(fileName, config);

        if (!reload && !haveFile)
            LOG(INFO, "No logging.conf found; using built-in defaults (see "
                      "bin/logging.example.conf)");

        if (reload && fromFile)
            LOG(INFO, "Reloaded {}", fileName);

        return;
    } catch (const std::exception& e) {
        failure = e.what();
    } catch (...) {
        failure = "an unknown error";
    }

    if (reload) {
        LOG_CORE(Config, ERROR, "logging.conf: {}", failure);

        return;
    }

    /* At start-up there is nothing in force to keep, so the built-in default is
     * tried; when even that will not go on, the console the root was given while
     * the process started up is where this process logs */
    clog << "*** logging.conf: " << failure << endl;

    try {
        LogConfig config = builtInLogConfig();

        adjustForCommandLine(config, doDebug, elogFileGiven, elogFileName);

        std::vector<string> errors;

        LogManager::configure(config, errors);
    } catch (...) {
        clog << "*** Unable to apply the built-in logging defaults" << endl;
    }
}

void xServer::startLogging(bool logrotate) {
    if (doDebug) {
        clog << "*** Running in debug mode..." << endl;
    }

    if (verbose) {
        LOG(INFO, "Running in verbose mode...");
    }

    if (logSocket) {
        if (logrotate)
            socketFile.open(socketFileName.c_str(), std::ios::out | std::ios::app);
        else
            socketFile.open(socketFileName.c_str(), std::ios::out);
        if (!socketFile.is_open()) {
            clog << "*** Unable to open socket log file: " << socketFileName << endl;
            ::exit(-1);
        }
        clog << "*** Logging raw data to " << socketFileName << "..." << endl;
    }
}

void xServer::rotateLogs() {
    LOG(INFO, "Received SIGHUP. Rotating log files...");

    /* Every sink of every logger opens its path anew, the ones a module
     * attached in code included, so that the next record lands in a new file
     * rather than in the one logrotate moved away */
    LogManager::reopenAll();

    // And logging.conf is read again, so that a change to it takes effect
    setupLogging(true);

    if (logSocket && socketFile.is_open()) {
        socketFile.close();
    }
    startLogging(true);
}

void xServer::run() { mainLoop(); }

void xServer::removeAllChanModes(Channel* theChan) {
    modeVectorType modeVector;

    // This is a protected method, theChan is non-NULL
    if (theChan->getMode(Channel::MODE_TOPICLIMIT)) {
        modeVector.push_back(make_pair(false, Channel::MODE_TOPICLIMIT));
    }
    if (theChan->getMode(Channel::MODE_NOPRIVMSGS)) {
        modeVector.push_back(make_pair(false, Channel::MODE_NOPRIVMSGS));
    }
    if (theChan->getMode(Channel::MODE_SECRET)) {
        modeVector.push_back(make_pair(false, Channel::MODE_SECRET));
    }
    if (theChan->getMode(Channel::MODE_PRIVATE)) {
        modeVector.push_back(make_pair(false, Channel::MODE_PRIVATE));
    }
    if (theChan->getMode(Channel::MODE_MODERATED)) {
        modeVector.push_back(make_pair(false, Channel::MODE_MODERATED));
    }
    if (theChan->getMode(Channel::MODE_INVITEONLY)) {
        modeVector.push_back(make_pair(false, Channel::MODE_INVITEONLY));
    }
    if (theChan->getMode(Channel::MODE_REGONLY)) {
        modeVector.push_back(make_pair(false, Channel::MODE_REGONLY));
    }
    if (theChan->getMode(Channel::MODE_REGISTERED)) {
        modeVector.push_back(make_pair(false, Channel::MODE_REGISTERED));
    }
    if (theChan->getMode(Channel::MODE_DELJOINS)) {
        modeVector.push_back(make_pair(false, Channel::MODE_DELJOINS));
    }
    if (theChan->getMode(Channel::MODE_NOCOLOR)) {
        modeVector.push_back(make_pair(false, Channel::MODE_NOCOLOR));
    }
    if (theChan->getMode(Channel::MODE_NOCTCP)) {
        modeVector.push_back(make_pair(false, Channel::MODE_NOCTCP));
    }
    if (theChan->getMode(Channel::MODE_NOPARTMSGS)) {
        modeVector.push_back(make_pair(false, Channel::MODE_NOPARTMSGS));
    }
    if (theChan->getMode(Channel::MODE_MODERATENOREG)) {
        modeVector.push_back(make_pair(false, Channel::MODE_MODERATENOREG));
    }
    if (theChan->getMode(Channel::MODE_TLSONLY)) {
        modeVector.push_back(make_pair(false, Channel::MODE_TLSONLY));
    }
    if (theChan->getMode(Channel::MODE_LIMIT)) {
        OnChannelModeL(theChan, false, 0, 0);
    }
    if (theChan->getMode(Channel::MODE_KEY)) {
        OnChannelModeK(theChan, false, 0, string());
    }
    if (theChan->getMode(Channel::MODE_APASS)) {
        OnChannelModeA(theChan, false, 0, string());
    }
    if (theChan->getMode(Channel::MODE_UPASS)) {
        OnChannelModeU(theChan, false, 0, string());
    }

    if (!modeVector.empty()) {
        OnChannelMode(theChan, 0, modeVector);
    }

    opVectorType opVector;
    voiceVectorType voiceVector;

    for (Channel::userIterator ptr = theChan->userList_begin(), end = theChan->userList_end();
         ptr != end; ++ptr) {
        if (ptr->second->getMode(ChannelUser::MODE_CHANOP)) {
            opVector.push_back(opVectorType::value_type(false, ptr->second));
        }
        if (ptr->second->getMode(ChannelUser::MODE_VOICE)) {
            voiceVector.push_back(voiceVectorType::value_type(false, ptr->second));
        }
    }

    banVectorType banVector;

    Channel::banIterator ptr = theChan->banList_begin(), end = theChan->banList_end();

    for (; ptr != end; ++ptr) {
        banVector.push_back(banVectorType::value_type(false, *ptr));
    }

    if (!opVector.empty()) {
        OnChannelModeO(theChan, 0, opVector);
    }
    if (!voiceVector.empty()) {
        OnChannelModeV(theChan, 0, voiceVector);
    }
    if (!banVector.empty()) {
        OnChannelModeB(theChan, 0, banVector);
    }
}

int xServer::Wallops(const string& msg) {
    if (msg.empty()) {
        return -1;
    }

    stringstream s;
    s << getCharYY() << " WA :" << msg;

    return Write(s);
}

bool xServer::ClearMode(Channel* theChan, const std::string& modes, const iClient* from) {
    assert(theChan != 0);

    if (modes.empty()) {
        return false;
    }

    // What clearing these modes removes from the channel as it is now.  A
    // letter that is no mode is left out, and the rest is cleared.
    std::vector<std::string> problems;
    std::vector<Channel::ModeChange> changes = theChan->changesToClear(modes, problems);
    for (const std::string& problem : problems) {
        LOG(WARN, "({}): {}", theChan->getName(), problem);
    }

    string letters;
    const unsigned int protocol = (Uplink != 0) ? Uplink->getProtocol() : 11;
    std::ranges::copy_if(modes, std::back_inserter(letters), [protocol](char letter) {
        // Not a letter the uplink does not have: u, M and Z came with P11
        const std::optional<Channel::ModeInfo> mode = Channel::findMode(letter);
        return mode && mode->protocol <= protocol;
    });

    if (letters.empty()) {
        return false;
    }

    // A server, or an oper, can have it all done with one CLEARMODE.  Any
    // other client has to be opped, and takes the modes off one by one,
    // each with its argument: a bare "-b" removes nothing.
    if ((from != 0) && !from->isOper()) {
        return changeModes(theChan, std::move(changes), from, 0);
    }

    // The network first, then our tables and the modules
    const bool sent = Write("{} CM {} :{}\r\n", numericOf(from), theChan->getName(), letters);
    ApplyChannelModes(theChan, 0, changes, "xServer::ClearMode>");

    return sent;
}

std::string xServer::numericOf(const iClient* from) const {
    return (from != 0) ? from->getCharYYXXX() : string(getCharYY());
}

bool xServer::enterChannel(const iClient* from, Channel* theChan, xClient*& joined) {
    joined = 0;
    if (0 == from) {
        // The network applies a server's change without asking who is opped
        return true;
    }

    if (const ChannelUser* member = theChan->findUser(from)) {
        return member->isModeO();
    }

    // Not on the channel.  One of our own clients joins, having the server
    // op it; anybody else cannot make the change.
    xClient* ours = Network->findLocalClient(from->getCharYYXXX());
    if (0 == ours || ours->getInstance() != from) {
        return false;
    }
    ours->Join(theChan, string(), 0, true);
    joined = ours;
    return true;
}

bool xServer::changeModes(Channel* theChan, std::vector<Channel::ModeChange> requested,
                          const iClient* from, ChannelUser* eventSource, time_t olderTimestamp) {
    assert(theChan != 0);

    // Everything is validated before anything is sent or changed, so that a
    // failure on a later mode cannot leave our state changed and the
    // network not told.

    // Check the changes against the channel, and drop what would change
    // nothing.  The target of an o or v is named by its numeric, as it is
    // on the wire.
    std::vector<Channel::ModeChange> wire;
    for (Channel::ModeChange& change : requested) {
        const Channel::ModeInfo& mode = change.mode;

        switch (mode.type) {
        case Channel::ModeType::Setting: {
            // A key or password cannot be replaced, only removed and set
            // again; removing one that is not there is a no-op.
            const bool isSet = theChan->getMode(mode.flag);
            if (change.set && isSet) {
                return false;
            }
            if (!change.set && !isSet) {
                continue;
            }
            break;
        }

        case Channel::ModeType::Prefix: {
            iClient* targetClient = Network->findClient(change.arg);
            ChannelUser* targetUser = (targetClient != 0) ? theChan->findUser(targetClient) : 0;
            if (0 == targetUser) {
                return false;
            }
            const bool has = ('o' == mode.letter) ? targetUser->isModeO() : targetUser->isModeV();
            if (has == change.set) {
                continue;
            }
            break;
        }

        case Channel::ModeType::List:
            // We see every ban on the network, so our list is the network's
            if (theChan->findBan(change.arg) == change.set) {
                continue;
            }
            break;

        case Channel::ModeType::Flag:
        case Channel::ModeType::SetOnly:
            break;
        }

        wire.push_back(std::move(change));
    }

    if (wire.empty()) {
        // Nothing to change, so nothing to be on the channel for
        return true;
    }

    xClient* joined = 0;
    if (!enterChannel(from, theChan, joined)) {
        return false;
    }

    // The line ends in the channel's timestamp.  A server that knows an
    // older one takes that over (ircu, mode_parse()), so an older one we
    // are given becomes ours before it goes out.  A younger one would have
    // the line bounced as coming from a server out of step: ours stands.
    if (olderTimestamp != 0) {
        if (olderTimestamp < theChan->getCreationTime()) {
            theChan->setCreationTime(olderTimestamp);
        } else if (olderTimestamp > theChan->getCreationTime()) {
            LOG(WARN,
                "({}): the timestamp given, {}, is younger than the channel's and is not sent",
                theChan->getName(), olderTimestamp);
        }
    }

    // Tell the network first, then update our tables and the modules: a
    // module may answer an event with traffic of its own, which has to
    // follow the mode that caused it.
    sendChannelModes(numericOf(from), theChan, wire);

    // A ban of ours is recorded as the bot's, or as this server's where the
    // change is the server's own
    ApplyChannelModes(theChan, eventSource, wire, "xServer::changeModes>",
                      (from != 0) ? from->getNickName() : getName());

    if (joined != 0) {
        joined->Part(theChan);
    }
    return true;
}

bool xServer::changeMembers(Channel* theChan, char letter, bool set,
                            std::span<iClient* const> targets, const iClient* from,
                            ChannelUser* eventSource) {
    assert(theChan != 0);

    const std::optional<Channel::ModeInfo> mode = Channel::findMode(letter);
    assert(mode && Channel::ModeType::Prefix == mode->type);

    // With a single target a problem with it fails the call; with several,
    // that target is skipped.
    const bool single = (1 == targets.size());

    std::vector<Channel::ModeChange> changes;
    for (const iClient* target : targets) {
        const char* problem = 0;
        if (NULL == target) {
            problem = "a NULL iClient";
        } else if ('o' == letter && !set && target->isModeK()) {
            // A network service (+k) is not deopped
            problem = "";
        } else if (NULL == theChan->findUser(target)) {
            problem = "a client that is not on the channel";
        }

        if (problem != 0) {
            if (*problem != 0) {
                LOG_MSG(WARN, "({chan}): {}", problem).with("chan", theChan).log();
            }
            if (single) {
                return false;
            }
            continue;
        }
        changes.push_back({set, *mode, target->getCharYYXXX()});
    }
    return changeModes(theChan, std::move(changes), from, eventSource);
}

bool xServer::changeBans(Channel* theChan, const banVectorType& bans, const iClient* from,
                         ChannelUser* eventSource) {
    assert(theChan != 0);

    std::vector<Channel::ModeChange> changes;
    changes.reserve(bans.size());
    for (const auto& [set, mask] : bans) {
        changes.push_back({set, *Channel::findMode('b'), mask});
    }
    return changeModes(theChan, std::move(changes), from, eventSource);
}

xServer::banVectorType xServer::bansFor(const Channel* theChan,
                                        std::span<iClient* const> targets) const {
    banVectorType bans;
    for (const iClient* target : targets) {
        if (NULL == target) {
            LOG_MSG(WARN, "Found NULL iClient for channel: {chan}").with("chan", theChan).log();
            continue;
        }
        // A network service (+k) is not banned, and neither is somebody
        // who is not on the channel
        if (target->isModeK() || 0 == theChan->findUser(target)) {
            continue;
        }
        bans.emplace_back(true, Channel::createBan(target));
    }
    return bans;
}

bool xServer::kickMembers(Channel* theChan, std::span<iClient* const> targets,
                          const std::string& reason, const iClient* from, iClient* kicker) {
    assert(theChan != 0);

    std::vector<iClient*> kicked;
    for (iClient* target : targets) {
        if (NULL == target) {
            LOG_MSG(WARN, "Found NULL iClient for channel: {chan}").with("chan", theChan).log();
            continue;
        }
        // A network service (+k) is not kicked
        if (target->isModeK()) {
            continue;
        }
        if (NULL == theChan->findUser(target)) {
            LOG_MSG(WARN, "Can't find {client} on channel {chan}")
                .with("client", target)
                .with("chan", theChan)
                .log();
            continue;
        }
        kicked.push_back(target);
    }

    if (kicked.empty()) {
        // With one target that is a refusal; with several, nothing to do
        return targets.size() != 1;
    }

    xClient* joined = 0;
    if (!enterChannel(from, theChan, joined)) {
        return false;
    }

    const std::string sourceNumeric = numericOf(from);
    for (iClient* target : kicked) {
        Write("{} K {} {} :{}", sourceNumeric, theChan->getName(), target->getCharYYXXX(), reason);

        // Take the member off the channel now.  A PART that a server sends
        // for it later is ignored, and this way a fake client of ours is
        // cleaned up too.
        delete theChan->removeUser(target);

        if (!target->removeChannel(theChan)) {
            LOG_MSG(ERROR, "Unable to remove channel {chan} from the iClient {client}")
                .with("chan", theChan)
                .with("client", target)
                .log();
        }

        // The network keeps a kicked member as a zombie until its own server
        // confirms with a PART (ircu, make_zombie()).  For a client of ours
        // that is us: nobody else will.  Where the uplink has removed the
        // member already, which it does when the kicker sits on the victim's
        // server, a PART for somebody who is not on the channel is ignored.
        if (isOurClient(target)) {
            Write("{} L {}", target->getCharYYXXX(), theChan->getName());
        }

        // The last argument says the kicked client is one of ours
        PostChannelKick(theChan, kicker, target, reason, isOurClient(target));
    }

    // Parting removes a channel that is left empty; otherwise it is ours to
    if (joined != 0) {
        joined->Part(theChan);
    } else if (theChan->empty()) {
        delete Network->removeChannel(theChan->getName());
    }
    return true;
}

bool xServer::sendText(TextType type, const iClient* from, std::string_view target,
                       std::string_view text) {
    if (target.empty()) {
        return false;
    }

    const char* const token = (TextType::PRIVMSG == type)  ? "P"
                              : (TextType::NOTICE == type) ? "O"
                                                           : "WC";

    const std::string prefix = numericOf(from) + ' ' + token + ' ' + std::string(target) + " :";
    if (prefix.size() + 1 >= (IRC_MAX_LINE - 2)) {
        return false;
    }
    const std::size_t room = (IRC_MAX_LINE - 2) - prefix.size();

    // A line break in the text means "and another message": every line goes
    // out by itself, and one too long for a line is continued in the next.
    // Write() would cut the text at a line break; this is what to do instead
    // when the breaks are meant, as in a multi-line help text.
    bool sent = false;
    while (!text.empty()) {
        const std::size_t lineEnd = std::min(text.find_first_of("\r\n"), text.size());
        std::string_view line = text.substr(0, lineEnd);
        text.remove_prefix(std::min(lineEnd + 1, text.size()));

        while (!line.empty()) {
            std::size_t take = std::min(line.size(), room);
            if (take < line.size()) {
                // Break at a space if there is one in the last part
                const std::size_t space = line.substr(0, take).rfind(' ');
                if (space != std::string_view::npos && space > take / 2) {
                    take = space;
                }
            }
            sent = Write(prefix + std::string(line.substr(0, take))) || sent;
            line.remove_prefix(take);
            while (!line.empty() && ' ' == line.front()) {
                line.remove_prefix(1);
            }
        }
    }
    return sent;
}

bool xServer::Topic(Channel* theChan, const std::string& newTopic, const iClient* from) {
    assert(theChan != 0);

    // A client has to be on the channel, and opped if it is +t.  A server
    // is not asked.
    if ((from != 0)) {
        const ChannelUser* member = theChan->findUser(from);
        if (0 == member || (theChan->getMode(Channel::MODE_TOPICLIMIT) && !member->isModeO())) {
            return false;
        }
    }

    const time_t now = ::time(0);
    bool sent = false;
    if (Uplink != 0 && Uplink->getProtocol() >= 11) {
        // With the channel's creation time, so that the topic is not applied
        // to a younger channel of the same name, and the topic's own time
        sent =
            Write("{} T {} {} {} :{}", numericOf(from), theChan->getName(),
                  static_cast<long>(theChan->getCreationTime()), static_cast<long>(now), newTopic);
    } else {
        sent = Write("{} T {} :{}", numericOf(from), theChan->getName(), newTopic);
    }

#ifdef TOPIC_TRACK
    theChan->setTopic(newTopic);
    theChan->setTopicTS(now);
    theChan->setTopicWhoSet((from != 0) ? from->getNickName() : getName());
#endif

    // Setting the topic reveals a delayed-join member
    if ((from != 0)) {
        theChan->revealUser(from);
    }
    return sent;
}

bool xServer::Invite(iClient* target, Channel* theChan, const iClient* from) {
    assert(target != 0 && theChan != 0);

    // An INVITE from a server is a protocol violation (ircu doc/P11.md 8.11)
    if ((0 == from)) {
        LOG_MSG(WARN, "({chan}): only a client can invite").with("chan", theChan).log();
        return false;
    }

    // A P11 link names the invitee by numnick and takes the channel's
    // creation time; a P10 link wants the nick.
    if (Uplink != 0 && Uplink->getProtocol() >= 11) {
        return Write("{} I {} {} {}", numericOf(from), target->getCharYYXXX(), theChan->getName(),
                     static_cast<long>(theChan->getCreationTime()));
    }
    return Write("{} I {} {}", numericOf(from), target->getNickName(), theChan->getName());
}

bool xServer::Kick(Channel* theChan, iClient* target, const std::string& reason,
                   const iClient* from) {
    assert(target != 0);
    iClient* const targets[] = {target};
    // As with the inbound KICK, the kicker is null when a server did it
    return kickMembers(theChan, targets, reason, from, const_cast<iClient*>(from));
}

bool xServer::Kick(Channel* theChan, const std::vector<iClient*>& targets,
                   const std::string& reason, const iClient* from) {
    return kickMembers(theChan, targets, reason, from, const_cast<iClient*>(from));
}

bool xServer::Op(Channel* theChan, iClient* target, const iClient* from) {
    iClient* const targets[] = {target};
    return changeMembers(theChan, 'o', true, targets, from,
                         (from != 0) ? theChan->findUser(from) : 0);
}

bool xServer::Op(Channel* theChan, const std::vector<iClient*>& targets, const iClient* from) {
    return changeMembers(theChan, 'o', true, targets, from,
                         (from != 0) ? theChan->findUser(from) : 0);
}

bool xServer::DeOp(Channel* theChan, iClient* target, const iClient* from) {
    iClient* const targets[] = {target};
    return changeMembers(theChan, 'o', false, targets, from,
                         (from != 0) ? theChan->findUser(from) : 0);
}

bool xServer::DeOp(Channel* theChan, const std::vector<iClient*>& targets, const iClient* from) {
    return changeMembers(theChan, 'o', false, targets, from,
                         (from != 0) ? theChan->findUser(from) : 0);
}

bool xServer::Voice(Channel* theChan, iClient* target, const iClient* from) {
    iClient* const targets[] = {target};
    return changeMembers(theChan, 'v', true, targets, from,
                         (from != 0) ? theChan->findUser(from) : 0);
}

bool xServer::Voice(Channel* theChan, const std::vector<iClient*>& targets, const iClient* from) {
    return changeMembers(theChan, 'v', true, targets, from,
                         (from != 0) ? theChan->findUser(from) : 0);
}

bool xServer::DeVoice(Channel* theChan, iClient* target, const iClient* from) {
    iClient* const targets[] = {target};
    return changeMembers(theChan, 'v', false, targets, from,
                         (from != 0) ? theChan->findUser(from) : 0);
}

bool xServer::DeVoice(Channel* theChan, const std::vector<iClient*>& targets, const iClient* from) {
    return changeMembers(theChan, 'v', false, targets, from,
                         (from != 0) ? theChan->findUser(from) : 0);
}

bool xServer::Ban(Channel* theChan, iClient* target, const iClient* from) {
    assert(theChan != 0 && target != 0);
    if (target->isModeK()) {
        return false;
    }
    iClient* const targets[] = {target};
    return Ban(theChan, bansFor(theChan, targets), from);
}

bool xServer::Ban(Channel* theChan, const std::vector<iClient*>& targets, const iClient* from) {
    assert(theChan != 0);
    return Ban(theChan, bansFor(theChan, targets), from);
}

bool xServer::Ban(Channel* theChan, const banVectorType& bans, const iClient* from) {
    assert(theChan != 0);
    return changeBans(theChan, bans, from, (from != 0) ? theChan->findUser(from) : 0);
}

bool xServer::UnBan(Channel* theChan, const std::string& banMask, const iClient* from) {
    return Ban(theChan, banVectorType{{false, banMask}}, from);
}

bool xServer::UnBan(Channel* theChan, const banVectorType& bans, const iClient* from) {
    return Ban(theChan, bans, from);
}

bool xServer::sendChannelModes(const std::string& source, Channel* theChan,
                               std::span<const Channel::ModeChange> changes) {
    assert(theChan != 0);
    return sendChannelModes(source, theChan->getName(), theChan->getCreationTime(), changes);
}

bool xServer::sendChannelModes(const std::string& source, const std::string& chanName,
                               time_t timestamp, std::span<const Channel::ModeChange> changes) {
    // The one place a channel MODE line is put together.  formatLines()
    // splits at six arguments or at the line limit, and ends every line in
    // the channel timestamp, which a P11 peer requires.
    bool sent = true;
    for (const std::string& line : Channel::formatModeLines(
             source + " M " + chanName, changes, static_cast<std::uint64_t>(timestamp))) {
        sent = Write(line) && sent;
    }
    return sent;
}

void xServer::applyModesSilently(Channel* theChan, std::span<const Channel::ModeChange> changes) {
    for (const Channel::ModeChange& change : changes) {
        switch (change.mode.type) {
        case Channel::ModeType::Flag:
            if (change.set) {
                theChan->setMode(change.mode.flag);
            } else {
                theChan->removeMode(change.mode.flag);
            }
            break;
        case Channel::ModeType::SetOnly: {
            unsigned int limit = 0;
            std::from_chars(change.arg.data(), change.arg.data() + change.arg.size(), limit);
            theChan->onModeL(change.set, limit);
            break;
        }
        case Channel::ModeType::Setting: {
            const string value = change.set ? change.arg : string();
            if ('k' == change.mode.letter) {
                theChan->onModeK(change.set, value);
            } else if ('A' == change.mode.letter) {
                theChan->onModeA(change.set, value);
            } else {
                theChan->onModeU(change.set, value);
            }
            break;
        }
        case Channel::ModeType::Prefix:
        case Channel::ModeType::List:
            break;
        }
    }
}

bool xServer::Mode(xClient* theClient, Channel* theChan, const string& modes, const string& args) {
    // The form modules have always used: a null xClient means "as the server"
    return (theClient != 0) ? Mode(theChan, modes, args, theClient->getInstance())
                            : Mode(theChan, modes, args);
}

bool xServer::Mode(Channel* theChan, const string& modes, const string& args, const iClient* from,
                   time_t olderTimestamp) {
    assert(theChan != 0);

    // The modes string must not be empty; the args string may be
    if (modes.empty()) {
        return false;
    }

    // The input is one or more groups of "<modes> [<args>...]", as in
    // "+ov nick1 nick2" or "+o nick1 -v nick2".
    StringTokenizer st(modes + ' ' + args);
    std::vector<std::string_view> tokens;
    for (StringTokenizer::size_type i = 0; i < st.size(); ++i) {
        tokens.emplace_back(st[i]);
    }

    std::vector<Channel::ModeChange> requested;
    for (std::size_t index = 0; index < tokens.size();) {
        const Channel::ParsedModes parsed = Channel::parseModes(
            tokens[index], std::span(tokens).subspan(index + 1),
            {.allowLeftover = true, .protocol = (Uplink != 0) ? Uplink->getProtocol() : 11});
        if (!parsed.ok()) {
            for (const std::string& problem : parsed.problems) {
                LOG(WARN, "({}): {}", theChan->getName(), problem);
            }
            return false;
        }
        requested.insert(requested.end(), parsed.changes.begin(), parsed.changes.end());
        index += 1 + parsed.argsUsed;
    }

    // A module names the target of an o or v by nick
    for (Channel::ModeChange& change : requested) {
        if (Channel::ModeType::Prefix == change.mode.type) {
            const iClient* target = Network->findNick(change.arg);
            if (0 == target) {
                return false;
            }
            change.arg = target->getCharYYXXX();
        }
    }

    return changeModes(theChan, std::move(requested), from,
                       (from != 0) ? theChan->findUser(from) : 0, olderTimestamp);
}

// Make sure the banMask is of the form nick!user@host
bool xServer::banSyntax(const string& theMask) const {
    string::size_type exPos = theMask.find('!');
    string::size_type atPos = theMask.find('@');

    if ((string::npos == exPos) || (string::npos == atPos) || (exPos > atPos)) {
        return false;
    }

    return true;
}

void xServer::UserLogin(iClient* destClient, const string& account, const unsigned int account_id,
                        const iClient::flagType flags, xClient* sourceClient) {
    assert(destClient != 0);

    if (account.empty()) {
        LOG_MSG(WARN, "Empty account name/domain for user: {client}")
            .with("client", destClient)
            .log();
        return;
    }

    destClient->setAccount(account);
    destClient->setAccountID(account_id);
    destClient->setAccountFlags(flags);

    stringstream outStream;
    outStream << getCharYY() << " AC " << destClient->getCharYYXXX() << " " << account << " "
              << account_id << " " << flags;
    Write(outStream);

    PostEvent(EVT_ACCOUNT, static_cast<void*>(destClient), 0, 0, 0, sourceClient);
}

void xServer::setBursting(bool newVal) {
    bursting = newVal;

    if (newVal) {
        // Starting bursting
        burstBytes = burstLines = 0;
    } else {
        // Completed bursting
        LOG(INFO, "Completed net burst in {} seconds, read {} bytes and processed {} commands",
            static_cast<long>(burstEnd - burstStart), burstBytes, burstLines);
    }
}

void xServer::doShutdown() {
    // elog	<< "xServer::doShutdown> Removing modules..."
    //	<< endl ;

    size_t count = 0;

    // First, remove all clients
    for (xNetwork::localClientIterator clientItr = Network->localClient_begin();
         clientItr != Network->localClient_end();) {
        ++count;
        DetachClient(clientItr++->second, "Server shutdown");
    }

    LOG_CORE(Modules, DEBUG, "Removed {} local clients", count);

    // elog	<< "xServer::doShutdown> Removing network clients..."
    //	<< endl ;

    count = 0;
    // Clear the channels
    for (xNetwork::clientIterator cItr = Network->clients_begin();
         cItr != Network->clients_end();) {
        iClient* theClient = cItr->second;
        ++count;
        ++cItr;
        delete Network->removeClient(theClient);
    }
    LOG(DEBUG, "Removed {} network clients", count);

    // elog	<< "xServer::doShutdown> Removing channels..."
    //	<< endl ;

    count = 0;
    for (xNetwork::channelIterator cItr = Network->channels_begin();
         cItr != Network->channels_end();) {
        Channel* theChan = cItr->second;
        ++count;
        ++cItr;

        LOG_MSG(DEBUG, "Found channel: {chan}").with("chan", theChan).log();

        delete Network->removeChannel(theChan);
    }
    LOG(DEBUG, "Removed {} channels", count);

    // Remove servers
    count = 0;
    while (Network->serverList_size() > 0) {
        xNetwork::serverIterator sItr = Network->servers_begin();
        ++count;

        iServer* tmpServer = sItr->second;
        delete Network->removeServer(tmpServer->getIntYY());
    }

    LOG(DEBUG, "Removed {} servers...", count);

    // elog	<< "xServer::doShutdown> Removing glines..."
    //	<< endl ;

    count = 0;
    // Remove glines
    for (glineIterator gItr = glines_begin(); gItr != glines_end();) {
        Gline* tmpGline = gItr->second;
        ++count;
        eraseGline(gItr++);
        delete tmpGline;
    }
    LOG(DEBUG, "Removed {} glines", count);

    count = 0;
    // Remove server command handlers
    for (auto cItr = commandMap.begin(); cItr != commandMap.end();) {
        ServerCommandHandler* tmpCommand = cItr->second;
        ++count;
        commandMap.erase(cItr++);
        delete tmpCommand;
    }
    LOG_CORE(Modules, DEBUG, "Removed {} server command handlers", count);

    commandMap.clear();

    count = 0;
    // Remove server command modules
    for (auto cmItr = commandModuleList.begin(); cmItr != commandModuleList.end();) {
        commandModuleType* tmpCommand = *cmItr;
        ++count;
        delete tmpCommand;
        cmItr = commandModuleList.erase(cmItr);
    }
    LOG_CORE(Modules, DEBUG, "Removed {} server command modules", count);

    commandModuleList.clear();

    // elog	<< "xServer::doShutdown> Removing timers..."
    //	<< endl ;

    count = 0;
    // All of the client timers should be cleared, but let's verify
    // The only timers left should be the server timers
    while (!timerQueue.empty()) {
        ++count;
        //	elog	<< "xServer::doShutdown> Removing a timer"
        //		<< endl ;

        // Delete the timerInfo structure, but not the TimerHandler
        // or void* data.
        delete timerQueue.top().second;
        timerQueue.pop();
    }
    LOG(DEBUG, "Removed {} timers", count);

    LOG(INFO, "Disconnecting...");

    // Close the connection
    if (serverConnection != 0) {
        ConnectionManager::Disconnect(this, serverConnection);
    }

    // Perform the optional disconnect above.
    // This will also commit the disconnects by any clients.
    ConnectionManager::Poll();

    // Deallocate the serverConnection
    // The Connection is deallocated in ConnectionManager::Poll()
}

bool xServer::DetachClient(iClient* fakeClient, const string& quitMessage) {
    assert(fakeClient != 0);

    // xNetwork::removeFakeClient() will remove the client from
    // the network data structurs, and free its numeric
    if (0 == Network->removeClient(fakeClient)) {
        LOG_MSG_CORE(Modules, ERROR,
                     "Failed to remove fakeClient from network data structures: {client}")
            .with("client", fakeClient)
            .log();
        return false;
    }

    if (!quitMessage.empty()) {
        Write("{} Q :{}", fakeClient->getCharYYXXX(), quitMessage);
    } else {
        Write("{} Q :Exiting", fakeClient->getCharYYXXX());
    }

    PostEvent(EVT_QUIT, static_cast<void*>(fakeClient));

    return true;
}

bool xServer::DetachServer(iServer* fakeServer) {
    assert(fakeServer != 0);

    if (0 == Network->removeServer(fakeServer->getIntYY())) {
        LOG_MSG(ERROR, "Failed to remove server: {server}").with("server", fakeServer).log();
        return false;
    }

    if (fakeServer->isJupe()) {
        // source_numeric JU -servername * expiration_time lastmod :reason
        // expiration: 604800 (max)
        Write("{} JU * -{} 604800 {} :{}", getCharYY(), fakeServer->getName(), ::time(0),
              fakeServer->getDescription());
    }

    else {
        Write("{} SQ {} {} :Unloading server", getCharYY(), fakeServer->getCharYY(),
              fakeServer->getConnectTime());
    }

    return true;
}

bool xServer::JoinChannel(iClient* theClient, const string& chanName) {
    assert(theClient != 0);

    if (0 == Network->findFakeClient(theClient)) {
        // Not a fake client
        LOG_MSG(WARN, "Attempt to force a non-fake client to join a channel: {client}")
            .with("client", theClient)
            .log();

        return false;
    }

    Channel* theChan = Network->findChannel(chanName);
    if (0 == theChan) {
        LOG(WARN, "Attempting to join non-existing channel: {}", chanName);
        return false;
    }

    // If the user is already on the channel, don't rejoin
    if (theChan->findUser(theClient)) {
        // User already in channel
        return true;
    }

    ChannelUser* theUser = new (std::nothrow) ChannelUser(theClient);
    assert(theUser != 0);

    if (!theChan->addUser(theUser)) {
        LOG_MSG(ERROR, "Failed to add user to channel: {chan}").with("chan", theChan).log();
        delete theUser;
        theUser = 0;
        return false;
    }

    if (!theClient->addChannel(theChan)) {
        LOG_MSG(ERROR, "Failed to add channel to client: {client}").with("client", theClient).log();

        theChan->removeUser(theUser);
        delete theUser;
        theUser = 0;
        return false;
    }

    stringstream s;
    s << theClient->getCharYYXXX() << " J " << chanName << ' ' << ::time(0);
    Write(s);

    PostChannelEvent(EVT_JOIN, theChan, static_cast<void*>(theClient), static_cast<void*>(theUser));

    return true;
}

void xServer::PartChannel(iClient* theClient, const string& chanName, const string& reason) {
    assert(theClient != 0);

    if (0 == Network->findFakeClient(theClient)) {
        // Not a fake client
        LOG_MSG(WARN, "Attempt to force a non-fake client to part a channel: {client}")
            .with("client", theClient)
            .log();

        return;
    }

    Channel* theChan = Network->findChannel(chanName);
    if (0 == theChan) {
        LOG(WARN, "Attempting to part non-existing channel: {}", chanName);
        return;
    }

    // The network knows of no membership to end if we know of none
    if (0 == theChan->findUser(theClient)) {
        LOG_MSG(WARN, "{client} is not on channel: {}", chanName).with("client", theClient).log();
        return;
    }

    delete theChan->removeUser(theClient);
    theClient->removeChannel(theChan);

    stringstream s;
    s << theClient->getCharYYXXX() << " L " << chanName;

    if (!reason.empty()) {
        s << " :" << reason;
    }
    Write(s);

    PostChannelEvent(EVT_PART, theChan, static_cast<void*>(theClient));
}

/// Have the server burst a channel
bool xServer::BurstChannel(const string& chanName, const string& chanModes,
                           const time_t& burstTime) {
    if (!bursting) {
        return false;
    }

    // Validate the modes before anything is sent.  The same typed changes
    // make up the BURST and update the channel.
    std::vector<Channel::ModeChange> modes;
    if (string::npos != chanModes.find_first_not_of(' ')) {
        StringTokenizer st(chanModes);
        std::vector<std::string_view> tokens;
        for (StringTokenizer::size_type i = 0; i < st.size(); ++i) {
            tokens.emplace_back(st[i]);
        }

        Channel::ParsedModes parsed =
            Channel::parseModes(tokens[0], std::span(tokens).subspan(1),
                                {.protocol = (Uplink != 0) ? Uplink->getProtocol() : 11});
        if (!parsed.ok()) {
            for (const std::string& problem : parsed.problems) {
                LOG(WARN, "({}): {}", chanName, problem);
            }
            return false;
        }
        for (const Channel::ModeChange& change : parsed.changes) {
            // A BURST can only set channel modes
            if (!change.set) {
                LOG(WARN, "Channel modes cannot contain a '-' polarity modifier");
                return false;
            }
            if (Channel::ModeType::Prefix == change.mode.type ||
                Channel::ModeType::List == change.mode.type) {
                LOG(WARN, "({}): mode '{}' is not a channel mode to burst", chanName,
                    change.mode.letter);
                return false;
            }
        }
        modes = std::move(parsed.changes);
    }

    Channel* theChan = Network->findChannel(chanName);
    if (0 == theChan) {
        LOG(WARN, "Channel does not exist: {}", chanName);
        return false;
    }

    if (burstTime >= theChan->getCreationTime()) {
        LOG(WARN, "Channel creation time is older than the burst time");
        return false;
    }

    // Our older timestamp wins: the network drops the channel's modes and
    // bans, and takes ours.
    theChan->removeAllModes();
    theChan->removeAllBans();

    const std::string block = Channel::burstModeBlock(modes);
    Write("{} B {} {}{}", getCharYY(), theChan->getName(), burstTime,
          block.empty() ? string() : ' ' + block);

    theChan->setCreationTime(burstTime);
    applyModesSilently(theChan, modes);

    return true;
}

bool xServer::findControlNick(const std::string& nickName) const {
    // elog	<< "xServer::findControlNick> nickName: "
    //	<< nickName
    //	<< endl ;
    return (controlNickSet.find(nickName) != controlNickSet.end());
}

void xServer::ControlCommand(iClient* srcClient, const string& message) {
    assert(srcClient != 0);

    LOG_MSG(DEBUG, "Received control message from: {client}: {}", message)
        .with("client", srcClient)
        .log();

    if (!hasControlAccess(srcClient->getAccount()) || !srcClient->isOper()) {
        // Silently return
        return;
    }

    StringTokenizer msgTokens(message);
    if (msgTokens.size() < 2) {
        Notice(srcClient, "Unable to comply");
        return;
    }

    // This is hideously ugly, if there proves to be any significant
    // need for many commands supported by the server then I will
    // use something with a better OO design (ok, so it will probably
    // get done just out of embarassment)
    const string command = string_lower(msgTokens[0]);

    // For unloadclient, the full path to the library must be specified,
    // for example: /home/gnuworld/gnuworld/lib/libstats.la
    if (msgTokens[0] == "unloadclient") {
        string reason;
        if (msgTokens.size() >= 3) {
            reason = msgTokens.assemble(2);
        }
        UnloadClient(msgTokens[1], reason);
        Notice(srcClient, string("Attempting to unload client: ") + msgTokens[1]);
        return;
    }

    if (msgTokens.size() < 3) {
        Notice(srcClient, "Unable to comply");
        return;
    }

    if (msgTokens[0] == "loadclient") {
        LoadClient(msgTokens[1], msgTokens[2]);
        Notice(srcClient, string("Attempting to load client module: ") + msgTokens[1] +
                              ", with config file: " + msgTokens[2]);
        return;
    }
}

void xServer::OnTimeout(Connection* cPtr) {
    std::stringstream s;
    s << "xServer::OnTimeout> " << *cPtr << endl;
    cout << s.str();
    if (cPtr == serverConnection) {
        /* If the connection timeout happens on the hub connection, stop the main loop in main.cc */
        lastLoop = true;

        /* The record says the same thing, without the prefix the function
         * itself carries; s keeps its own, which is what cout printed */
        std::stringstream conn;
        conn << *cPtr;
        LOG(ERROR, "{}", conn.str());
    }
}

bool xServer::hasControlAccess(const std::string& userName) const {
    return (allowControlSet.find(userName) != allowControlSet.end());
}

bool xServer::Message(iClient* theClient, const string& message) {
    assert(theClient != 0);

    if (message.empty() || !isConnected()) {
        return false;
    }

    return sendText(TextType::PRIVMSG, nullptr, theClient->getCharYYXXX(), message);
}

bool xServer::serverMessage(Channel* theChan, const string& message) {
    assert(theChan != 0);

    if (message.empty() || !isConnected()) {
        return false;
    }

    return sendText(TextType::PRIVMSG, nullptr, theChan->getName(), message);
}

bool xServer::SendNumeric(unsigned int numeric, const iClient* to, const string& text) {
    assert(to != 0);
    return isConnected() && Write("{} {:03} {} {}", getCharYY(), numeric, to->getCharYYXXX(), text);
}

bool xServer::SetNetConf(const string& key, const string& value) {
    if (key.empty() || !isConnected()) {
        return false;
    }
    const time_t now = ::time(0);
    Network->addNetConf(key, value, now);
    return Write("{} CF {} {} :{}", getCharYY(), static_cast<long>(now), key, value);
}

void xServer::UpdateAccountFlags(iClient* theClient, iClient::flagType flags) {
    assert(theClient != 0);

    if (Uplink != 0 && Uplink->getProtocol() >= 11) {
        Write("{} AC {} {} {} {}", getCharYY(), theClient->getCharYYXXX(), theClient->getAccount(),
              theClient->getAccountID(), static_cast<unsigned int>(flags));
    }
    theClient->setAccountFlags(flags);
}

bool xServer::isOurClient(const iClient* theClient) const {
    if (0 == theClient) {
        return false;
    }
    if (theClient->getIntYY() == getIntYY()) {
        return true;
    }
    return Network->findFakeClient(const_cast<iClient*>(theClient)) != 0;
}

bool xServer::OpMode(iClient* target, const string& userModes) {
    assert(target != 0);

    if (userModes.empty() || !isConnected()) {
        return false;
    }
    return Write("{} OM {} :{}", getCharYY(), target->getCharYYXXX(), userModes);
}

bool xServer::GlobalNotice(const string& text, const iClient* from) {
    if (text.empty() || !isConnected()) {
        return false;
    }

    // "$*": every server, which is to say every user
    return sendText(TextType::NOTICE, from, "$*", text);
}

bool xServer::Notice(iClient* theClient, const string& message) {
    assert(theClient != 0);

    if (message.empty() || !isConnected()) {
        return false;
    }

    return sendText(TextType::NOTICE, nullptr, theClient->getCharYYXXX(), message);
}

bool xServer::serverNotice(Channel* theChan, const string& Message) {
    assert(theChan != 0);

    if (Message.empty() || !isConnected()) {
        return false;
    }

    return sendText(TextType::NOTICE, nullptr, theChan->getName(), Message);
}

bool xServer::XReply(iServer* theServer, const string& Routing, const string& Message) {
    assert(theServer != 0);

    if (Message.empty() || !isConnected()) {
        return false;
    }

    /* If we are sending an XREPLY to ourselves; PostEvent instead. */
    if (theServer == getMe()) {
        void* const thisRouting = const_cast<char*>(Routing.c_str());
        void* const thisMessage = const_cast<char*>(Message.c_str());
        PostEvent(EVT_XREPLY, static_cast<void*>(theServer), reinterpret_cast<void*>(thisRouting),
                  reinterpret_cast<void*>(thisMessage));
        return true;
    }

    stringstream s;
    s << getCharYY() << " XR " << theServer->getCharYY() << " " << Routing << " :" << Message;
    return Write(s.str());
}

bool xServer::XQuery(iServer* theServer, const string& Routing, const string& Message) {
    assert(theServer != 0);

    if (Message.empty() || !isConnected()) {
        return false;
    }

    /* If we are sending an XQUERY to ourselves; PostEvent instead. */
    if (theServer == getMe()) {
        void* const thisRouting = const_cast<char*>(Routing.c_str());
        void* const thisMessage = const_cast<char*>(Message.c_str());
        PostEvent(EVT_XQUERY, static_cast<void*>(theServer), reinterpret_cast<void*>(thisRouting),
                  reinterpret_cast<void*>(thisMessage));
        return true;
    }

    stringstream s;
    s << getCharYY() << " XQ " << theServer->getCharYY() << " " << Routing << " :" << Message;
    return Write(s.str());
}
} // namespace gnuworld
