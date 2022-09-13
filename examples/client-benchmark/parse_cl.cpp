/******************************************************************************
**
** parse_cl.cpp
**
** Thu Aug  6 19:42:25 2020
** Linux 5.4.0-42-generic (#46-Ubuntu SMP Fri Jul 10 00:24:02 UTC 2020) x86_64
** cerik@Erik-VBox-Ubuntu (Erik Cota-Robles)
**
** Copyright (c) 2020 Erik Cota-Robles
**
** Definition of command line parser class
**
** Automatically created by genparse v0.9.3
**
** See http://genparse.sourceforge.net for details and updates
**
**
******************************************************************************/

#include <stdlib.h>

#if defined(_WIN32) || defined(WIN32)
#include "getopt.h"
#else
#include <getopt.h>
#endif

#include "parse_cl.h"

/*----------------------------------------------------------------------------
**
** Cmdline::Cmdline ()
**
** Constructor method.
**
**--------------------------------------------------------------------------*/

Cmdline::Cmdline(int argc, char *argv[]) // ISO C++17 not allowed: throw (std::string )
{
	extern char *optarg;
	extern int optind;
	int c;

	static struct option long_options[] = {{"noStun", no_argument, NULL, 'n'},
	                                       {"stunServer", required_argument, NULL, 's'},
	                                       {"stunPort", required_argument, NULL, 't'},
	                                       {"webSocketServer", required_argument, NULL, 'w'},
	                                       {"webSocketPort", required_argument, NULL, 'x'},
	                                       {"durationInSec", required_argument, NULL, 'd'},
	                                       {"noSend", no_argument, NULL, 'o'},
	                                       {"enableThroughputSet", no_argument, NULL, 'p'},
	                                       {"throughtputSetAsKB", required_argument, NULL, 'r'},
	                                       {"bufferSize", required_argument, NULL, 'b'},
										   {"dataChannelCount", required_argument, NULL, 'c'},
	                                       {"help", no_argument, NULL, 'h'},
	                                       {NULL, 0, NULL, 0}};

	_program_name += argv[0];

	/* default values */
	_n = false;
	_s = "stun.l.google.com";
	_t = 19302;
	_w = "localhost";
	_x = 8000;
	_h = false;
	_d = 300;
	_o = false;
	_p = false;
	_r = 300;
	_b = 0;
	_c = 1;

	optind = 0;
	while ((c = getopt_long(argc, argv, "s:t:w:x:d:r:b:c:enhvop", long_options, &optind)) != -1) {
		switch (c) {
		case 'n':
			_n = true;
			break;

		case 's':
			_s = optarg;
			break;

		case 't':
			_t = atoi(optarg);
			if (_t < 0) {
				std::string err;
				err += "parameter range error: t must be >= 0";
				throw(std::range_error(err));
			}
			if (_t > 65535) {
				std::string err;
				err += "parameter range error: t must be <= 65535";
				throw(std::range_error(err));
			}
			break;

		case 'w':
			_w = optarg;
			break;

		case 'x':
			_x = atoi(optarg);
			if (_x < 0) {
				std::string err;
				err += "parameter range error: x must be >= 0";
				throw(std::range_error(err));
			}
			if (_x > 65535) {
				std::string err;
				err += "parameter range error: x must be <= 65535";
				throw(std::range_error(err));
			}
			break;

		case 'd':
			_d = atoi(optarg);
			if (_d < 0) {
				std::string err;
				err += "parameter range error: d must be >= 0";
				throw(std::range_error(err));
			}
			break;

		case 'o':
			_o = true;
			break;

		case 'b':
			_b = atoi(optarg);
			if (_b < 0) {
				std::string err;
				err += "parameter range error: b must be >= 0";
				throw(std::range_error(err));
			}
			break;

		case 'p':
			_p = true;
			break;

		case 'r':
			_r = atoi(optarg);
			if (_r <= 0) {
				std::string err;
				err += "parameter range error: r must be > 0";
				throw(std::range_error(err));
			}
			break;

		case 'c':
			_c = atoi(optarg);
			if (_c <= 0) {
				std::string err;
				err += "parameter range error: c must be > 0";
				throw(std::range_error(err));
			}
			break;

		case 'h':
			_h = true;
			this->usage(EXIT_SUCCESS);
			break;

		default:
			this->usage(EXIT_FAILURE);
		}
	} /* while */

	_optind = optind;
}

/*----------------------------------------------------------------------------
**
** Cmdline::usage () and version()
**
** Print out usage (or version) information, then exit.
**
**--------------------------------------------------------------------------*/

void Cmdline::usage(int status) {
	if (status != EXIT_SUCCESS)
		std::cerr << "Try `" << _program_name << " --help' for more information.\n";
	else {
		std::cout << "\
usage: " << _program_name
		          << " [ -enstwxdobprhv ] \n\
libdatachannel client implementing WebRTC Data Channels with WebSocket signaling\n\
   [ -n ] [ --noStun ] (type=FLAG)\n\
          Do NOT use a stun server (overrides -s and -t).\n\
   [ -s ] [ --stunServer ] (type=STRING, default=stun.l.google.com)\n\
          STUN server URL or IP address.\n\
   [ -t ] [ --stunPort ] (type=INTEGER, range=0...65535, default=19302)\n\
          STUN server port.\n\
   [ -w ] [ --webSocketServer ] (type=STRING, default=localhost)\n\
          Web socket server URL or IP address.\n\
   [ -x ] [ --webSocketPort ] (type=INTEGER, range=0...65535, default=8000)\n\
          Web socket server port.\n\
   [ -d ] [ --durationInSec ] (type=INTEGER, range>=0...INT32_MAX, 0:infinite(INT32_MAX), Valid only for offering client, default=300)\n\
          Benchmark duration in seconds.\n\
   [ -o ] [ --noSend ] (type=FLAG)\n\
          Do NOT send message (Only Receive, for one-way testing purposes).\n\
   [ -b ] [ --bufferSize ] (type=INTEGER, range>0...INT_MAX, default=0)\n\
          Set internal buffer size .\n\
   [ -p ] [ --enableThroughputSet ] (type=FLAG)\n\
          Send a constant data per second (KB). See throughtputSetAsKB params.\n\
   [ -r ] [ --throughtputSetAsKB ] (type=INTEGER, range>0...INT_MAX, default=300)\n\
          Send constant data per second (KB).\n\
   [ -c ] [ --dataChannelCount ] (type=INTEGER, range>0...INT_MAX, default=1)\n\
          Dat Channel count to create.\n\
   [ -h ] [ --help ] (type=FLAG)\n\
          Display this help and exit.\n";
	}
	exit(status);
}
