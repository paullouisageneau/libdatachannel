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

Cmdline::Cmdline (int argc, char *argv[]) // ISO C++17 not allowed: throw (std::string )
{
  extern char *optarg;
  extern int optind;
  int c;

  static struct option long_options[] =
  {
    {"noStun", no_argument, NULL, 'n'},
    {"udpMux", no_argument, NULL, 'm'},
    {"stunServer", required_argument, NULL, 's'},
    {"stunPort", required_argument, NULL, 't'},
    {"webSocketServer", required_argument, NULL, 'w'},
    {"webSocketPort", required_argument, NULL, 'x'},
    {"help", no_argument, NULL, 'h'},
    {NULL, 0, NULL, 0}
  };

  _program_name += argv[0];

  /* default values */
  _n = false;
  _m = false;
  _s = "stun.l.google.com";
  _t = 19302;
  _w = "localhost";
  _x = 8000;
  _h = false;

  optind = 0;
  while ((c = getopt_long (argc, argv, "s:t:w:x:enmhv", long_options, &optind)) != - 1)
    {
      switch (c)
        {
        case 'n':
          _n = true;
          break;

		case 'm':
		  _m = true;
		  break;

        case 's':
          _s = optarg;
          break;

        case 't':
          _t = atoi (optarg);
          if (_t < 0)
            {
              std::string err;
              err += "parameter range error: t must be >= 0";
              throw (std::range_error(err));
            }
          if (_t > 65535)
            {
              std::string err;
              err += "parameter range error: t must be <= 65535";
              throw (std::range_error(err));
            }
          break;

        case 'w':
          _w = optarg;
          break;

        case 'x':
          _x = atoi (optarg);
          if (_x < 0)
            {
              std::string err;
              err += "parameter range error: x must be >= 0";
              throw (std::range_error(err));
            }
          if (_x > 65535)
            {
              std::string err;
              err += "parameter range error: x must be <= 65535";
              throw (std::range_error(err));
            }
          break;

        case 'h':
          _h = true;
          this->usage (EXIT_SUCCESS);
          break;

        default:
          this->usage (EXIT_FAILURE);

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

void Cmdline::usage (int status)
{
  if (status != EXIT_SUCCESS)
    std::cerr << "Try `" << _program_name << " --help' for more information.\n";
  else
    {
      std::cout << "\
usage: " << _program_name << " [ -enstwxhv ] \n\
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
   [ -m ] [ --udpMux ] (type=FLAG)\n\
          Use UDP multiplex.\n\
   [ -h ] [ --help ] (type=FLAG)\n\
          Display this help and exit.\n";
    }
  exit (status);
}

