//////////////////////////////////////////////////////////////////
//
// gk.cxx for H.323 gatekeeper
//
// This work is published under the GNU Public License (GPL)
// see file COPYING for details.
// We also explicitely grant the right to link this code
// with the OpenH323 library.
//
// History:
// 	990500	initial version (Xiang Ping Chen, Rajat Todi, Joe Metzger)
//	990600	ported to OpenH323 V. 1.08 (Jan Willamowius)
//	990702	code cleanup (Jan Willamowius)
//	990924	clean shutdown (Jan Willamowius)
//	991016	clean shutdown (Jan Willamowius)
//
//////////////////////////////////////////////////////////////////


#if (_MSC_VER >= 1200)  
#pragma warning( disable : 4800 ) // one performance warning off
#pragma warning( disable : 4786 ) // warning about too long debug symbol off
#endif

#ifndef WIN32
#include <signal.h>
#endif
#include <ptlib.h>
#include <q931.h>
#include "gk.h"
#include "RasSrv.h"
#include "SoftPBX.h"
#include "MulticastGRQ.h"
#include "BroadcastListen.h"
#include "Toolkit.h"
#include "h323util.h"


/*
 * many things here should be members of Gatkeeper. 
 */

namespace { // keep the global objects private


MulticastGRQ * MulticastGRQThread = NULL;
BroadcastListen * BroadcastThread = NULL;
PMutex ShutdownMutex;
PMutex ReloadMutex;

#ifndef WIN32
PString pidfile("/var/run/gk.pid");
#endif
#ifdef PTRACING
PTextFile *logfile = 0;
PString logfilename;
#endif

bool ExitFlag = false;


void ShutdownHandler(void)
{
	// we may get one shutdown signal for every thread; make sure we
	// delete objects only once
	if (ShutdownMutex.WillBlock())
		return;
	PWaitAndSignal shutdown(ShutdownMutex);
	if (BroadcastThread != NULL)
	{
		PTRACE(3, "GK\tClosing BroadcastThread");
		BroadcastThread->Close();
		BroadcastThread->WaitForTermination();
		delete BroadcastThread;
		BroadcastThread = NULL;
	}
	if (MulticastGRQThread != NULL)
	{
		PTRACE(3, "GK\tClosing MulticastGRQThread");
		MulticastGRQThread->Close();
		MulticastGRQThread->WaitForTermination();
		delete MulticastGRQThread;
		MulticastGRQThread = NULL;
	}
	if (RasThread != NULL)
	{
		PTRACE(3, "GK\tClosing RasThread");
		RasThread->Close();
		// send all registered clients a URQ
		RasThread->UnregisterAllEndpoints();
		RasThread->WaitForTermination();
		delete RasThread;
		RasThread = NULL;
	}

	// delete singleton objects
	PTRACE(3, "GK\tDeleting global reference tables");

	// The singletons would be deleted automatically
	// by destructor of listptr<SingletonBase *>
	// However, I have to delete Toolkit instance here,
	// or it will cause a core dump. I don't know why...
        // delete resourceManager::Instance();
        // delete RegistrationTable::Instance();
        // delete CallTable::Instance();
        delete Toolkit::Instance();

#ifdef PTRACING
	PTrace::SetStream(&cerr); // redirect to cerr
	delete logfile;
#endif
	return;
}

#ifdef PTRACING
void ReopenLogFile()
{
	if (!logfilename) {
		PTRACE(1, "GK\tLogging closed.");
		PTrace::SetStream(&cerr); // redirect to cerr
		delete logfile;
		logfile = new PTextFile(logfilename, PFile::WriteOnly, PFile::Create | PFile::Truncate);
		if (!logfile->IsOpen()) {
			cout << "Warning: could not open trace output file \""
				<< logfilename << '"' << endl;
			delete logfile;
			logfile = 0;
			return;
		}
		PTrace::SetStream(logfile); // redirect to logfile
	}
	PTRACE(1, "GK\tTrace logging started.");
}
#endif

} // end of anonymous namespace

void ReloadHandler(void)
{
	// only one thread must do this
	if (ReloadMutex.WillBlock())
		return;
	
	/*
	** Enter critical Section
	*/
	PWaitAndSignal reload(ReloadMutex);

	/*
	** Force reloading config
	*/
	InstanceOf<Toolkit>()->ReloadConfig();
	PTRACE(3, "GK\t\tConfig reloaded.");
	GkStatus::Instance()->SignalStatus("Config reloaded.\r\n");

	/*
	** Update all gateway prefixes
	*/

	RegistrationTable::Instance()->LoadConfig();
	CallTable::Instance()->LoadConfig();

	RasThread->LoadConfig();

	/*
	** Don't disengage current calls!
	*/
	PTRACE(3, "GK\t\tCarry on current calls.");

	/*
	** Leave critical Section
	*/
	// give other threads the chance to pass by this handler
	PProcess::Current().Sleep(1000); 
}

#ifdef WIN32

BOOL WINAPI WinCtrlHandlerProc(DWORD dwCtrlType)
{
	PTRACE(1, "GK\tGatekeeper shutdown");
	PWaitAndSignal shutdown(ShutdownMutex);
	return ExitFlag = true;
}

#else

void UnixShutdownHandler(int sig)
{
	PTRACE(1, "GK\tGatekeeper shutdown (signal " << sig << ")");
	if (ShutdownMutex.WillBlock())
		return;
	PWaitAndSignal shutdown(ShutdownMutex);
	ExitFlag = true;
	PFile::Remove(pidfile);
}

void UnixReloadHandler(int sig) // For HUP Signal
{
	PTRACE(1, "GK\tGatekeeper Hangup (signal " << sig << ")");
#ifdef PTRACING
	ReopenLogFile();
#endif
	ReloadHandler();
}

#endif


// default params for overwriting
Gatekeeper::Gatekeeper(const char * manuf,
					   const char * name,
					   WORD majorVersion,
					   WORD minorVersion,
					   CodeStatus status,
					   WORD buildNumber)
	: PProcess(manuf, name, majorVersion, minorVersion, status, buildNumber)
{
}


const PString Gatekeeper::GetArgumentsParseString() const
{
	return PString
		("r-routed."
		 "b-bandwidth:"
		 "i-interface:"
#ifdef PTRACING
		 "t-trace."
		 "o-output:"
#endif
		 "l-timetolive:"
		 "c-configfile:"
		 "s-section:"
		 "-pid:"
		 "h-help:"
		 );
}


BOOL Gatekeeper::InitHandlers(const PArgList &args)
{
#ifdef WIN32
	SetConsoleCtrlHandler(WinCtrlHandlerProc, TRUE);
#else
	signal(SIGTERM, UnixShutdownHandler);
	signal(SIGINT, UnixShutdownHandler);
	signal(SIGQUIT, UnixShutdownHandler);
	signal(SIGUSR1, UnixShutdownHandler);

	struct sigaction sa;
	sigemptyset(&sa.sa_mask);
	sigaddset(&sa.sa_mask, SIGHUP); // ignore while in handler
	sa.sa_flags = 0;
	sa.sa_handler = UnixReloadHandler;

	sigaction(SIGHUP, &sa, NULL);

	if (args.HasOption("pid"))
		pidfile = args.GetOptionString("pid");
	PTextFile pid;
	pid.Open(pidfile, PFile::WriteOnly);
	pid.WriteLine(PString(PString::Unsigned, getpid()));
#endif
	return TRUE;
}


BOOL Gatekeeper::InitLogging(const PArgList &args)
{
#ifdef PTRACING
	// PTrace::SetOptions(PTrace::Timestamp | PTrace::Thread);
	// PTrace::SetOptions(PTrace::Timestamp);
	//PTrace::SetOptions(PTrace::DateAndTime | PTrace::TraceLevel | PTrace::Thread);
	PTrace::SetOptions(PTrace::DateAndTime | PTrace::TraceLevel);
	PTrace::SetLevel(args.GetOptionCount('t'));
	if (args.HasOption('o')) {
		logfilename = args.GetOptionString('o');
		ReopenLogFile();
	}
#endif
	
	return TRUE;
}


BOOL Gatekeeper::InitToolkit(const PArgList &args)
{
	InstanceOf<Toolkit>(); // force using the right Toolkit constructor

	return TRUE;
}


BOOL Gatekeeper::InitConfig(const PArgList &args)
{
	// get the name of the config file
	PFilePath fp("gatekeeper.ini");
	PString section("Gatekeeper::Main");

	if (args.HasOption('c')) 
		fp = PFilePath(args.GetOptionString('c'));

	if (args.HasOption('s')) 
		section = args.GetOptionString('s');

	InstanceOf<Toolkit>()->SetConfig(fp, section);

	if( (GkConfig()->GetInteger("Fourtytwo") ) != 42) { 
		cerr << "WARNING: No config file found!\n"
			 << "- Does the config file exist? The default (~/.pwlib_config/Gatekeeper.ini or gatekeeper.ini in current directory) or the one given with -c?\n"
			 << "- Did you specify they the right 'Main' section with -s?\n" 
			 << "- Is the line 'Fourtytwo=42' present in this 'Main' section?"<<endl;
	}
	
	return TRUE;
}


void Gatekeeper::PrintOpts(void)
{
	PStringArray opts = GetArgumentsParseString().Tokenise(".:", FALSE);

	cout << "Known options:" << endl;
	for(PINDEX i=0; i< opts.GetSize(); i++){
		cout << opts[i]<< endl;
	}
}


void Gatekeeper::HouseKeeping(void)
{
	for (unsigned count=1; !ExitFlag; count++) {	

		Sleep(1000);

		if (!RasThread->Check()) // return true if the thread running
			break;

		if (!(count % 60)) // one minute
			RegistrationTable::Instance()->CheckEndpoints();

		CallTable::Instance()->CheckCalls();
	}
}

void Gatekeeper::Main()
{
	PArgList & args = GetArguments();
	args.Parse(GetArgumentsParseString());

	int GKcapacity = 100000; // default gatekeeper capacity (in 100s bit)
	BOOL GKroutedSignaling = FALSE;	// default: use direct signaling
	PIPSocket::Address GKHome;

	if(! InitLogging(args)) return;

	if(! InitHandlers(args)) return;

	if(! InitToolkit(args)) return;

	if(! InitConfig(args)) return;

	if (args.HasOption('h')) {
		cout << "OpenH323 gatekeeper '" << Toolkit::GKName() << "' started on " << inet_ntoa(GKHome) << endl;
		cout << Toolkit::GKVersion() << endl;
		PrintOpts();
        	delete Toolkit::Instance();
		exit(0);
	}

	// read gatekeeper home address from commandline
	if (args.HasOption('i'))
		GKHome = args.GetOptionString('i');
	else {
		PString s = GkConfig()->GetString("Home", "x");
		if (s == "x")
			PIPSocket::GetHostAddress(GKHome);
		else
			GKHome = s;
	}
	
	cout << "OpenH323 gatekeeper with ID '" << Toolkit::GKName() << "' started on " << inet_ntoa(GKHome) << endl;
	cout << Toolkit::GKVersion() << endl;
	PTRACE(1, "GK\tGatekeeper with ID '" << Toolkit::GKName() << "' started on " << inet_ntoa(GKHome));
	PTRACE(1, "GK\t"<<Toolkit::GKVersion());

	// Copyright notice
	cout <<
		"This program is free software; you can redistribute it and/or\n"
		"modify it under the terms of the GNU General Public License\n"
		"as published by the Free Software Foundation; either version 2\n"
		"of the License, or (at your option) any later version.\n"
	    << endl;

	// read signaling method from commandline
	if (args.HasOption('r'))
		GKroutedSignaling = TRUE;
	if (GKroutedSignaling)
		PTRACE(2, "GK\tUsing routed signalling");
	else
		PTRACE(2, "GK\tUsing direct signalling");

	// read capacity from commandline
	if (args.HasOption('b'))
		GKcapacity = args.GetOptionString('b').AsInteger();
	PTRACE(2, "GK\tAvailable Bandwidth: " << GKcapacity);
	resourceManager::Instance()->SetBandWidth(GKcapacity);

	// read timeToLive from command line
	if (args.HasOption('l'))
		SoftPBX::TimeToLive = args.GetOptionString('l').AsInteger();
	PTRACE(2, "GK\tTimeToLive for Registrations: " << SoftPBX::TimeToLive);
  
	RasThread = new H323RasSrv(GKHome);
	RasThread->SetGKSignaling(GKroutedSignaling);

	MulticastGRQThread = new MulticastGRQ(GKHome, RasThread);

#if (defined P_LINUX) || (defined P_FREEBSD) || (defined P_HPUX9) || (defined P_SOLARIS)
	// On some OS we don't get broadcasts on a socket that is
	// bound to a specific interface. For those we have to start
	// a thread that listens just for those broadcasts.
	// On Windows NT we get all messages on the RAS socket, even
	// if it's bound to a specific interface and thus don't have
	// to start this thread.
	BroadcastThread = new BroadcastListen(RasThread);
#endif

	// let's go
	RasThread->Resume();

	HouseKeeping();

	// graceful shutdown
	ShutdownHandler();
}

