/*
 * Copyright (c) 2002 Robert Collins.
 *
 *     This program is free software; you can redistribute it and/or modify
 *     it under the terms of the GNU General Public License as published by
 *     the Free Software Foundation; either version 2 of the License, or
 *     (at your option) any later version.
 *
 *     A copy of the GNU General Public License can be found at
 *     http://www.gnu.org/
 *
 * Written by Robert Collins <robertc@hotmail.com>
 *
 */

#ifndef _FINDVISITOR_H_
#define _FINDVISITOR_H_

class String;
/* For the wfd definition. See the TODO in find.cc */
#include "win32.h"

class FindVisitor
{
public:
  virtual void visitFile(String const &basePath, WIN32_FIND_DATA const *);
  virtual void visitDirectory(String const &basePath, WIN32_FIND_DATA const *);
  virtual ~ FindVisitor ();
protected:
  FindVisitor ();
  FindVisitor (FindVisitor const &);
  FindVisitor & operator= (FindVisitor const &);
};

#endif // _FINDVISITOR_H_