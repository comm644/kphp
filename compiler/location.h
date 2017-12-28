#pragma once
#include "compiler/data_ptr.h"

class Location {
public:
  SrcFilePtr file;
  FunctionPtr function;
  int line;

  Location();

  void set_file (SrcFilePtr file);
  void set_function (FunctionPtr function);
  void set_line (int line);
  SrcFilePtr get_file() const;
  FunctionPtr get_function() const;
  int get_line() const;
};

bool operator<(Location const & lhs, Location const & rhs);
