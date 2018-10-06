# Copyright (c) 2017 DaSoftver LLC.
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#    http://www.apache.org/licenses/LICENSE-2.0
#
#    Unless required by applicable law or agreed to in writing, software
#    distributed under the License is distributed on an "AS IS" BASIS,
#    WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
#    See the License for the specific language governing permissions and
#    limitations under the License.

#makefile for Cloudgizer

#set mariadb sock, library and include for MariaDB LGPL client
MARIASOCK="-mariasock /var/lib/mysql/mysql.sock"
MARIAINCLUDE=/usr/local/include/mariadb
MARIALGPLCLIENT=/usr/local/lib/mariadb

#C compiler, gcc only
CC=gcc

#this is where CLD include/library is
FCLIB=/usr/local/lib/cld

#apache version
APACHE_VERSION=204

#build debug or not (1 if debug build)
CLDDEBUG=0

#based on CLDDEBUG from debug file, we use appropriate tags
#Note: we always use -g in order to get line number of where the problem is
#(optimization is still valid though)
OPTIMIZATION_DEBUG=-g -O0 -DDEBUG 
OPTIMIZATION_PROD=-O2 -g
ifeq ($(CLDDEBUG), 1)
OPTIMIZATION=$(OPTIMIZATION_DEBUG)
else
OPTIMIZATION=$(OPTIMIZATION_PROD)
endif

#C flags are as strict as we can do, in order to discover as many bugs as early on
CFLAGS=-std=gnu89 -Werror -Wall -Wextra -Wuninitialized -Wmissing-declarations -Wformat -Wno-format-zero-length -fPIC -I $(MARIAINCLUDE) 
#the same flags, just with comma for apxs "-Wc," flag
CFLAGSMOD=-Wc,-std=gnu89 -Wc,-Werror -Wc,-Wall -Wextra -Wc,-Wuninitialized -Wc,-Wmissing-declarations -Wc,-Wformat -Wc,-Wno-format-zero-length 

#This is for building object code that's part of installation, and NOT the final product - 
#final product is made at customer site ONLY because we do NOT distribute any libraries (shared or linked)!
LDFLAGSLOCAL=-Wl,-no-as-needed -ldl -rdynamic

#linker flags include mariadb (LGPL), crypto (OpenSSL, permissive license). This is for building object code that's part 
#this is for installation at customer's site where we link CLD with mariadb (LGPL), crypto (OpenSSL)
LDFLAGSINSTALL=-Wl,-no-as-needed -L$(MARIALGPLCLIENT) -lmariadb -lcrypto -ldl -rdynamic -Wl,--rpath=$(FCLIB)

#this is what we build during development
all:   libcld.so cld.o cld.a libacld.so apachemod

#build apache mod stuff	
apachemod: .libs/mod_cld.o


clean:
	touch *.c
	touch *.h


#this is building CLD at customer's site - customer MUST have mariadb,OpenSSL and curl installed - we 
#do NOT distribute these!
makecld:
	$(CC)  -o cld cld.o cld.a $(LDFLAGSINSTALL) 

#
# The rest is building object files. a_* is for web server (apache) module
# and with a_ is for command line use (a libraries for each case). Since we 
# do not link any apache libs for command line use, we need two different sets
# of object files.
# Other than CLD preprocessor, we do NOT use any libraries at customer's site - 
# the Makefile for application (such as in hello world example) will link with
# those libraries AT customer site.
#
cld.o: cld.c
	$(CC) -c -o $@ $< $(CFLAGS) $(LDFLAGSLOCAL) $(OPTIMIZATION) 

.libs/mod_cld.o: mod_cld.c cld.h
	apxs -D APACHE_VERSION=$(APACHE_VERSION) -c $(CFLAGSMOD) $< -fPIC 

libacld.so: a_mys.o a_sec.o a_chandle.o a_cldrt.o a_cldrtc.o cldmem.o
	rm -f libacld.so
	$(CC) -shared -o libacld.so $^ $(CFLAGS) $(OPTIMIZATION) 

cld.a: mys.o sec.o chandle.o cldrtc.o cldmem.o
	rm -f cld.a
	ar rcs cld.a $^ 

libcld.so: mys.o sec.o chandle.o cldrt.o cldrtc.o cldmem.o 
	rm -f libcld.so
	$(CC) -shared -o libcld.so $^ $(CFLAGS) $(OPTIMIZATION) 

a_chandle.o: chandle.c cld.h
	$(CC) -c -o $@ $< $(CFLAGS) $(OPTIMIZATION) -DAMOD 

chandle.o: chandle.c cld.h
	$(CC) -c -o $@ $< $(CFLAGS) $(OPTIMIZATION)

a_mys.o: mys.c cld.h
	$(CC) -c -o $@ $< $(CFLAGS) $(OPTIMIZATION) -DAMOD 

mys.o: mys.c cld.h
	$(CC) -c -o $@ $< $(CFLAGS) $(OPTIMIZATION)

a_sec.o: sec.c cld.h
	$(CC) -c -o $@ $< $(CFLAGS) $(OPTIMIZATION) -DAMOD 

sec.o: sec.c cld.h
	$(CC) -c -o $@ $< $(CFLAGS) $(OPTIMIZATION)

a_cldrtc.o: cldrtc.c cld.h
	$(CC) -c -o $@ $< $(CFLAGS) $(OPTIMIZATION) -DAMOD 

cldrtc.o: cldrtc.c cld.h
	$(CC) -c -o $@ $< $(CFLAGS) $(OPTIMIZATION)

a_cldrt.o: cldrt.c cld.h
	$(CC) -c -o $@ $< $(CFLAGS) $(OPTIMIZATION) -DAMOD 

cldrt.o: cldrt.c cld.h
	$(CC) -c -o $@ $< $(CFLAGS) $(OPTIMIZATION)

cldmem.o: cldmem.c cld.h
	$(CC) -c -o $@ $< $(CFLAGS) $(OPTIMIZATION)



