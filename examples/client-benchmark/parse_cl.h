/******************************************************************************
**
** parse_cl.h
**
** Thu Aug  6 19:42:25 2020
** Linux 5.4.0-42-generic (#46-Ubuntu SMP Fri Jul 10 00:24:02 UTC 2020) x86_64
** cerik@Erik-VBox-Ubuntu (Erik Cota-Robles)
**
** Copyright (c) 2020 Erik Cota-Robles
**
** Header file for command line parser class
**
** Automatically created by genparse v0.9.3
**
** See http://genparse.sourceforge.net for details and updates
**
******************************************************************************/

#ifndef CMDLINE_H
#define CMDLINE_H

#include <iostream>
#include <string>

/*----------------------------------------------------------------------------
**
** class Cmdline
**
** command line parser class
**
**--------------------------------------------------------------------------*/

class Cmdline
{
private:
  /* parameters */
  bool _n;
  std::string _s;
  int _t;
  std::string _w;
  int _x;
  bool _h;
  int _d;
  bool _o;
  bool _p;
  int _r;
  int _b;
  int _c;

  /* other stuff to keep track of */
  std::string _program_name;
  int _optind;

public:
  /* constructor and destructor */
  Cmdline (int, char **); // ISO C++17 not allowed: throw (std::string);
  ~Cmdline (){}

  /* usage function */
  void usage (int status);

  /* return next (non-option) parameter */
  int next_param () { return _optind; }

  bool noStun () const { return _n; }
  std::string stunServer () const { return _s; }
  int stunPort () const { return _t; }
  std::string webSocketServer () const { return _w; }
  int webSocketPort () const { return _x; }
  bool h () const { return _h; }
  int durationInSec () const { return _d; }
  bool noSend () const { return _o; }
  int bufferSize() const { return _b; }
  bool enableThroughputSet () const { return _p; }
  int throughtputSetAsKB() const { return _r; }  
  int dataChannelCount() const { return _c; }
};

#endif
