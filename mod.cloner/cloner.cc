/**
 * cloner.cc
 * Load fake clones for testing or fun.
 *
 * Copyright (C) 2002 Daniel Karrels <dan@karrels.com>
 *		      Reed Loden <reed@reedloden.com>
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
 * Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA 02111-1307,
 * USA.
 *
 * $Id: cloner.cc,v 1.36 2005/01/12 04:36:45 dan_karrels Exp $
 */

#include	<new>
#include	<list>
#include	<vector>
#include	<iostream>
#include	<sstream>
#include	<string>
#include	<algorithm>

#include	<ctime>
#include	<cstdlib>
#include	<cmath>

#include	"client.h"
#include	"iClient.h"
#include	"cloner.h"
#include	"EConfig.h"
#include	"ip.h"
#include	"Network.h"
#include	"StringTokenizer.h"
#include	"misc.h"
#include	"ELog.h"

namespace gnuworld
{

using std::vector ;
using std::endl ;
using std::stringstream ;
using std::string ;

/*
 *  Exported function used by moduleLoader to gain an
 *  instance of this module.
 */

extern "C"
{
  xClient* _gnuwinit(const string& args)
  { 
    return new cloner( args );
  }

} 
 
cloner::cloner( const string& configFileName )
 : xClient( configFileName )
{
EConfig conf( configFileName ) ;

cloneDescription = conf.Require( "clonedescription" )->second ;
cloneMode = conf.Require( "clonemode" )->second ;
fakeServerName = conf.Require( "fakeservername" )->second ;
fakeServerDescription = conf.Require( "fakeserverdescription" )->second ;

allowOpers = false ;
string stringOperAccess = conf.Require( "allow_opers" )->second ;
if( !strcasecmp( stringOperAccess, "yes" ) ||
	!strcasecmp( stringOperAccess, "true" ) )
	{
	allowOpers = true ;
	}

EConfig::const_iterator ptr = conf.Find( "permit_user" ) ;
while( ptr != conf.end() && ptr->first == "permit_user" )
	{
	allowAccess.push_back( ptr->second ) ;
	++ptr ;
	}

cloneBurstCount = atoi( conf.Require( "cloneburstcount" )->second.c_str() ) ;
if( cloneBurstCount < 1 )
	{
	elog	<< "cloner> cloneBurstCount must be at least 1"
		<< endl ;
	::exit( 0 ) ;
	}

ptr = conf.Find( "fakehost" ) ;
while( ptr != conf.end() && ptr->first == "fakehost" )
	{
	hostNames.push_back( ptr->second ) ;
	++ptr ;
	}

if( hostNames.empty() )
	{
	elog	<< "cloner> Must specify at least one hostname"
		<< endl ;
	::exit( 0 ) ;
	}

ptr = conf.Find( "fakeuser" ) ;
while( ptr != conf.end() && ptr->first == "fakeuser" )
	{
	userNames.push_back( ptr->second ) ;
	++ptr ;
	}

if( userNames.empty() )
	{
	elog	<< "cloner> Must specify at least one username"
		<< endl ;
	::exit( 0 ) ;
	}

ptr = conf.Find( "spam" ) ;
while( ptr != conf.end() && ptr->first == "spam" )
	{
	spamList.push_back( ptr->second ) ;
	++ptr ;
	}

if( spamList.empty() )
	{
	elog	<< "cloner> Must specify at least one spam line"
		<< endl ;
	::exit( 0 ) ;
	}

playChan = conf.Require( "playchan" )->second ;

makeCloneCount = 0 ;
playCloneCount = 0 ;

// Let's make sure we don't get all flood at the same time
spamFloodCount = 0 ;
actionFloodCount = 1 ;
nickFloodCount = 2 ;
kickFloodCount = 3 ;
topicFloodCount = 4 ;
opFloodCount = 5 ;
deopFloodCount = 6 ;
joinFloodCount = 7 ;

doKill = true ;

spamInterval = atoi( conf.Require( "spaminterval" )->second.c_str() ) ;
cycleInterval = atoi( conf.Require( "cycleinterval" )->second.c_str() ) ;
quitInterval = atoi( conf.Require( "quitinterval" )->second.c_str() ) ;
opInterval = atoi( conf.Require( "opsinterval" )->second.c_str() ) ;
topicInterval = atoi( conf.Require( "topicinterval" )->second.c_str() ) ;
nickInterval = atoi( conf.Require( "nickchangeinterval" )->second.c_str() ) ;
killInterval = atoi( conf.Require( "killinterval" )->second.c_str() ) ;

floodLines = atoi( conf.Require( "floodlines" )->second.c_str() ) ;
floodCount = atoi( conf.Require( "floodcount" )->second.c_str() ) ;
playOps = atoi( conf.Require( "playops" )->second.c_str() ) ;
playOpers = atoi( conf.Require( "playopers" )->second.c_str() ) ;

minNickLength = atoi( conf.Require( "minnicklength" )->second.c_str() ) ;
maxNickLength = atoi( conf.Require( "maxnicklength" )->second.c_str() ) ;

if( playChan[0] != '#' && playChan != "0" )
	{
	elog	<< "cloner> invalid channel name for playchan"
			<< endl ;
	::exit( 0 );
	}

if( minNickLength < 1 )
	{
	elog	<< "cloner> minNickLength must be at least 1"
			<< endl ;
	::exit( 0 );
	}
if( maxNickLength <= minNickLength )
	{
	elog	<< "cloner> minNickLength must be less than maxNickLength"
			<< endl ;
	::exit( 0 ) ;
	}
}

cloner::~cloner()
{}

void cloner::OnAttach()
{
MyUplink->RegisterEvent( EVT_KILL, this );
}

void cloner::OnConnect()
{
fakeServer = new (std::nothrow) iServer(
	MyUplink->getIntYY(), // uplinkIntYY
	string( "00]]]" ),
	fakeServerName,
	::time( 0 ),
	fakeServerDescription ) ;
assert( fakeServer != 0 ) ;

MyUplink->AttachServer( fakeServer, this ) ;

xClient::OnConnect() ;
}

void cloner::OnEvent( const eventType& theEvent,
	void*, void* Data2, void*, void* )
{
switch ( theEvent )
	{
	case EVT_KILL:
		{
		iClient* tmpUser = static_cast< iClient* >( Data2 ) ;

    	auto pos = std::find( clones.begin() , clones.end() , tmpUser ) ;
		if( pos != clones.end() ) 
			{
			clones.remove( tmpUser ) ;
			}

		break;
		}
	default:
		break;
	}
}

void cloner::OnPrivateMessage( iClient* theClient, const string& Message,
	bool )
{
//elog << "cloner::OnPrivateMessage> " << Message << endl ;

// Get rid of anyone who is not an oper and does not have access
bool userHasAccess = hasAccess( theClient->getAccount() ) ;
if( !userHasAccess && !theClient->isOper() )
	{
	// Normal user
	return ;
	}

if( !userHasAccess )
	{
	// The client must be an oper
	// Are opers allow to use the service?
	if( !allowOpers )
		{
		// Nope
		return ;
		}
	}

StringTokenizer st( Message ) ;
if( st.empty() )
	{
	return ;
	}

string command( string_upper( st[ 0 ] ) ) ;
string topic ;

if( st.size() > 1 )
	{
	topic = string_upper( st[ 1 ] ) ;
	}

//elog	<< "cloner::OnPrivateMessage> command: "
//	<< command
//	<< ", topic: "
//	<< topic
//	<< endl ;

if( command == "SHOWCOMMANDS" )
	{
	if( st.size() < 1 )
		{
		Notice( theClient, "Usage: %s", command.c_str() ) ;
		return ;
		}
	if( st.size() >= 1 )
		{
		Notice( theClient, "_-=[Cloner Commands]=-_" ) ;
		Notice( theClient, "LOADCLONES JOINALL JOIN PARTALL "
			"KILLALL/QUITALL SAYALL/MSGALL PLAY "
			"ACTALL/DOALL/DESCRIBEALL NOTICEALL" ) ;
		Notice( theClient, "_-=[End of Cloner Commands]=-_" ) ;
		}
	}
else if( command == "HELP" )
	{
	if( st.size() < 1 )
		{
		Notice( theClient, "Usage: %s <topic>",
			command.c_str() ) ;
		return ;
		}

	if( topic == "SHOWCOMMANDS" )
		{
		Notice( theClient, "%s - Shows a list of all commands",
			topic.c_str() ) ;
		}
	else if( topic == "HELP" )
		{
		Notice( theClient, "%s <topic> - Gives help on a topic",
			topic.c_str() ) ;
		}
	else if( topic == "LOADCLONES" )
		{
		Notice( theClient, "%s <# of clones> - Queue creation "
			"  of clone(s)", topic.c_str() ) ;
		}
	else if( topic == "JOINALL" )
		{
		Notice( theClient, "%s <#channel> - Make all clones "
			"/join a #channel", topic.c_str() ) ;
		}
	else if( topic == "JOIN" )
		{
		Notice( theClient, "%s <#channel> <# of clones> - Make clones "
			"/join a #channel", topic.c_str() ) ;
		}
	else if( topic == "PARTALL" )
		{
		Notice( theClient, "%s <#channel> [reason] - Make "
			"all clones /part a #channel with an optional "
			"reason", topic.c_str() ) ;
		}
	else if( topic == "PART" )
		{
		Notice( theClient, "%s <#channel> <# of clones> [reason] - Makes clones "
			"/part a #channel with an optional reason", topic.c_str() ) ;
		}
	else if( topic == "KILLALL" || topic == "QUITALL" )
		{
		Notice( theClient, "%s [reason] - Make all "
			"clones /quit with an optional reason",
			topic.c_str() ) ;
		}
	else if( topic == "SAYALL" || topic == "MSGALL" )
		{
		Notice( theClient, "%s <#channel/nickname> "
			"<message> - Make all clones /msg a #channel or "
			"nickname", topic.c_str() ) ;
		}
	else if( topic == "PLAY" )
		{
		Notice( theClient, "%s <# of clones/OFF> - Make clones "
			"create random activity on a #channel at the defined intervals", topic.c_str() ) ;
		}
	else if( topic == "ACTALL" || topic == "DOALL" || topic ==
		"DESCRIBEALL" )
		{
		Notice( theClient, "%s <#channel/nickname> <action> - "
			"Make all clones /me a channel or nickname",
			topic.c_str() ) ;
		}
	else if( topic == "NOTICEALL" )
		{
		Notice( theClient, "%s <#channel/nickname> "
			"<notice> - Make all clones /notice a #channel "
			"or nickname", topic.c_str() ) ;
		}
	} // "HELP"
else if( command == "LOADCLONES" )
	{
	if( st.size() < 2 )
		{
		Notice( theClient, "Usage: %s <# of clones>",
			command.c_str() ) ;
		return ;
		}

	int numClones = atoi( st[ 1 ].c_str() ) ;
	if( numClones < 1 )
		{
		Notice( theClient,
			"LOADCLONES: Invalid number of clones" ) ;
		return ;
		}

	if( 0 == makeCloneCount )
		{
		loadCloneTimer = MyUplink->RegisterTimer( ::time( 0 ) + 1, this, 0 ) ;
		}

	makeCloneCount += static_cast< size_t >( numClones ) ;
//	elog	<< "cloner::OnPrivateMessage> makeCloneCount: "
//		<< makeCloneCount
//		<< endl ;

	Notice( theClient, "Queuing %d Clones", numClones ) ;
	} // "LOADCLONES"
else if( command == "JOINALL" )
	{
	if( st.size() < 2 )
		{
		Notice( theClient, "Usage: %s <#channel>",
			command.c_str() ) ;
		return ;
		}

	string chanName( st[ 1 ] ) ;
	if( chanName[ 0 ] != '#' )
		{
		chanName.insert( chanName.begin(), '#' ) ;
		}

	// Is the channel the playChan? Then use the PLAY command!
	if( strcasecmp( chanName.c_str(), playChan.c_str() ) == 0 )
		{
		Notice( theClient, "That's the play channel! Please use the PLAY command." ) ;
		return ;
		}

	// Does the channel already exist?
	Channel* theChan = Network->findChannel( chanName ) ;
	if( NULL == theChan )
		{
		theChan = new (std::nothrow)
			Channel( chanName, ::time( 0 ) ) ;
		assert( theChan != 0 ) ;

		// Add the channel to the network tables
		if( !Network->addChannel( theChan ) )
			{
			delete theChan ; theChan = 0 ;
			}
		}

	for( std::list< iClient* >::const_iterator ptr = clones.begin(),
		endPtr = clones.end() ; ptr != endPtr ; ++ptr )
		{
		stringstream s ;
		s	<< (*ptr)->getCharYYXXX()
			<< " J "
			<< chanName
			<< " "
			<< theChan->getCreationTime() ;

		MyUplink->Write( s ) ;

		// Creating ChannelUser, and adding to network table.
		ChannelUser* theUser =
			new (std::nothrow) ChannelUser( *ptr ) ;
		assert( theUser != 0 ) ;

		if( !theChan->addUser( theUser ) )
			{
			elog << "clone::OnPrivateMessage> Failed to addUser()." << endl ;
			delete theUser ; theUser = 0 ;
			}

		if( !(*ptr)->addChannel( theChan ) )
			{
			elog << "clone::OnPrivateMessage> Failed to addChannel()." << endl ;
			theChan->removeUser( *ptr ) ;
			delete theUser ; theUser = 0 ;
			}
		}
	} // JOINALL
else if( command == "JOIN" )
	{
	if( st.size() < 3 )
		{
		Notice( theClient, "Usage: %s <#channel> <# of clones>",
			command.c_str() ) ;
		return ;
		}

	size_t numClones = atoi( st[ 2 ].c_str() ) ;
	if( numClones < 1 || numClones > clones.size() )
		{
		Notice( theClient,
			"%s: Invalid number of clones", command.c_str() ) ;
		return ;
		}

	string chanName( st[ 1 ] ) ;

	if( chanName[ 0 ] != '#' )
		{
		chanName.insert( chanName.begin(), '#' ) ;
		}

	// Is the channel the playChan? Then use the PLAY command!
	if( strcasecmp( chanName.c_str(), playChan.c_str() ) == 0 )
		{
		Notice( theClient, "That's the play channel! Please use the PLAY command." ) ;
		return ;
		}

	// Does the channel already exist?
	Channel* theChan = Network->findChannel( chanName ) ;
	if( NULL == theChan )
		{
		theChan = new (std::nothrow)
			Channel( chanName, ::time( 0 ) ) ;
		assert( theChan != 0 ) ;

		// Add the channel to the network tables
		if( !Network->addChannel( theChan ) )
			{
			delete theChan ; theChan = 0 ;
			}
		}

	for( size_t i = 0 ; i < numClones ; i++ )
		{
		iClient* theClone = availableClone( theChan ) ;
		if( NULL == theClone ) break ;

		stringstream s ;
		s	<< theClone->getCharYYXXX()
			<< " J "
			<< chanName 
			<< " "
			<< theChan->getCreationTime() ;

		MyUplink->Write( s ) ;

		// Creating ChannelUser, and adding to network table. 
		ChannelUser* theUser =
			new (std::nothrow) ChannelUser( theClone ) ;
		assert( theUser != 0 ) ;

		if( !theChan->addUser( theUser ) )
			{
			elog << "clone::OnPrivateMessage> Failed to addUser()." << endl ;
			delete theUser ; theUser = 0 ;
			}

		if( !theClone->addChannel( theChan ) )
			{
			elog << "clone::OnPrivateMessage> Failed to addChannel()." << endl ;
			theChan->removeUser( theClone ) ;
			delete theUser ; theUser = 0 ;
			}
		}
	} // JOIN
else if( command == "PARTALL" )
	{
	if( st.size() < 2 )
		{
		Notice( theClient, "Usage: %s <#channel> [reason]",
			command.c_str() ) ;
		return ;
		}

	string chanName( st[ 1 ] ) ;
	if( chanName[ 0 ] != '#' )
		{
		chanName.insert( chanName.begin(), '#' ) ;
		}

	Channel* theChan = Network->findChannel ( chanName ) ;
	if( NULL == theChan )
		{
		Notice( theClient, "I cannot find any information about that channel." ) ;
		return ;
		}

	for( std::list< iClient* >::const_iterator ptr = clones.begin(),
		endPtr = clones.end() ; ptr != endPtr ; ++ptr )
		{
		ChannelUser* tmpUser = theChan->findUser( *ptr ) ;
		if( tmpUser != NULL )
			{
			iClient* tmpClone = Network->findClient( tmpUser->getCharYYXXX() ) ;
			if( NULL == tmpClone ) continue ;

			stringstream s ;
			s	<< tmpUser->getCharYYXXX()
				<< " L "
				<< chanName ;

			if( st.size() > 3 )
				{
				s	<< " :"
					<< st.assemble( 2 ).c_str() ;
				}

			MyUplink->Write( s ) ;

			theChan->removeUser( tmpClone ) ;
			delete tmpUser ; tmpUser = 0 ;

			tmpClone->removeChannel( theChan ) ;
			}
		}
	} // PARTALL
else if( command == "PART" )
	{
	if( st.size() < 3 )
		{
		Notice( theClient, "Usage: %s <#channel> <# of clones> [reason]",
			command.c_str() ) ;
		return ;
		}

	size_t numClones = atoi( st[ 2 ].c_str() ) ;
	if( numClones < 1 )
		{
		Notice( theClient,
			"%s: Invalid number of clones", command.c_str() ) ;
		return ;
		}

	string chanName( st[ 1 ] ) ;

	if( chanName[ 0 ] != '#' )
		{
		chanName.insert( chanName.begin(), '#' ) ;
		}

	// Is the channel the playChan? Then use the PLAY command!
	if( strcasecmp( chanName.c_str(), playChan.c_str() ) == 0 )
		{
		Notice( theClient, "That's the play channel! Please use the PLAY command." ) ;
		return ;
		}

	Channel* theChan = Network->findChannel( chanName ) ;
	if( NULL == theChan )
		{
		Notice( theClient, "I cannot find any information about that channel." ) ;
		return ;
		}

	size_t i { } ;
	for( std::list< iClient* >::const_iterator ptr = clones.begin(),
		endPtr = clones.end() ; ptr != endPtr ; ++ptr )
		{
		if( i == numClones ) break ;

		ChannelUser* tmpUser = theChan->findUser( *ptr ) ;
		if( tmpUser != NULL )
			{
			iClient* tmpClone = Network->findClient( tmpUser->getCharYYXXX() ) ;
			if( NULL == tmpClone ) continue ;

			stringstream s ;
			s	<< tmpUser->getCharYYXXX()
				<< " L "
				<< chanName ;

			if( st.size() > 3 )
				{
				s	<< " :"
					<< st.assemble( 3 ).c_str() ;
				}

			MyUplink->Write( s ) ;

			theChan->removeUser( tmpClone ) ;
			delete tmpUser ; tmpUser = 0 ;

			tmpClone->removeChannel( theChan ) ;
			}
		i++ ;
		}
	} // PART
else if( command == "KILLALL" || command == "QUITALL" )
	{
	if( st.size() < 1 )
    	{
        Notice( theClient, "Usage: %s [reason]",
			command.c_str() ) ;
        return ;
        }

	string quitMsg ;
	if( st.size() >= 2 )
		{
		quitMsg = st.assemble( 1 ) ;
		}

	for( std::list< iClient* >::const_iterator ptr = clones.begin(),
		endPtr = clones.end() ; ptr != endPtr ; ++ptr )
		{
		if( MyUplink->DetachClient( *ptr, quitMsg ) )
			{
			delete *ptr ;
			}
		}
	clones.clear() ;

	if( playCloneCount > 0 )
		deactivatePlay() ;

	} // KILLALL/QUITALL
else if( command == "SAYALL" || command == "MSGALL" )
	{
	if( st.size() < 3 )
		{
		Notice( theClient, "Usage: %s <#channel/nickname> "
			"<message>", command.c_str() ) ;
		return ;
		}

	string chanOrNickName( st[ 1 ] ) ;
	string privMsg( st.assemble(2).c_str() ) ;

	if( chanOrNickName[ 0 ] != '#' )
		{ // Assume nickname
		iClient* Target = Network->findNick( st[ 1 ] ) ;
		if( NULL == Target )
			{
			Notice( theClient, "Unable to find nick: %s"
				, st[ 1 ].c_str() ) ;
			return ;
			}
		chanOrNickName = Target->getCharYYXXX();
		}

	for( std::list< iClient* >::const_iterator ptr = clones.begin(),
		endPtr = clones.end() ; ptr != endPtr ; ++ptr )
		{
		stringstream s ;
		s	<< (*ptr)->getCharYYXXX()
			<< " P "
			<< chanOrNickName
			<< " :"
			<< privMsg ;

		MyUplink->Write( s ) ;
		}
	} // SAYALL/MSGALL
else if( command == "PLAY" )
	{
	if( st.size() < 2 )
		{
		Notice( theClient, "Usage: %s <[# of clones]/OFF>", command.c_str() ) ;
		return ;
		}

	if( playChan == "0" )
		{
		Notice( theClient, "%s: Please specify a play channel in the config file", command.c_str() ) ;
		return ;
		}

	Channel* theChan = Network->findChannel( playChan ) ;

	if( string_upper( st[ 1 ] ) == "OFF" )
		{
		/* Checking if PLAY is already active. */ 
		if( 0 == playCloneCount )
			{
			Notice ( theClient, "%s is already OFF", command.c_str() ) ;
			return;
			}

		// Parting the playchan.
		for( std::list< iClient* >::const_iterator ptr = clones.begin(),
			endPtr = clones.end() ; ptr != endPtr ; ++ptr )
			{
			ChannelUser* theUser = theChan->findUser( *ptr ) ;
			if( NULL == theUser ) continue ;

			stringstream s ;
			s	<< (*ptr)->getCharYYXXX()
				<< " L "
				<< theChan->getName() 
				<< ": PLAY deactivated!" ;

			MyUplink->Write( s ) ;

			theChan->removeUser( *ptr ) ;
			delete theUser ; theUser = 0 ;

			(*ptr)->removeChannel( theChan ) ;
			}

		if( deactivatePlay() )
			Notice( theClient, "%s is deactivated.", command.c_str() ) ;
		}
	else
		{
		/* Checking if PLAY is already active. */ 
		if( playCloneCount > 0 )
			{
			Notice ( theClient, "%s is already activated.", command.c_str() ) ;
			return;
			}

		playCloneCount = atoi( st[ 1 ].c_str() ) ;

		/* The number of clones must be at least 1, and not exceed the number
		 * of clones loaded. */
		if( playCloneCount < 1 || playCloneCount > clones.size() )
			{
			Notice( theClient,
				"%s: Invalid number of clones", command.c_str() ) ;
			playCloneCount = 0 ;
			return ;
			}

		// Does the channel already exist?
		Channel* theChan = Network->findChannel( playChan ) ;
		if( NULL == theChan )
			{
			theChan = new (std::nothrow)
				Channel( playChan, ::time( 0 ) ) ;
			assert( theChan != 0 ) ;

			// Add the channel to the network tables
			if( !Network->addChannel( theChan ) )
				{
				delete theChan ; theChan = 0 ;
				}
			}

		// Joining the playchan.
		size_t i { } ;
		int j { }, k { } ;
		for( std::list< iClient* >::const_iterator ptr = clones.begin(),
			endPtr = clones.end() ; ptr != endPtr ; ++ptr )
			{
			if( i > playCloneCount) // Stop when we have reached the number of clones.
				break ;

			stringstream s ;
			s	<< (*ptr)->getCharYYXXX()
				<< " J "
				<< theChan->getName() 
				<< " "
				<< theChan->getCreationTime() ;

			MyUplink->Write( s ) ;

			// Creating ChannelUser, and adding to network table. 
			ChannelUser* theUser =
				new (std::nothrow) ChannelUser( *ptr ) ;
			assert( theUser != 0 ) ;

			if( !theChan->addUser( theUser ) )
				{
				elog << "cloner::OnPrivateMessage> Failed to addUser()." << endl ;
				delete theUser ; theUser = 0 ;
				}

			if( !(*ptr)->addChannel( theChan ) )
				{
				elog << "cloner::OnPrivateMessage> Failed to addChannel()." << endl ;

				theChan->removeUser( *ptr ) ;
				delete theUser ; theUser = 0 ;
				}

			// Let's op a percentage of the clones.
			if( playOps > 0 && ( k < round( ( playCloneCount / 100 ) * playOps ) + 1 ) )
				{
				stringstream s ;
				s	<< MyUplink->getCharYY()
					<< " M "
					<< theChan->getName()
					<< " +o "
					<< (*ptr)->getCharYYXXX() 
					<< " "
					<< theChan->getCreationTime() ;

					MyUplink->Write( s ) ;

					theUser->setModeO() ;
				}

			// Let's oper a percentage of the clones.
			if( playOpers > 0 && ( j < round( ( playCloneCount / 100 ) * playOpers ) + 1 ) )
				{
				stringstream s ;
				s	<< (*ptr)->getCharYYXXX()
					<< " M "
					<< (*ptr)->getNickName() 
					<< " :+o" ;

					MyUplink->Write( s ) ;

					(*ptr)->setModeO() ;
				}
			i++ ; j++ ; k++ ;
			}

		// Activating timers.
		if( spamInterval > 0 )
			{
			spamTimer = MyUplink->RegisterTimer( ::time( 0 ) + spamInterval, this, 0 ) ;
			actionTimer = MyUplink->RegisterTimer( ::time( 0 ) + spamInterval + 5, this, 0 ) ;
			}

		if( cycleInterval > 0)
			{
			joinTimer = MyUplink->RegisterTimer( ::time( 0 ) + ( cycleInterval / 2 ) + 10, this, 0 ) ;
			partTimer = MyUplink->RegisterTimer( ::time( 0 ) + cycleInterval + 15, this, 0 ) ;
			kickTimer = MyUplink->RegisterTimer( ::time( 0 ) + cycleInterval + 35, this, 0 ) ;
			}

		if( quitInterval > 0 )
			quitTimer = MyUplink->RegisterTimer( ::time( 0 ) + quitInterval + 20, this, 0 ) ;

		if( opInterval > 0 )
			{
			opTimer = MyUplink->RegisterTimer( ::time( 0 ) + opInterval + 25, this, 0 ) ;
			deopTimer = MyUplink->RegisterTimer( ::time( 0 ) + ( opInterval / 2 ) + 30, this, 0 ) ;
			}

		if( topicInterval > 0 )
			topicTimer = MyUplink->RegisterTimer( ::time( 0 ) + topicInterval + 40, this, 0 ) ;

		if( nickInterval > 0 )
			nickTimer = MyUplink->RegisterTimer( ::time( 0 ) + nickInterval + 45, this, 0 ) ;

		if( killInterval > 0 )
			killTimer = MyUplink->RegisterTimer( ::time( 0 ) + ( killInterval / 2 ) + 50, this, 0 ) ;

		Notice( theClient, "%s is now activated on %s for %d clones.", command.c_str(), playChan.c_str(), playCloneCount ) ;
		}
	} // PLAY
else if( command == "ACTALL" || command == "DOALL" ||
	command == "DESCRIBEALL" )
	{
	if( st.size() < 3 )
		{
		Notice( theClient, "Usage: %s <#channel/nickname> "
			"<action>", command.c_str() ) ;
		return ;
		}

	string chanOrNickName( st[ 1 ] ) ;
	string action( st.assemble(2).c_str() ) ;

	if( chanOrNickName[ 0 ] != '#' )
		{ // Assume nickname
		iClient* Target = Network->findNick( st[ 1 ] ) ;
		if( NULL == Target )
			{
			Notice( theClient, "Unable to find nick: %s"
				, st[ 1 ].c_str() ) ;
			return ;
			}
		chanOrNickName = Target->getCharYYXXX();
		}

	for( std::list< iClient* >::const_iterator ptr = clones.begin(),
		endPtr = clones.end() ; ptr != endPtr ; ++ptr )
		{
		stringstream s ;
		s	<< (*ptr)->getCharYYXXX()
			<< " P "
			<< chanOrNickName
			<< " :\001ACTION "
			<< action
			<< "\001" ;

		MyUplink->Write( s ) ;
		}
	} // ACTALL/DOALL/DESCRIBEALL
else if( command == "NOTICEALL" )
	{
	if( st.size() < 3 )
		{
		Notice( theClient, "Usage: %s <#channel/nickname> "
			"<notice>", command.c_str() ) ;
		return ;
		}

	string chanOrNickName( st[ 1 ] ) ;
	string notice( st.assemble(2).c_str() ) ;

	if( chanOrNickName[ 0 ] != '#' ) 
		{ // Assume nickname
		iClient* Target = Network->findNick( st[ 1 ] ) ;
		if( NULL == Target )
			{
			Notice( theClient, "Unable to find nick: %s"
				, st[ 1 ].c_str() ) ;
			return ;
		    }
		chanOrNickName = Target->getCharYYXXX();
		}
	
	for( std::list< iClient* >::const_iterator ptr = clones.begin(),
		endPtr = clones.end() ; ptr != endPtr ; ++ptr )
		{
		stringstream s ;
		s	<< (*ptr)->getCharYYXXX()
			<< " O "
			<< chanOrNickName
			<< " :"
			<< notice ;

		MyUplink->Write( s ) ;
		}
	} // NOTICEALL
}

void cloner::OnTimer( const xServer::timerID& timer_id, void* )
{
//elog	<< "cloner::OnTimer> makeCloneCount: "
//	<< makeCloneCount
//	<< endl ;

if( timer_id == loadCloneTimer )
	{
	if( 0 == makeCloneCount )
		{
		return ;
		}

	size_t cloneCount = makeCloneCount ;
	if( cloneCount > cloneBurstCount )
		{
		// Too many
		cloneCount = cloneBurstCount ;
		}

	makeCloneCount -= cloneCount ;

	//elog	<< "cloner::OnTimer> loadCloneTimer: "
	//	<< cloneCount
	//	<< endl ;

	for( size_t i = 0 ; i < cloneCount ; ++i )
		{
		addClone() ;
		}

	if( makeCloneCount > 0 )
		{
		loadCloneTimer = MyUplink->RegisterTimer( ::time( 0 ) + 1, this, 0 ) ;
		}
	}
else if( timer_id == spamTimer )
	{
	Channel* theChan = Network->findChannel( playChan ) ;
	iClient* theClone = randomChanClone( theChan ) ;
	ChannelUser* theUser = theChan->findUser ( theClone ) ;

	if( theClone != NULL && ( !theChan->getMode( Channel::MODE_M ) || (theUser->isModeV() ) ) )
		{
		int tmp { 1 } ;
		if( spamFloodCount > floodCount )
			{
			tmp = floodLines ;
			spamFloodCount = 0 ;
			}

		for( int i = 0 ; i < tmp ; i++ )
			{
			stringstream s ;
			s	<< theClone->getCharYYXXX()
				<< " P "
				<< theChan->getName()
				<< " :"
				<< randomSpam() ;	

			MyUplink->Write( s ) ;
			}
			
		spamFloodCount++ ;
		}

	spamTimer = MyUplink->RegisterTimer( ::time( 0 ) + spamInterval, this, 0 ) ;
	}
else if( timer_id == actionTimer )
	{
	Channel* theChan = Network->findChannel( playChan ) ;
	iClient* theClone = randomChanClone( theChan ) ;
	ChannelUser* theUser = theChan->findUser ( theClone ) ;

	if( theClone != NULL && ( !theChan->getMode( Channel::MODE_M ) || (theUser->isModeV() ) ) )
		{
		int tmp { 1 } ;
		if( actionFloodCount > floodCount )
			{
			tmp = floodLines ;
			actionFloodCount = 0 ;
			}

		for( int i = 0 ; i < tmp ; i++ )
			{
			stringstream s ;
			s	<< theClone->getCharYYXXX()
				<< " P "
				<< theChan->getName()
				<< " :\001ACTION "
				<< randomSpam() 
				<< "\001" ;	

			MyUplink->Write( s ) ;
			}
			
		actionFloodCount++ ;
		}

	actionTimer = MyUplink->RegisterTimer( ::time( 0 ) + ( spamInterval * 4 ), this, 0 ) ;
	}
else if( timer_id == joinTimer )
	{
	Channel* theChan = Network->findChannel( playChan ) ;

	int tmp { 1 } ;
	/* 
	 * To callibrate joins and parts, we double the floodCount for join flood.
	 * The reason is that kicks occur at cycleInterval * 2. 
	 * Join flood and kick flood will therefore be 1:1. 
	 */
	if( joinFloodCount > ( floodCount * 2 ) )
		{
		tmp = floodLines ;
		joinFloodCount = 0 ;
		}

	for( int i = 0 ; i < tmp ; i++ )
		{
		iClient* theClone = availableClone( theChan ) ; 
		if( theClone == NULL ) break ;

		stringstream s ;
		s	<< theClone->getCharYYXXX()
			<< " J "
			<< theChan->getName()
			<< " "
			<< theChan->getCreationTime() ;

		MyUplink->Write( s ) ;

		// Creating ChannelUser, and adding to network table. 
		ChannelUser* theUser =
			new (std::nothrow) ChannelUser( theClone ) ;
		assert( theUser != 0 ) ;

		if( !theChan->addUser( theUser ) )
			{
			delete theUser ; theUser = 0 ;
			}

		if( !theClone->addChannel( theChan ) )
			{
			theChan->removeUser( theClone ) ;
			delete theUser ; theUser = 0 ;
			}

		joinFloodCount++ ;
		}
	/*
	 * We need to maintain the desired number of clones on the playchan. 
	 * The interval for joins adds up with the interval for parts and kicks initiated by
	 * the cloner. The interval for join flood is double the interval for kick floor, since
	 * the interval for ordinary kicks is the double of the interval for ordinary joins. Thus 1:1.
	 * We need to factor in the quitTimer, and to make an adjustment to factor in that the cloner
	 * may be kicked by other users due to its behaviour. 
	 * 
 	 * Firstly, lets adjust the timer to cater for cloner quits (quitTimer) and kills (killTimer).
	 */
	int quitDiff { } ;
	if( quitInterval > 0 )
		quitDiff = cycleInterval / ( ( cycleInterval + quitInterval ) / cycleInterval ) ;

	int killDiff { } ;
	if( killInterval > 0 )
		killDiff = cycleInterval / ( ( cycleInterval + killInterval ) / cycleInterval ) ;

	/* 
	 * Secondly, let's adjust the interval with the percentage of the difference between the number of 
	 * clones present and the number of clones initiated in the playchan multiplied by two.
	 */

	float diff = ( ( ( (float)playCloneCount - (float)cloneCount( theChan ) ) / 
			(float)playCloneCount ) * ( (float)cycleInterval - (float)quitDiff - (float)killDiff ) ) * 2 ;

	// We cannot go back in time. Cutting the interval in two. 
	if( diff > ( cycleInterval - quitDiff ) ) diff = ( (float)cycleInterval - (float)quitDiff - (float)killDiff ) / 2 ; 

	elog	<< "cloner::onTimer> joinTimer: Number of clones on channel: " << cloneCount( theChan )
			<< " playCloneCount: " << playCloneCount
			<< ". Interval reduced by " << quitDiff 
			<< " seconds to adjust for quitTimer and " << killDiff
			<< " seconds to adjust for killTimer. Correcting interval by an additional " 
			<< (int)diff << " seconds due to clone count." << endl ;

	joinTimer = MyUplink->RegisterTimer( ::time( 0 ) + cycleInterval - quitDiff - killDiff - (int)diff, this, 0 ) ; 
	}
else if( timer_id == partTimer )
	{
	Channel* theChan = Network->findChannel( playChan ) ;
	iClient* theClone = randomChanClone( theChan ) ;

	if( theClone != NULL )
		{
		stringstream s ;
		s	<< theClone->getCharYYXXX()
			<< " L "
			<< theChan->getName() 
			<< " :"
			<< randomSpam() ;

		MyUplink->Write( s ) ;

		// Cleaning up.
		ChannelUser* theUser = theChan->removeUser( theClone ) ;
		delete theUser ; theUser = 0 ;

		theClone->removeChannel ( theChan ) ;
		}

	/*
	 * Although the joinTimer is supposed to callibrate the number of clones,
	 * we would wan't to have the percentage adjustmet mechanism also here. 
	 */

	float diff = ( ( ( (float)playCloneCount - (float)cloneCount( theChan ) ) / (float)playCloneCount ) * ( (float)cycleInterval * 2 ) ) ;

	// We cannot go back in time. Cutting the interval in two. 
	if( diff > ( cycleInterval * 2 ) ) diff = cycleInterval ; 

	elog	<< "cloner::onTimer> partTimer: Number of clones on channel: "
			<< cloneCount( theChan )
			<< " playCloneCount: "
			<< playCloneCount
			<< ". Correcting interval by " 
			<< (int)diff << " seconds." << endl ;
	
	partTimer = MyUplink->RegisterTimer( ::time( 0 ) + ( cycleInterval * 2 ) + (int)diff, this, 0 ) ;
	}
else if( timer_id == quitTimer )
	{
	Channel* theChan = Network->findChannel( playChan ) ;
	iClient* theClone = randomChanClone( theChan ) ;

	if( theClone != NULL )
		{
		if( MyUplink->DetachClient( theClone, randomSpam() ) )
			{
			clones.remove( theClone ) ;
			delete theClone ;
			}

		// Adding a new clone.
		addClone() ;
		}
	
	quitTimer = MyUplink->RegisterTimer( ::time( 0 ) + quitInterval, this, 0 ) ;
	}
else if( timer_id == opTimer )
	{
	Channel* theChan = Network->findChannel( playChan ) ;
	iClient* tmpClone = randomChanOpClone( theChan ) ;

	// If there aren't any op'd clones on the channel, let's reop them.
	if( tmpClone == NULL ) 
		{
		float opTarget = round( ( (float)playCloneCount / 100 ) * playOps ) ;
		for( int i = 0 ; i < opTarget + 1 ; i++ )
			{
			ChannelUser* tmpUser = theChan->findUser ( randomChanClone( theChan ) ) ;
			if( NULL == tmpUser ) continue ;
			stringstream s ;
			s	<< MyUplink->getCharYY()
				<< " M "
				<< theChan->getName()
				<< " +o "
				<< tmpUser->getCharYYXXX() 
				<< " "
				<< theChan->getCreationTime() ;

				MyUplink->Write( s ) ;
				tmpUser->setModeO() ;
			}
		}

	iClient* theClone = randomChanOpClone( theChan ) ;

	if( theClone != NULL ) // Still no op'd clones?
		{
		int tmp { 1 } ;
		if( opFloodCount > floodCount )
			{
			tmp = floodLines ;
			opFloodCount = 0 ;
			}

		for( int i = 0 ; i < tmp ; i++ )
			{
			iClient* destClone = randomChanClone( theChan ) ;
			ChannelUser* destUser = theChan->findUser( destClone ) ;
			if( NULL == destClone ) break ;

			stringstream s ;
			s	<< theClone->getCharYYXXX()
				<< " M "
				<< theChan->getName()
				<< " +o "
				<< destClone->getCharYYXXX() ;

			MyUplink->Write( s ) ;
			destUser->setModeO() ;
			}

		opFloodCount++ ;
		}
	/*
	 * The interval for ops adds up with the interval for deops initiated by
	 * the cloner, so as to maintain the number of ops on the channel. However, opped clones
	 * may be kicked by other clones, or other users due to its behaviour. Hence, we need to adjust the interval
	 * to increase the number of ops if necessary. 
	 * 
	 * The following will decrease the interval with twice the same percentage as the difference between
	 * the number of opped clones and a percentage of the number of clones initiated on the channel.
	 */

	float opTarget = round( ( (float)playCloneCount / 100 ) * playOps ) ;
	float diff = ( ( ( opTarget - (float)cloneOpCount( theChan ) ) / opTarget ) * (float)opInterval ) * 2 ;

	// We cannot go back in time. Cutting the interval in two. 
	if( diff > opInterval ) diff = (float)opInterval / 2 ; 

	elog	<< "clone::OnTimer> opTimer: Current ops: "
			<< cloneOpCount( theChan )
			<< " opTarget: "
			<< opTarget
			<< ". Correcting interval by "
			<< (int)diff << " seconds." << endl ;

	opTimer = MyUplink->RegisterTimer( ::time( 0 ) + opInterval - (int)diff, this, 0 ) ;
	}
else if( timer_id == deopTimer )
	{
	Channel* theChan = Network->findChannel( playChan ) ;
	iClient* theClone = randomChanOpClone( theChan ) ;

	if( theClone != NULL )
		{
		int tmp { 1 } ;
		if( deopFloodCount > floodCount )
			{
			tmp = floodLines / 2 ;
			deopFloodCount = 0 ;
			}

		for( int i = 0 ; i < tmp ; i++ )
			{
			// Let's break if the number of op'd clones is less than four.
			if( cloneOpCount(theChan) < 4 ) break ;

			iClient* destClone = randomChanOpClone( theChan ) ;
			if( NULL == destClone ) break ;

			ChannelUser* destUser = theChan->findUser( destClone ) ;
			if( NULL == destUser ) break ;

			stringstream s ;
			s	<< theClone->getCharYYXXX()
				<< " M "
				<< theChan->getName()
				<< " -o "
				<< destClone->getCharYYXXX() ;

			MyUplink->Write( s ) ;

			destUser->removeModeO() ;

			// Did the clone deop itself? Break.
			if( theClone == destClone ) break ;
			}

		deopFloodCount++ ;
		}

	deopTimer = MyUplink->RegisterTimer( ::time( 0 ) + opInterval, this, 0 ) ;
	}
else if( timer_id == kickTimer )
	{
	Channel* theChan = Network->findChannel( playChan ) ;
	iClient* theClone = randomChanOpClone( theChan ) ;

	if( theClone != NULL )
		{
		int tmp { 1 } ;
		if( kickFloodCount > floodCount )
			{
			tmp = floodLines ;
			kickFloodCount = 0 ;
			}

		for( int i = 0 ; i < tmp ; i++ )
			{
			iClient* destClone = randomChanClone( theChan ) ;
			if( NULL == theClone ) break ;

			stringstream s ;
			s	<< theClone->getCharYYXXX()
				<< " K "
				<< theChan->getName()
				<< " "
				<< destClone->getCharYYXXX() 
				<< " :"
				<< randomSpam() ;

			MyUplink->Write( s ) ;

			ChannelUser* destUser = theChan->removeUser( destClone ) ;
			delete destUser ; destUser = 0 ;

			destClone->removeChannel( theChan ) ;

			// Did the clone kick itself? Break.
			if( theClone == destClone ) break ;
			}
		kickFloodCount++ ; 
		}

	kickTimer = MyUplink->RegisterTimer( ::time( 0 ) + ( cycleInterval * 2 ), this, 0 ) ;
	}
else if( timer_id == topicTimer )
	{
	Channel* theChan = Network->findChannel( playChan ) ;
	iClient* theClone = randomChanOpClone( theChan ) ;

	if( theClone != NULL )
		{
		int tmp { 1 } ;
		if( topicFloodCount > floodCount )
			{
			tmp = floodLines ;
			topicFloodCount = 0 ;
			}

		for( int i = 0 ; i < tmp ; i++ )
			{
			stringstream s ;
			s	<< theClone->getCharYYXXX()
				<< " T "
				<< theChan->getName()
				<< " :"
				<< randomSpam() ;

			MyUplink->Write( s ) ;
			}
		topicFloodCount++ ;
		}
	
	topicTimer = MyUplink->RegisterTimer( ::time( 0 ) + topicInterval, this, 0 ) ;
	}
else if( timer_id == nickTimer )
	{
	Channel* theChan = Network->findChannel( playChan ) ;
	iClient* theClone = randomChanClone( theChan ) ;

	if( theClone != NULL )
		{
		int tmp { 1 } ;
		if( nickFloodCount > floodCount )
			{
			tmp = floodLines ;
			nickFloodCount = 0 ;
			}

		for( int i = 0 ; i < tmp ; i++ )
			{
			string tmpNick = randomNick() ;
			stringstream s ;
			s	<< theClone->getCharYYXXX()
				<< " N "
				<< tmpNick 
				<< " "
				<< ::time( 0 ) ;

			MyUplink->Write( s ) ;
			Network->rehashNick( theClone->getCharYYXXX(), tmpNick ) ;
			}
		nickFloodCount++ ;
		}

	nickTimer = MyUplink->RegisterTimer( ::time( 0 ) + nickInterval, this, 0 ) ;
	}
else if( timer_id == killTimer )
	{
	/* We don't want the same random numeric to be chosen for the new clone as the killed one. */
	if( doKill )
		{
		Channel* theChan = Network->findChannel( playChan ) ;
		iClient* theClone = randomOper() ;
		iClient* destClone = randomChanClone( theChan ) ;

		doKill = false ;

		if( theClone != NULL && destClone != NULL )
			{
			stringstream s ;
			s	<< theClone->getCharYYXXX()
				<< " D "
				<< destClone->getCharYYXXX() 
				<< " :"
				<< MyUplink->getName()
				<< "!"
				<< theClone->getNickName()
				<< " ("
				<< randomSpam()
				<< ")" ;

			MyUplink->Write( s ) ;

			if( Network->removeClient( destClone ) )
				{
				clones.remove( destClone ) ;
				delete destClone ;
				}
			}
		}
	else
		{
		doKill = true ;

		// Adding a new clone.
		addClone() ;
		}

	killTimer = MyUplink->RegisterTimer( ::time( 0 ) + killInterval / 2, this, 0 ) ;
	}
}

void cloner::addClone()
{
// The XXX doesn't matter here, the core will choose an
// appropriate value.
string yyxxx( fakeServer->getCharYY() + "]]]" ) ;

iClient* newClient = new iClient(
		fakeServer->getIntYY(),
		yyxxx,
		randomNick( minNickLength, maxNickLength ),
		randomUser(),
		randomNick( 6, 6 ),
		randomHost(),
		randomHost(),
		cloneMode,
		string(),
		0,
		cloneDescription,
		::time( 0 ) ) ;
assert( newClient != 0 );

if( MyUplink->AttachClient( newClient, this ) )
	{
	clones.push_back( newClient ) ;
	}
}

// Returns an iClient pointer to a random clone. 
iClient* cloner::randomClone()
{
srand( ::time( 0 ) ) ;

std::list < iClient* >::iterator theClone = clones.begin() ;
std::advance( theClone, rand() % clones.size() ) ;

return *theClone ;
}

// Returns an iClient pointer to a random clone present on a channel. 
iClient* cloner::randomChanClone( Channel* theChan )
{
srand( ::time( 0 ) ) ;
size_t i { } ;

while ( i < theChan->userList_size() )
	{
	i++ ;
	Channel::userIterator chanUsers = theChan->userList_begin() ;
	std::advance( chanUsers, rand() % theChan->userList_size() ) ;

	// Check if tmpUser is in clones list. Let's skip the oper'ed users (we don't want them to quit). 
	ChannelUser* tmpUser = chanUsers->second;
	if( NULL == tmpUser || tmpUser->isOper() ) continue;

	auto pos = find( clones.begin() , clones.end() , tmpUser->getClient() ) ;
	if( pos != clones.end() )
		return tmpUser->getClient() ;
	}
	
// We have iterated through as many entries as in the userlist, without finding
// a single clone. Return NULL.
return NULL ;
}

// Returns an iClient pointer to a random clone not present on a channel.
iClient* cloner::availableClone( Channel* theChan )
{
srand( ::time( 0 ) ) ;
size_t i { } ;

while ( i < clones.size() )
	{
	i++ ;
	std::list < iClient* >::iterator theClone = clones.begin() ;
	std::advance( theClone, rand() % clones.size() ) ;

	if( !(*theClone)->findChannel( theChan ) ) return *theClone ;
	}

// We have iterated through the clones list, without finding any clones not
// present on the channel. Return NULL.
return NULL ;
}

// Returns an iClient pointer to a random oper'ed clone.
iClient* cloner::randomOper()
{
srand( ::time( 0 ) ) ;
size_t i { } ;

while ( i < clones.size() )
	{
	i++ ;
	std::list < iClient* >::iterator theClone = clones.begin() ;
	std::advance( theClone, rand() % clones.size() ) ;

	if( (*theClone)->isOper() ) return *theClone ;
	}

// We have iterated through the clones list, without finding any oper'd clones.
return NULL ;
}

// Returns an iClient pointer to a random clone present and op'd on a channel.
iClient* cloner::randomChanOpClone( Channel* theChan )
{
srand( ::time( 0 ) ) ;
size_t i { } ;

while ( i < theChan->userList_size() )
	{
	i++ ;
	Channel::userIterator chanUsers = theChan->userList_begin() ;
	std::advance( chanUsers, rand() % theChan->userList_size() ) ;

	// Check if tmpUser is op'd and on clones list.
	ChannelUser* tmpUser = chanUsers->second ;
	if( NULL == tmpUser || !tmpUser->isModeO() ) continue ;

	auto pos = find( clones.begin() , clones.end() , tmpUser->getClient() ) ;
	if( pos != clones.end() )
		return tmpUser->getClient() ;
	}

return NULL;
}

size_t cloner::cloneCount( Channel* theChan )
{
size_t cloneCount { } ;

for( Channel::userIterator chanUsers = theChan->userList_begin() ;
	chanUsers != theChan->userList_end(); ++chanUsers )
	{
	ChannelUser* tmpUser = chanUsers->second ;
	if( NULL == tmpUser ) continue ;

	// Check if tmpUser is in clones list.
    auto pos = std::find( clones.begin() , clones.end() , tmpUser->getClient() ) ;
	if( pos != clones.end() ) cloneCount++ ;
	} // for()

return cloneCount ;
}

size_t cloner::cloneOpCount( Channel* theChan )
{
size_t opCount { } ;

for( Channel::userIterator chanUsers = theChan->userList_begin() ;
	chanUsers != theChan->userList_end(); ++chanUsers )
	{
	ChannelUser* tmpUser = chanUsers->second ;
	if( NULL == tmpUser || !tmpUser->isModeO() ) continue ;

	// Check if tmpUser is in clones list.
    auto pos = std::find( clones.begin() , clones.end() , tmpUser->getClient() ) ;
	if( pos != clones.end() ) opCount++ ;
	} // for()

return opCount ;
}

string cloner::randomUser()
{
return userNames[ rand() % userNames.size() ] ;
}

string cloner::randomHost()
{
return hostNames[ rand() % hostNames.size() ] ;
}

string cloner::randomSpam()
{
return spamList[ rand() % spamList.size() ] ;
}

string cloner::randomNick( int minLength, int maxLength )
{
string retMe ;

// Generate a random number between minLength and maxLength
// This will be the length of the nickname
int randomLength = minLength + (rand() % (maxLength - minLength + 1) ) ;

for( int i = 0 ; i < randomLength ; i++ )
        {
        retMe += randomChar() ;
        }

//elog << "randomNick: " << retMe << endl ;
return retMe ;
}

// ascii [65,122]
char cloner::randomChar()
{
char c = ('A' + (rand() % ('z' - 'A')) ) ;
//elog << "char: returning: " << c << endl ;
return c ;
         
//return( (65 + (rand() % 122) ) ;
//return (char) (1 + (int) (9.0 * rand() / (RAND_MAX + 1.0) ) ) ;
}

bool cloner::hasAccess( const string& accountName ) const
{
for( std::list< string >::const_iterator itr = allowAccess.begin() ;
	itr != allowAccess.end() ; ++itr )
	{
	if( !strcasecmp( accountName, *itr ) )
		{
		return true ;
		}
	}
return false ;
}

bool cloner::deactivatePlay()
{
MyUplink->UnRegisterTimer( spamTimer, 0 ) ;
MyUplink->UnRegisterTimer( actionTimer, 0 ) ;
MyUplink->UnRegisterTimer( joinTimer, 0 ) ;
MyUplink->UnRegisterTimer( partTimer, 0 ) ;
MyUplink->UnRegisterTimer( quitTimer, 0 ) ;
MyUplink->UnRegisterTimer( opTimer, 0 ) ;
MyUplink->UnRegisterTimer( deopTimer, 0 ) ;
MyUplink->UnRegisterTimer( kickTimer, 0 ) ;
MyUplink->UnRegisterTimer( topicTimer, 0 ) ;
MyUplink->UnRegisterTimer( nickTimer, 0 ) ;
MyUplink->UnRegisterTimer( killTimer, 0 ) ;

playCloneCount = 0 ;

return true ;
}

} // namespace gnuworld
