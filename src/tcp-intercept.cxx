#include "../config.h"

#include <iostream>
#include <fstream>
#include <getopt.h>
#include <netinet/in.h>
#include <ev.h>
#include <sysexits.h>

#include <boost/ptr_container/ptr_list.hpp>

#include "gettext.h"
#define _(String) gettext(String)
#define N_(String) String

#include "../Socket/Socket.hxx"
#include <libsimplelog.h>
#include <libdaemon/daemon.h>
#include <netinet/tcp.h>

std::string logfilename;
FILE *logfile;

static const int MAX_CONN_BACKLOG = 32;

std::auto_ptr<SockAddr::SockAddr> bind_listen_addr;
std::auto_ptr<SockAddr::SockAddr> bind_addr_outgoing;
bool keepalive = false;
bool nodelay = false;

struct connection {
	std::string id;

	Socket s_client;
	Socket s_server;

	ev_io e_s_connect;
	ev_io e_c_read, e_c_write;
	ev_io e_s_read, e_s_write;

	std::string buf_c_to_s, buf_s_to_c;
	bool con_open_c_to_s, con_open_s_to_c;
};
boost::ptr_list< struct connection > connections;


void received_sigint(EV_P_ ev_signal *w, int revents) throw() {
	LogInfo(_("Received SIGINT, exiting"));
	ev_break(EV_A_ EVUNLOOP_ALL);
}
void received_sigterm(EV_P_ ev_signal *w, int revents) throw() {
	LogInfo(_("Received SIGTERM, exiting"));
	ev_break(EV_A_ EVUNLOOP_ALL);
}

void received_sighup(EV_P_ ev_signal *w, int revents) throw() {
	LogInfo(_("Received SIGHUP, closing this logfile"));
	if( logfilename.size() > 0 ) {
		fclose(logfile);
		logfile = fopen(logfilename.c_str(), "a");
		LogSetOutputFile(NULL, logfile);
	} /* else we're still logging to stderr, which doesn't need reopening */
	LogInfo(_("Received SIGHUP, (re)opening this logfile"));
}

void received_sigpipe(EV_P_ ev_signal *w, int revents) throw() {
	LogDebug(_("Received SIGPIPE, ignoring"));
}


void kill_connection(EV_P_ struct connection *con) {
	// Remove from event loops
	ev_io_stop(EV_A_ &con->e_c_read );
	ev_io_stop(EV_A_ &con->e_c_write );
	ev_io_stop(EV_A_ &con->e_s_read );
	ev_io_stop(EV_A_ &con->e_s_write );

	/* TRANSLATORS: %1$s contains the connection ID that was just closed */
	LogInfo(_("%1$s: closed"), con->id.c_str());

	// Find and erase this connection in the list
	// TODO scaling issue: this is O(n) with the number of connections.
	for( typeof(connections.begin()) i = connections.begin(); i != connections.end(); ++i ) {
		if( &(*i) == con ) {
			connections.erase(i);
			break; // Stop searching
		}
	}
}

static void server_socket_connect_done(EV_P_ ev_io *w, int revents) {
	struct connection* con = reinterpret_cast<struct connection*>( w->data );

	ev_io_stop(EV_A_ &con->e_s_connect); // We connect only once

	Errno connect_error("connect()", con->s_server.getsockopt_so_error());
	if( connect_error.error_number() != 0 ) {
		/* TRANSLATORS: %1$s contains the connection ID,
		   %2$s the error message */
		LogWarn(_("%1$s: connect to server failed: %2$s"),
			con->id.c_str(),
			connect_error.what() );
		kill_connection(EV_A_ con);
		return;
	}

	/* TRANSLATORS: %1$s contains the connection ID */
	LogInfo(_("%1$s: server accepted connection, splicing"), con->id.c_str());
	ev_io_start(EV_A_ &con->e_c_write);
	ev_io_start(EV_A_ &con->e_s_write);
}

inline static void peer_ready_write(EV_P_ struct connection* con,
                                    std::string const &dir,
                                    bool &con_open,
                                    Socket &rx, ev_io *e_rx_read,
                                    std::string &buf,
                                    Socket &tx, ev_io *e_tx_write ) {
	if( buf.length() == 0 ) {
		// All is written, and we're still ready to write, read some more
		ev_io_start( EV_A_ e_rx_read );
		ev_io_stop( EV_A_ e_tx_write );
		return;
	}
	// buf.length() > 0
	try {
		ssize_t rv = tx.send(buf.data(), buf.length());
		if( rv == 0 ) {
			// Weird situation. FD was ready for write, but send() returned 0
			// anyway... Retry later
			LogWarn(_("%1$s %2$s: could not send(), but was ready for write"),
				con->id.c_str(), dir.c_str());
			return;
		}
		buf = buf.substr( rv );
	} catch( Errno &e ) {
		/* TRANSLATORS: %1$s contains the connection ID,
		   %2$s contains the direction (separately translated),
		   %3$s contains the error
		 */
		LogError(_("%1$s %2$s: Error: %3$s)"), con->id.c_str(), dir.c_str(), e.what());
		kill_connection(EV_A_ con);
	}
}
inline static void peer_ready_read(EV_P_ struct connection* con,
                                   std::string const &dir,
                                   bool &con_open,
                                   Socket &rx, ev_io *e_rx_read,
                                   std::string &buf,
                                   Socket &tx, ev_io *e_tx_write ) {
	// TODO allow read when we still have data in the buffer to streamline things
	assert( buf.length() == 0 );
	try {
		buf = rx.recv();
		ev_io_stop( EV_A_ e_rx_read );
		if( buf.length() == 0 ) { // EOF has been read
			/* TRANSLATORS: %1$s contains the connection ID,
			   %2$s contains the direction (separately translated)
			 */
			LogInfo(_("%1$s %2$s: EOF"), con->id.c_str(), dir.c_str());
			tx.shutdown(SHUT_WR); // shutdown() does not block
			con_open = false;
			if( !con->con_open_s_to_c && !con->con_open_c_to_s ) {
				// Connection fully closed, clean up
				kill_connection(EV_A_ con);
			}
		} else { // data has been read
			ev_io_start( EV_A_ e_tx_write );
		}
	} catch( Errno &e ) {
		/* TRANSLATORS: %1$s contains the connection ID,
		   %2$s contains the direction (separately translated),
		   %3$s contains the error
		 */
		LogError(_("%1$s %2$s: Error: %3$s)"), con->id.c_str(), dir.c_str(), e.what());
		kill_connection(EV_A_ con);
	}
}

static void client_ready_write(EV_P_ ev_io *w, int revents) {
	struct connection* con = reinterpret_cast<struct connection*>( w->data );
	assert( w == &con->e_c_write );
	return peer_ready_write(EV_A_ con, _("S>C"), con->con_open_s_to_c,
	                        con->s_server, &con->e_s_read,
	                        con->buf_s_to_c,
	                        con->s_client, &con->e_c_write);
}
static void server_ready_write(EV_P_ ev_io *w, int revents) {
	struct connection* con = reinterpret_cast<struct connection*>( w->data );
	assert( w == &con->e_s_write );
	return peer_ready_write(EV_A_ con, _("C>S"), con->con_open_c_to_s,
	                        con->s_client, &con->e_c_read,
	                        con->buf_c_to_s,
	                        con->s_server, &con->e_s_write);
}

static void client_ready_read(EV_P_ ev_io *w, int revents) {
	struct connection* con = reinterpret_cast<struct connection*>( w->data );
	assert( w == &con->e_c_read );
	return peer_ready_read(EV_A_ con, _("C>S"), con->con_open_c_to_s,
	                       con->s_client, &con->e_c_read,
	                       con->buf_c_to_s,
	                       con->s_server, &con->e_s_write);
}
static void server_ready_read(EV_P_ ev_io *w, int revents) {
	struct connection* con = reinterpret_cast<struct connection*>( w->data );
	assert( w == &con->e_s_read );
	return peer_ready_read(EV_A_ con, _("S>C"), con->con_open_s_to_c,
	                       con->s_server, &con->e_s_read,
	                       con->buf_s_to_c,
	                       con->s_client, &con->e_c_write);
}


static bool our_sockaddr(SockAddr::SockAddr const *destination) throw(Errno) {
	// Begin with quick checks
	if( destination->port_number() != bind_listen_addr->port_number() ) {
		return false;
	}

	if( ! bind_listen_addr->is_any() ) {
		if( *destination == *bind_listen_addr ) {
			// We've "intercepted" a connection that was directed to us
			return true;
		}
	} else {
		// Iterate over all local IPs
		// This can't be cached, since local IPs change over time
		std::auto_ptr< boost::ptr_vector< SockAddr::SockAddr> > local_addrs
			= SockAddr::getifaddrs();
		for( typeof(local_addrs->begin()) i = local_addrs->begin(); i != local_addrs->end(); i++ ) {
			if( destination->address_equal(*i) ) return true;
		}
	}
	return false;
}


static void listening_socket_ready_for_read(EV_P_ ev_io *w, int revents) {
	Socket* s_listen = reinterpret_cast<Socket*>( w->data );

	std::auto_ptr<struct connection> new_con( new struct connection );

	std::auto_ptr<SockAddr::SockAddr> client_addr;
	std::auto_ptr<SockAddr::SockAddr> server_addr;
	try {
		new_con->s_client = s_listen->accept(&client_addr);
		//We do not want to buffer small packets, which could increase latency/jitter
		//for real time applications. Let them go out as they came in!!
		if(nodelay){
			int val = 1;
			new_con->s_client.setsockopt(IPPROTO_TCP, TCP_NODELAY, (char *) &val, sizeof(val));
		}
		//Take care of socks hanging in ESTABLISHED/CLOSE_WAIT/FIN_WAIT2 states		
		if(keepalive){
			int val = 1;
			new_con->s_client.setsockopt(SOL_SOCKET, SO_KEEPALIVE, &val, sizeof(val));
			//LogInfo(_("%1$s: Enabling keepalive on s_client"), new_con->id.c_str());
		}

		server_addr = new_con->s_client.getsockname();

		new_con->id.assign( client_addr->string() );
		new_con->id.append( "-->" );
		new_con->id.append( server_addr->string() );

		new_con->s_client.non_blocking(true);

		if( our_sockaddr(server_addr.get()) ) {
			/* TRANSLATORS: %1$s contains the connection ID
			 */
			LogWarn(_("%1$s: Connection directly to us, dropping"), new_con->id.c_str());
			return;
			// Sockets will go out of scope, and close() themselves
		}

		/* TRANSLATORS: %1$s contains the connection ID
		 */
		LogInfo(_("%1$s: Connection intercepted"), new_con->id.c_str());

		new_con->s_server = Socket::socket(server_addr->addr_family(), SOCK_STREAM, 0);
		if(nodelay){
			int val = 1;
			new_con->s_server.setsockopt(IPPROTO_TCP, TCP_NODELAY, (char *) &val, sizeof(val));
		}
		if(keepalive){
			int val = 1;
			new_con->s_server.setsockopt(SOL_SOCKET, SO_KEEPALIVE, &val, sizeof(val));
			//LogInfo(_("%1$s: Enabling keepalive on s_server"), new_con->id.c_str());		
		}
		
		if( bind_addr_outgoing.get() != NULL ) {
			new_con->s_server.bind( *bind_addr_outgoing );
		} else {
#if HAVE_DECL_IP_TRANSPARENT
			int value = 1;
			new_con->s_server.setsockopt(SOL_IP, IP_TRANSPARENT, &value, sizeof(value));
#endif
			new_con->s_server.bind( *client_addr );
		}
		new_con->s_server.non_blocking(true);
	} catch( Errno &e ) {
		LogError(_("Error: %s"), e.what());
		return;
		// Sockets will go out of scope, and close() themselves
	}

	ev_io_init( &new_con->e_s_connect, server_socket_connect_done, new_con->s_server, EV_WRITE );
	ev_io_init( &new_con->e_c_read,  client_ready_read,  new_con->s_client, EV_READ );
	ev_io_init( &new_con->e_c_write, client_ready_write, new_con->s_client, EV_WRITE );
	ev_io_init( &new_con->e_s_read,  server_ready_read,  new_con->s_server, EV_READ );
	ev_io_init( &new_con->e_s_write, server_ready_write, new_con->s_server, EV_WRITE );
	new_con->e_s_connect.data =
		new_con->e_c_read.data =
		new_con->e_c_write.data =
		new_con->e_s_read.data =
		new_con->e_s_write.data =
			new_con.get();
	new_con->con_open_c_to_s = new_con->con_open_s_to_c = true;

	try {
		new_con->s_server.connect( *server_addr );
		// Connection succeeded right away, flag the callback right away
		ev_feed_event(EV_A_ &new_con->e_s_connect, 0);

	} catch( Errno &e ) {
		if( e.error_number() == EINPROGRESS ) {
			// connect() is started, wait for socket to become write-ready
			// Have libev call the callback
			ev_io_start( EV_A_ &new_con->e_s_connect );

		} else {
			LogError(_("Error: %s"), e.what());
			return;
			// Sockets will go out of scope, and close() themselves
		}
	}

	std::auto_ptr<SockAddr::SockAddr> my_addr;
	my_addr = new_con->s_server.getsockname();
	/* TRANSLATORS: %1$s contains the connection ID,
	   %2$s the source address of the new connection,
	   %3$s the destination address of the new connection
	 */
	LogInfo(_("%1$s: Connecting %2$s-->%3$s"), new_con->id.c_str(),
			my_addr->string().c_str(), server_addr->string().c_str());

	connections.push_back( new_con.release() );
}

const char* pidfile = NULL;
const char* return_pidfile() {
	return pidfile;
}

int main(int argc, char* argv[]) {
	setlocale (LC_ALL, "");
	bindtextdomain(PACKAGE, LOCALEDIR);
	textdomain(PACKAGE);

	// Default options
	struct {
		bool fork;
		std::string bind_addr_listen;
		std::string bind_addr_outgoing;
	} options = {
		/* fork = */ true,
		/* bind_addr_listen = */ "[0.0.0.0]:[5000]",
		/* bind_addr_outgoing = */ "[0.0.0.0]:[0]"
		};

	{ // Parse options
		char optstring[] = "hVknfp:b:B:l:";
		struct option longopts[] = {
			{"help",			no_argument, NULL, 'h'},
			{"version",			no_argument, NULL, 'V'},
			{"keepalive",		no_argument, NULL, 'k'},
			{"tcp_nodelay",		no_argument, NULL, 'n'},
			{"foreground",		no_argument, NULL, 'f'},
			{"pid-file",		required_argument, NULL, 'p'},
			{"bind-listen",		required_argument, NULL, 'b'},
			{"bind-outgoing",	required_argument, NULL, 'B'},
			{"log",				required_argument, NULL, 'l'},
			{NULL, 0, 0, 0}
		};
		int longindex;
		int opt;
		while( (opt = getopt_long(argc, argv, optstring, longopts, &longindex)) != -1 ) {
			switch(opt) {
			case 'h':
			case '?':
				std::cerr << _(
				//  >---------------------- Standard terminal width ---------------------------------<
					"Options:\n"
					"  -h --help                       Displays this help message and exits\n"
					"  -V --version                    Displays the version and exits\n"
					"  -k --keepalive                  Enable keepalive on the sockets\n"
					"  -n --tcp_nodelay                Disable Nagel's Algorithm on the sockets\n"
					"  -f --foreground                 Don't fork and detach\n"
					"  --pid-file -p file              The file to write the PID to, especially\n"
					"                                  usefull when running as a daemon. Must be an\n"
					"                                  absolute path.\n"
					"  --bind-listen -b host:port      Bind to the specified address for incomming\n"
					"                                  connections.\n"
					"                                  host and port resolving can be bypassed by\n"
					"                                  placing [] around them\n"
					"  --bind-outgoing -B host:port    Bind to the specified address for outgoing\n"
					"                                  connections.\n"
					"                                  host and port resolving can be bypassed by\n"
					"                                  placing [] around them\n"
					"                                  the special string \"client\" can be used to\n"
					"                                  reuse the client's source address. Note that\n"
					"                                  you should take care that the return packets\n"
					"                                  pass through this process again!\n"
					"  --log -l file                   Log to file\n"
					);
				if( opt == '?' ) exit(EX_USAGE);
				exit(EX_OK);
			case 'V':
				printf(_("%1$s version %2$s\n"
				         " configured with: %3$s\n"
				         " CFLAGS=\"%4$s\" CXXFLAGS=\"%5$s\" CPPFLAGS=\"%6$s\"\n"
				         " Options:\n"
				         "   IPv6: %7$s\n"
				         "\n"),
					 PACKAGE_NAME, PACKAGE_VERSION " (" PACKAGE_GITREVISION ")",
				         CONFIGURE_ARGS,
				         CFLAGS, CXXFLAGS, CPPFLAGS,
#ifdef ENABLE_IPV6
				         _("yes")
#else
				         _("no")
#endif
				         );
				exit(EX_OK);
			case 'k':
				keepalive = true;
				break;
			case 'n':
				nodelay = true;
				break;
			case 'f':
				options.fork = false;
				break;
			case 'p':
				pidfile = optarg;
				break;
			case 'b':
				options.bind_addr_listen = optarg;
				break;
			case 'B':
				options.bind_addr_outgoing = optarg;
				break;
			case 'l':
				logfilename = optarg;
				logfile = fopen(logfilename.c_str(), "a");
				LogSetOutputFile(NULL, logfile);
				break;
			}
		}
	}

	/* Set indetification string for the daemon for both syslog and PID file */
	daemon_pid_file_ident = daemon_log_ident = daemon_ident_from_argv0(argv[0]);

	LogInfo(_("%1$s version %2$s starting up"), PACKAGE_NAME, PACKAGE_VERSION " (" PACKAGE_GITREVISION ")");

	Socket s_listen;
	{ // Open listening socket
		std::string host, port;

		/* Address format is
		 *   - hostname:portname
		 *   - [numeric ip]:portname
		 *   - hostname:[portnumber]
		 *   - [numeric ip]:[portnumber]
		 */
		size_t c = options.bind_addr_listen.rfind(":");
		if( c == std::string::npos ) {
			/* TRANSLATORS: %1$s contains the string passed as option
			 */
			fprintf(stderr, _("Invalid bind string \"%1$s\": could not find ':'\n"), options.bind_addr_listen.c_str());
			exit(EX_DATAERR);
		}
		host = options.bind_addr_listen.substr(0, c);
		port = options.bind_addr_listen.substr(c+1);

		std::auto_ptr< boost::ptr_vector< SockAddr::SockAddr> > bind_sa
			= SockAddr::resolve( host, port, 0, SOCK_STREAM, 0);
		if( bind_sa->size() == 0 ) {
			fprintf(stderr, _("Can not bind to \"%1$s\": Could not resolve\n"), options.bind_addr_listen.c_str());
			exit(EX_DATAERR);
		} else if( bind_sa->size() > 1 ) {
			// TODO: allow this
			fprintf(stderr, _("Can not bind to \"%1$s\": Resolves to multiple entries:\n"), options.bind_addr_listen.c_str());
			for( typeof(bind_sa->begin()) i = bind_sa->begin(); i != bind_sa->end(); i++ ) {
				std::cerr << "  " << i->string() << "\n";
			}
			exit(EX_DATAERR);
		}
		s_listen = Socket::socket( (*bind_sa)[0].proto_family() , SOCK_STREAM, 0);
		s_listen.set_reuseaddr();
		s_listen.bind((*bind_sa)[0]);
		s_listen.listen(MAX_CONN_BACKLOG);

#if HAVE_DECL_IP_TRANSPARENT
		int value = 1;
		s_listen.setsockopt(SOL_IP, IP_TRANSPARENT, &value, sizeof(value));
#endif

		/* TRANSLATORS: %1$s contains the listening address
		 */
		LogInfo(_("Listening on %s"), (*bind_sa)[0].string().c_str());

		bind_listen_addr.reset( bind_sa->release(bind_sa->begin()).release() ); // Transfer ownership; TODO: this should be simpeler that double release()
	}

	if( options.bind_addr_outgoing == "client" ) {
		bind_addr_outgoing.reset(NULL);
		LogInfo(_("Outgoing connections will connect from original source address"));
	} else { // Resolve client address
		std::string host, port;

		/* Address format is
		 *   - hostname:portname
		 *   - [numeric ip]:portname
		 *   - hostname:[portnumber]
		 *   - [numeric ip]:[portnumber]
		 */
		size_t c = options.bind_addr_outgoing.rfind(":");
		if( c == std::string::npos ) {
			/* TRANSLATORS: %1$s contains the string passed as option
			 */
			fprintf(stderr, _("Invalid bind string \"%1$s\": could not find ':'\n"), options.bind_addr_outgoing.c_str());
			exit(EX_DATAERR);
		}
		host = options.bind_addr_outgoing.substr(0, c);
		port = options.bind_addr_outgoing.substr(c+1);

		std::auto_ptr< boost::ptr_vector< SockAddr::SockAddr> > bind_sa
			= SockAddr::resolve( host, port, 0, SOCK_STREAM, 0);
		if( bind_sa->size() == 0 ) {
			fprintf(stderr, _("Can not bind to \"%1$s\": Could not resolve\n"), options.bind_addr_outgoing.c_str());
			exit(EX_DATAERR);
		} else if( bind_sa->size() > 1 ) {
			fprintf(stderr, _("Can not bind to \"%1$s\": Resolves to multiple entries:\n"), options.bind_addr_outgoing.c_str());
			for( typeof(bind_sa->begin()) i = bind_sa->begin(); i != bind_sa->end(); i++ ) {
				std::cerr << "  " << i->string() << "\n";
			}
			exit(EX_DATAERR);
		}
		bind_addr_outgoing.reset( bind_sa->release(bind_sa->begin()).release() ); // Transfer ownership; TODO: this should be simpeler that double release()

		LogInfo(_("Outgoing connections will connect from %1$s"), bind_addr_outgoing->string().c_str());
	}

	if( options.fork ) {
		/* Prepare for return value passing from the initialization procedure of the daemon process */
		if (daemon_retval_init() < 0) {
			LogError(_("Failed to create pipe."));
			exit(EX_OSERR);
		}

		pid_t child = daemon_fork();
		if( child < 0 ) {
			LogError(_("Could not fork(): %s"), strerror(errno));
			daemon_retval_done();
			exit(EX_OSERR);
		} else if( child > 0 ) { // parent
			int ret;
			/* Wait for 20 seconds for the return value passed from the daemon process */
			if ((ret = daemon_retval_wait(20)) < 0) {
				LogError(_("Could not recieve return value from daemon process: %s"), strerror(errno));
				exit(EX_OSERR);
			}
			LogInfo(_("Child process [%1$d] forked succesfully, it signaled %2$d"), child, ret);
			exit(ret);
		}
		// Child continues

		/* Close all FDs; 0, 1 and 2 are kept open anyway and point to
		 * /dev/null (done by daemon_fork()) */
		daemon_close_all(s_listen, fileno(logfile), -1);
	}

	if( pidfile != NULL ) { // PID-file
		daemon_pid_file_proc = return_pidfile;
		if( daemon_pid_file_create() ) {
			daemon_retval_send(EX_OSERR);
			exit(EX_OSERR);
		}
	}

	// Let our parent know that we're doing fine
	daemon_retval_send(0);

	{
		ev_signal ev_sigint_watcher;
		ev_signal_init( &ev_sigint_watcher, received_sigint, SIGINT);
		ev_signal_start( EV_DEFAULT_ &ev_sigint_watcher);

		ev_signal ev_sigterm_watcher;
		ev_signal_init( &ev_sigterm_watcher, received_sigterm, SIGTERM);
		ev_signal_start( EV_DEFAULT_ &ev_sigterm_watcher);

		ev_signal ev_sighup_watcher;
		ev_signal_init( &ev_sighup_watcher, received_sighup, SIGHUP);
		ev_signal_start( EV_DEFAULT_ &ev_sighup_watcher);

		ev_signal ev_sigpipe_watcher;
		ev_signal_init( &ev_sigpipe_watcher, received_sigpipe, SIGPIPE);
		ev_signal_start( EV_DEFAULT_ &ev_sigpipe_watcher);


		ev_io e_listen;
		e_listen.data = &s_listen;
		ev_io_init( &e_listen, listening_socket_ready_for_read, s_listen, EV_READ );
		ev_io_start( EV_DEFAULT_ &e_listen );

		LogInfo(_("Setup done, starting event loop"));
		try {
			ev_run(EV_DEFAULT_ 0);
		} catch( std::exception &e ) {
			std::cerr << e.what() << "\n";
			return EX_SOFTWARE;
		}
	}

	LogInfo(_("Exiting cleanly..."));
	daemon_pid_file_remove();
	return EX_OK;
}
