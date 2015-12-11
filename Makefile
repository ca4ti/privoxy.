# $Id: Makefile,v 1.5.2.1 2002/08/05 17:46:13 oes Exp $
#
# Written by and Copyright (C) 2001 the SourceForge
# Privoxy team. http://www.privoxy.org/
#
# Based on the Internet Junkbuster originally written
# by and Copyright (C) 1997 Anonymous Coders and 
# Junkbusters Corporation.  http://www.junkbusters.com
#
# This program is free software; you can redistribute it 
# and/or modify it under the terms of the GNU General
# Public License as published by the Free Software
# Foundation; either version 2 of the License, or (at
# your option) any later version.
#
# This program is distributed in the hope that it will
# be useful, but WITHOUT ANY WARRANTY; without even the
# implied warranty of MERCHANTABILITY or FITNESS FOR A
# PARTICULAR PURPOSE.  See the GNU General Public
# License for more details.
#
# The GNU General Public License should be included with
# this file.  If not, you can view it at
# http://www.gnu.org/copyleft/gpl.html
# or write to the Free Software Foundation, Inc., 59
# Temple Place - Suite 330, Boston, MA  02111-1307, USA.
#
# $Log: Makefile,v $
# Revision 1.5.2.1  2002/08/05 17:46:13  oes
# Change make to gmake to fix auto-build on Solaris
#
# Revision 1.5  2002/04/11 12:51:34  oes
# Bugfix
#
# Revision 1.4  2002/04/09 16:38:10  oes
# Added option to run the whole build process
#
# Revision 1.3  2002/03/26 22:29:54  swa
# we have a new homepage!
#
# Revision 1.2  2002/03/24 13:25:42  swa
# name change related issues
#
# Revision 1.1  2001/12/01 11:24:29  jongfoster
# Will display a warning if non-GNU make is used
#
#

#############################################################################

error:
	@if [ -f GNUmakefile ]; then \
	    echo "***"; \
	    echo "*** You are not using the GNU version of Make - maybe it's called gmake"; \
	    echo "*** or it's in a different directory?"; \
	    echo "***"; \
	    exit 1; \
	 else \
	    echo "***"; \
	    echo "*** To build this program, you must run"; \
	    echo "*** autoheader && autoconf && ./configure and then run GNU make."; \
	    echo "***"; \
	    echo -n "*** Shall I do this for you now? (y/n) "; \
	    read answer; \
	    if [ $$answer = "y" ]; then \
	       autoheader && autoconf && ./configure && gmake; \
	    fi; \
	 fi

.PHONY: error

#############################################################################

## Local Variables:
## tab-width: 3
## end:
