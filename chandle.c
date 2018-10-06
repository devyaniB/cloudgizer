/*
Copyright (c) 2017 DaSoftver LLC.

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

    Unless required by applicable law or agreed to in writing, software
    distributed under the License is distributed on an "AS IS" BASIS,
    WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
    See the License for the specific language governing permissions and
    limitations under the License.
*/

//
// Handling of fatal crashes, or controlled program aborts.
// The purpose is to create a backtrace file which contains the
// stack, together with source file name/line numbers, when a 
// crash occurs.
//
// For the source code/line number reporting to work,  -g and
// -rdynamic (most likely) must be used. 
//

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <stdlib.h>
#include <stdio.h>
#include <signal.h>
#include <assert.h>
#include <execinfo.h>
#include <string.h>
#include <stdlib.h>
#include <stdarg.h>
#include <assert.h>
#include <unistd.h>
#include <time.h>
#include <link.h>
#include <sys/resource.h>
#include "cld.h"

// *******************
// NO CALLS TO CODE OUTSIDE OF THIS MODULE MUST BE MADE AND NO CLD_TRACE()!!
// *******************
// Otherwise, those calls' tracing would always place
// the last 'traced' (i.e. visited) location, right there
// and not in the place where it happened
// Meaning, as a crash-handling code, all of it is right here.
 
// limitations on stack size, intended to be reasonably well sized
//
#define MAX_STACK_FRAMES 512
#define MAX_EXPL_LEN 1024


// Describes the shared libraries used, in particular the name and address range for each
// so that at run time we find out for each stack element where exactly it comes from - and determine
// the exact source code location. We assume program has less than MAX_SO shared objects used
#define MAX_SO 512

// these are generally set elsewhere for usage here, they provide
// additional information about where we were when crash happened
const char *func_name; // name of the last function we were in, as set by the tracing system (CLD_TRACE)
int func_line; // function line (last known) as set by CLD_TRACE

// Static variables to be used in the case of a crash
static void *stack_dump[MAX_STACK_FRAMES]; // stack frame`
static char timestr[100];
static char expla[MAX_EXPL_LEN + 1];
static char backtrace_file[400];
static char backtrace_start[sizeof(backtrace_file)+500];
static so_info so[MAX_SO]; // info on all shared libraries linked with this program
static int total_so = 0; // total number of shared libraries we found on startup

// function prototypes
int addr2line(void const * const addr, const char *fname);
void posix_print_stack_trace();
void signal_handler(int sig);
void set_signal_handler();
void cld_get_time_crash (char *outstr, int outstrLen);
int modinfo(struct dl_phdr_info *info, size_t size, void *data);


// 
// Resolve symbol name and source location given the path to the executable 
// and an address 
// addr is program address for which to find line #.
// fname is file name where to write.
// Returns exit code from addr2line
// This function is called multiple times (once for each line
//      on backtrace), so we use >> to add to output
//
int addr2line(void const * const addr, const char *fname)
{
    char addr2line_cmd[512] = {0};
    assert (fname);
    assert (addr);
    int it;

    //
    // Go through all shared objects and find out where is the address on the stack located.
    // This allows us to find base address and shared object name, and thus the source code location.
    //
    for (it = 0; it < total_so; it++)
    {
       //
       // Is address in question between the start and end address of shared object?
       //
       if (so[it].mod_addr<=addr && addr<=so[it].mod_end) 
       {
           break;
       }
    }


    if (it == total_so)
    {
        //
        // This should NEVER happen, we couldn't find the address in any shared object!! We just default to first one
        // even if it won't really work
        //
        it = 0;
    }

    // get line information for an address, and put it into a backtrace file
    // that has timestamp and process number. We do this by finding the actual RELATIVE
    // address of the fault address (addr) and the load address of this executable module (mod_addr)
    snprintf(addr2line_cmd, sizeof(addr2line_cmd)-1, "addr2line -f -e %s 0x%lx |grep -v \"??\" >> %s", so[it].mod_name, (unsigned long)(addr-so[it].mod_addr),
        fname);
    
    // execute addr2line, which is a Linux utility that does this 
    return system(addr2line_cmd);
}

// 
// Get stack trace for current execution, then abort program.
//
void posix_print_stack_trace()
{
    cld_get_stack(backtrace_file);
    cld_report_error ("Something went wrong, see backtrace file");
}

// 
// Obtain backtrace, and write information to output file fname.
// Obtain each stack item and process it to obtain file name and line number.
// Print out along side other information for debugging and post-mortem.
// Do not call this more than once, because certain failures may loop back here
// and go into infinite loop, destroying all useful information.
//
void cld_get_stack(const char *fname)
{
    //
    // This static variable is okay because if we're here, the program WILL end right here in this module.
    // So after it restarts, this static variable will re-initialize.
    //
    static int was_here = 0;
    int i = 0;
    int trace_size = 0;
    char **dump_msg = (char **)NULL;
    int rs;

    if (was_here == 1)
        return;

    was_here = 1;
    CLD_UNUSED(rs);

    // get stack and symbols
    trace_size = backtrace(stack_dump, MAX_STACK_FRAMES);
    dump_msg = backtrace_symbols(stack_dump, trace_size);
 
    sprintf(backtrace_start, "echo 'START STACK DUMP ***********' >> %s", fname);
    rs = system (backtrace_start);

    cld_get_time_crash (timestr, sizeof(timestr)-1);
    sprintf(backtrace_start, "echo '%d: %s: %s last known tracing file/line: [%s][%d]' >> %s", getpid(), timestr, expla, func_name, func_line, backtrace_file);
    rs = system (backtrace_start);

    // get source and line number for each stack line item
    for (i = 0; i < trace_size; ++i)
    {
        // try to display what we can
        // we don't check for return value because some lines with ??
        // are filtered out (we only look for source line in the module we originate from)

        // divide stack entries
        sprintf(backtrace_start, "echo '-----' >> %s",  fname);
        rs = system (backtrace_start);

        // display source file/line number
        addr2line(stack_dump[i], fname);
        sprintf(backtrace_start, "echo '%s' >> %s", dump_msg[i], fname);
        rs = system (backtrace_start);
    }
    sprintf(backtrace_start, "echo 'END STACK DUMP ***********' >> %s", fname);
    rs = system (backtrace_start);

    // skip freeing to avoid potential issues SIGKILL
    //if (dump_msg) { free(dump_msg); } 

}

// 
// Signal handler for signal sig. sig is signal number
// This way at run time we know which signal was caught. We also core dump for 
// more information.
//
void signal_handler(int sig)
{

    sprintf(backtrace_start, "echo '***\n***\n***\n' >> %s", backtrace_file);
    int rs = system (backtrace_start);
    CLD_UNUSED(rs);
    switch(sig)
    {
        case SIGUSR2:
            // ignore USR2 as it has no meaning in CLD. 
        case SIGUSR1:
            // Apache sends SIGUSR1 when it wants to exit
            snprintf(backtrace_start, sizeof(backtrace_start)-1, "echo 'SIGUSR caught: web server is terminating the program' >> %s", backtrace_file);
            system(backtrace_start);
            _Exit(2); // exit immediately, since we may not even be in the request (i.e. don't go finishing a request),
                        // this is OK since transaction will be rolled back
        case SIGPIPE:
            return; // ignore SIGNPIPE to avoid superfluous issues
        case SIGABRT:
            strncpy(expla, "Caught SIGABRT: usually caused by an abort() or assert()\n", MAX_EXPL_LEN - 1);
            break;
        case SIGFPE:
            strncpy(expla, "Caught SIGFPE: math exception, such as divide by zero\n",
                MAX_EXPL_LEN - 1);
            break;
        case SIGILL:
            strncpy(expla, "Caught SIGILL: illegal code\n",  MAX_EXPL_LEN - 1);
            break;
        case SIGINT:
            strncpy(expla, "Caught SIGINT: interrupt signal, a ctr-c?\n",
                 MAX_EXPL_LEN - 1);
            break;
        case SIGBUS:
            strncpy(expla, "Caught SIGBUS: bus error\n",  MAX_EXPL_LEN - 1);
            break;
        case SIGSEGV:
            strncpy(expla, "Caught SIGSEGV: segmentation fault\n",  MAX_EXPL_LEN - 1);
            break;
        case SIGHUP:
            snprintf(backtrace_start, sizeof(backtrace_start)-1, "echo 'SIGHUP caught: hanging up now (terminating)' >> %s", backtrace_file);
            system(backtrace_start);
            _Exit(2); // exit immediately, since we may not even be in the request (i.e. don't go finishing a request),
                        // this is OK since transaction will be rolled back
        case SIGTERM:
            snprintf(backtrace_start, sizeof(backtrace_start)-1, "echo 'SIGTERM caught: someone is terminating the program' >> %s", backtrace_file);
            system(backtrace_start);
            _Exit(2); // exit immediately, since we may not even be in the request (i.e. don't go finishing a request),
                        // this is OK since transaction will be rolled back
        default:
            // this really should not happen since we handled all the signals we trapped, just in case
            snprintf(expla, sizeof(expla)-1, "Caught something not handled, signal [%d]\n", sig);
            break;
    }

    // 
    // Printout stack trace
    //
    posix_print_stack_trace();


    _Exit(1);
}
 

// 
// Set each signal handler, this must be called asap in the program
//
void set_signal_handler()
{
    struct sigaction psa;
    memset (&psa, 0, sizeof (psa));
    psa.sa_handler = signal_handler;
    sigaction(SIGABRT, &psa, NULL);
    sigaction(SIGFPE,  &psa, NULL);
    sigaction(SIGILL,  &psa, NULL);
    sigaction(SIGINT,  &psa, NULL);
    sigaction(SIGSEGV, &psa, NULL);
    sigaction(SIGBUS, &psa, NULL);
    sigaction(SIGTERM, &psa, NULL);
    sigaction(SIGHUP, &psa, NULL);
    sigaction(SIGPIPE, &psa, NULL);
    sigaction(SIGUSR1, &psa, NULL);
    sigaction(SIGUSR2, &psa, NULL);
}



// 
// Obtain load start address for current executable module. This must be 
// deducted from an address obtained by backtrace.. in order to have proper
// source/line number information available.
// Returns 0.
//
int modinfo(struct dl_phdr_info *info, size_t size, void *data)
{
    // set as unused as API is broader than what we need
    CLD_UNUSED(size);
    CLD_UNUSED(data);


    int i;

    // go through a list of segments loaded for this module and pick one we're in
    // and make sure we get the loading address
    for (i = 0; i < info->dlpi_phnum; i++) 
    {
        // get start load address of module
        if (info->dlpi_phdr[i].p_type == PT_LOAD) 
        {
            // this is global module address we will use in addr2line as a base to deduct
            so[total_so].mod_addr = (void *) (info->dlpi_addr + info->dlpi_phdr[i].p_vaddr);

            // get ending module, we use it here to find out if we're in the range of addresses
            // for this module, and if we are, this is the base module for our code
            so[total_so].mod_end = so[total_so].mod_addr + info->dlpi_phdr[i].p_memsz;

            // get module name, we will be using it in addr2line
            snprintf(so[total_so].mod_name,sizeof(so[total_so].mod_name)-1,"%s", info->dlpi_name);
            total_so++;
            if (total_so >= MAX_SO)
            {
                cld_fatal_error("Too many shared libraries to load", "0", 0);
            }
            break;
        }
    }
    return 0;
}
 
//
// This is what's called by generated CLD program at the beginning
// to enable catchng signals and dumping human-readable stack in backtrace file
// 'dir' is the tracing directory where to write trace file
//
void cld_set_crash_handler(const char *dir)
{
    static int modinfo_done = 0; 
    // Once obtained, do not waste time processing loading of other modules and other crash settings, as it 
    // it has already been done - and once used, program goes away. This is per all requests for this module in this process,
    // so once we have found the base addresses and setup other handling, don't do this again in any request! 
    if (modinfo_done==0) 
    {
        // build backtrace file name to be used througout here
        sprintf(backtrace_file, "%s/backtrace", dir);

        // set a hook for module loading, so we go through all modules loaded
        // and then figure out the one we're in and get the info needed for
        // source code/line number resolution. This is always called at the beginning
        // of the program, so we have this info ready in case of crash.
        dl_iterate_phdr(&modinfo, NULL);
        modinfo_done = 1;

        expla[0] = 0;

        // set signal handling
        set_signal_handler();
    }

}

// 
// Get time without tracing, self contained here
// This is on purpose, so no outside calls are made
// see explanation at the top of this file
// This uses localtime on the server
// 'outstr' is the output time, and 'outstrLen' is the 
// length of this buffer.
//
void cld_get_time_crash (char *outstr, int outstrLen)
{
    time_t t;
    struct tm *tmp;

    t = time(NULL);
    tmp = localtime(&t);
    if (tmp == NULL) 
    {
        outstr[0] = 0;
        return; 
    }

    if (strftime(outstr, outstrLen, "%F-%H-%M-%S", tmp) == 0) 
    {
        outstr[0] = 0;
    }
}


//
// Return total number of shared libraries loaded.
// Output variable sos is the list of shared libraries loaded, which we can print out if needed.
//
int cld_total_so(so_info **sos)
{
    *sos =  so;
    return total_so;
}





