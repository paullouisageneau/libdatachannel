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

#include <getopt.h>
#include <stdlib.h>
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
    {"stunServer", required_argument, NULL, 's'},
    {"stunPort", required_argument, NULL, 't'},
    {"webSocketServer", required_argument, NULL, 'w'},
    {"webSocketPort", required_argument, NULL, 'x'},
    {"help", no_argument, NULL, 'h'},
    {"version", no_argument, NULL, 'v'},
    {NULL, 0, NULL, 0}
  };

  _program_name += argv[0];

  /* default values */
  _s = "stun.l.google.com";
  _t = 19302;
  _w = "localhost";
  _x = 8000;
  _h = false;
  _v = false;

  optind = 0;
  while ((c = getopt_long (argc, argv, "s:t:w:x:hv", long_options, &optind)) != - 1)
    {
      switch (c)
        {
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

        case 'v': 
          _v = true;
          this->version (EXIT_SUCCESS);
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
usage: " << _program_name << " [ -stwxhv ] \n\
libdatachannel client implementing WebRTC Data Channels with WebSocket signaling\n\
   [ -s ] [ --stunServer ] (type=STRING, default=stun.l.google.com)\n\
          Stun server URL or IP address.\n\
   [ -t ] [ --stunPort ] (type=INTEGER, range=0...65535, default=19302)\n\
          Stun server port.\n\
   [ -w ] [ --webSocketServer ] (type=STRING, default=localhost)\n\
          Web socket server URL or IP address.\n\
   [ -x ] [ --webSocketPort ] (type=INTEGER, range=0...65535, default=8000)\n\
          Web socket server port.\n\
   [ -h ] [ --help ] (type=FLAG)\n\
          Display this help and exit.\n\
   [ -v ] [ --version ] (type=FLAG)\n\
          Output version information and exit.\n";
    }
  exit (status);
}

void Cmdline::version (int status)
{
  std::cout << _program_name << " v0.5\n";
  exit (status);
}
