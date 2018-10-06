/*
Copyright 2017 DaSoftver LLC

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
// Library used both by CLD utility and CLD run-time 
// trailing 'c' in the name of this file refers to 'common' 
// code.
//


#include "cld.h"



// these are used in crash-handler. We set these whenever we trace
// and they are the last location we traced. Crash handler uses these
// to provide more context about the crash.
extern const char *func_name;
extern int func_line;



// 
// Trace execution. This is called from CLD_TRACE. 
// 'trace_level' is currently always 1. It is compared with the trace parameter in debug file, which is currently
// 0 or 1 (no tracing for 0 and tracing for 1). In the future, there may be more trace levels, with trace level 1
// including all tracing, trace level 2 including levels 2,3.. in trace file, etc.
// 'from_file' is the file name this trace is coming from.
// 'from_line' is the source line number, 'from_fun' is the function name.
// The rest (format,...) is printf-like data, the actual trace content.
// The trace also includes current time and PID.
//
// Trace can be called from memory function like cld_realloc. In this case, cld_check_memory may fail and call here.
// This will call cld_checkmem() which will call cld_check_memory again, which will call here again. This time, however,
// cld_checkmem will NOT be called and the trace call will succeed. Then there is abort() in cld_check_memory right after call which will call
// trace again but again cld_checkmem will NOT be called again because we never exited the first call to it, and so in_memory_check
// is still 1 and will remain so until the program exits. 
// If trace is called from anywhere else other than cld_* functions, it will  work the same way except there is no double calling of cld_check_memory.
//
void trace_cld(int trace_level, const char *from_file, int from_line, const char *from_fun, const char *format, ...)
{
    cld_config *pc = cld_get_config();
    if (pc == NULL) CLD_FATAL_HANDLER ("Cannot allocate process configuration");

    // these are captured even if trace is turned off so can we report func/file name
    // in case of a crash
    if (pc->trace.in_trace == 0)  // exclude calls made in this very function
                    // as otherwise, this function would always be the last
                    // visited
    {
        func_name = from_file;
        func_line = from_line;
    }


    // THIS FUNCTON MUST NEVER USE ANY FORM OF MALLOC OR CLD_MALLOC
    // or it may fail when memory is out or not available (such as in cld_malloc)

    //
    // if memory check enabled, check it BEFORE we return from here (see below trace_level check) because we want memory
    // check to happen even if trace does not.
    // This check MUST be before checking for in_trace==0 below as well because if there is a problem and it needs to be 
    // shown, the call to tracel from cld_checkmem() would be ignored otherwise.
    //
    if (pc->debug.memory_check == 1)
    {
        //
        // Keep this code below (within the above if) as small as possible - no extra bells and whistles,
        // no tracing here, just memory check.
        //
        // if this call comes from memory checking (i.e. from cld_checkmem() here), ignore it, or we'd 
        // have infinite recursion.
        //
        if (pc->trace.in_memory_check == 0) 
        {
            pc->trace.in_memory_check = 1;
            // from cld_checkmem, there can be calls to this very function (trace_cld)
            // so we must not come back here if that happens - only after we're done - this
            // is what in_memory_check guards against.
            cld_checkmem();
            pc->trace.in_memory_check = 0;
        }
    }


    // control which tracing will be done
    if (pc->debug.trace_level < trace_level) return;

    char trc[CLD_TRACE_LEN + 1];

    // if this tracing came from this very function, ignore
    if (pc->trace.in_trace == 1) return;
    pc->trace.in_trace = 1;

    if (pc->trace.f == NULL) 
    {
        pc->trace.in_trace = 0;
        return;
    }

    va_list args;
    va_start (args, format);
    vsnprintf (trc, sizeof(trc) - 1, format, args);
    va_end (args);

    // write time, code and message out
    // do NOT use pc->trace.time - this MUST stay constant during the request because it is
    // used in save_HTML to make sure name generated from this value is the same even if this name
    // is generated multiple times.
    // We do not specify PID as it is embedded in file name.
    char curr_time[200];
    cld_current_time (curr_time, sizeof(curr_time)-1);
    fprintf (pc->trace.f, "%s (%s:%d)| %s %s\n", curr_time, from_file, from_line, from_fun, trc);
    //
    // We do not fflush() here - this is done either at the end of request (cld_shut()) or
    // when program crashes (cld_report_error())
    //
    pc->trace.in_trace = 0;
}

// 
// Get PID
//
inline int cld_getpid ()
{
    CLD_TRACE ("");
    return (int) getpid();
}


// 
// Get current time
// Output: outstr is the time string. 
// out_str_len is time string buffer size
// If can't get time, output is empty string.
//
void cld_current_time (char *outstr, int out_str_len)
{
    CLD_TRACE("");
    time_t t;
    struct tm *tmp;

    // get current time zone - may be set by customer program!
    char *curr_time_zone = getenv("TZ");
#define restore_curr_time_zone if (curr_time_zone!=NULL && curr_time_zone[0]!=0) { putenv(curr_time_zone); tzset(); }

    // set time zone to local - we did this in main() first thing before customer code. We cast cld_get_tz()
    // into mutable char * because putenv does NOT modify its string. The result of cld_get_tz must NOT change by 
    // callers.
    putenv((char*)cld_get_tz());
    tzset();

    t = time(NULL);
    tmp = localtime(&t);
    if (tmp == NULL) 
    {
        // return to customer TZ
        restore_curr_time_zone
        outstr[0] = 0;
        return; 
    }

    if (strftime(outstr, out_str_len, "%F-%H-%M-%S", tmp) == 0) 
    {
        outstr[0] = 0;
    }
    // return to customer TZ
    restore_curr_time_zone
}


// cld_clear_config MUST be called at the beginning of the "main" code - otherwise its data WILL
// be released in cld_done() and we will end up using bad data

// 
// Both configuration and run-time information (context, debug, trace, etc.)
// This is really a program  context.
// This static is okay, because cld_clear_config() is ALWAYS called at the very start of the
// request. So cld_pc is NEVER cross-request, i.e. it's always set and consumed in each request. THus
// various loaded modules (which take turns in the same process, but NEVER simultaneously) can use it without any chance of it
// carrying (wrong) value from previous request in the same process. 
//
static cld_config *cld_pc;

//
// Clear configuration. We NEVER carry anything from one request to another. Everything is NEW
// for each request. This function is ALWAYS called upon each request. 
//
inline void cld_clear_config()
{
    cld_pc = NULL;
}

//
// Get config and context data
//
inline cld_config *cld_get_config()
{
    if (cld_pc == NULL)
    {
        cld_pc = (cld_config*)cld_malloc (sizeof(cld_config));
        if (cld_pc == NULL)
        {
            CLD_FATAL_HANDLER ("Cannot allocate process configuration");
        }
        cld_init_config (cld_pc);
    }
    return cld_pc;
}


// 
// Handle fatal error, such that error reporting has failed, or that it may be
// unsafe to call it. Last resort calling, should be used in desperation.
// 'errtext' is the error text, and fname/lnum is file name and line number
// that provide some context.
//
void cld_fatal_error (const char *errtext, const char *fname, int lnum)
{
    // !!!! HERE, PC CAN BE NULL - see cld_config() above, it would call this function
    // MUST NOT USE MALLOC or cld_malloc
    // THese static variables are okay because this function ALWAYS exits the program, so
    // the next process of this program will have them all anew.

    // Only was_here static is actually functional
    static int was_here = 0;
    // THe other two statics only increase chance stack won't run out of memory
    static char err_name[512];
    static char time[CLD_TIME_LEN + 1];
    
    if (was_here == 1) exit(-1); // recursive calls must end, this function must always exit!

    was_here = 1;

    // get user information
    uid_t uid = geteuid(); 
    struct passwd *pwd = getpwuid(uid); 
    if (pwd == NULL) exit(-1);

    // here we don't use ->log_directory because it may not have been set yet
    snprintf (err_name, sizeof (err_name) - 1, "%s/" CLD_TRACE_DIR  "/fatal_error", pwd->pw_dir);
    FILE *f = fopen (err_name, "a+");
    if (f == NULL) f = fopen (err_name, "w+");
    if (f == NULL) exit(-1);
    cld_current_time (time, sizeof(time)-1);

    // write error
    fprintf (f, "%s: %d: Fatal error occurred in application: [%s], file [%s], line [%d]\n", time, cld_getpid(), errtext, fname, lnum);
    fclose (f);

#ifndef AMOD
    // if in web context, send out error message, do not include actual message, it has been written to trace file above
    // do include PID to be able to find it, should customer inform you of the message
    printf ("Content-type: text/html\n\n");
    printf ("Application has encountered an unexpected error, process id [%d].\n", 
        cld_getpid());
    printf ("<br/>Please contact application owner about this message.");
    printf ("<hr/>");
#else
    // this MUST BE THE LAST PIECE so if cld_get_config() comes back here,
    // we just exit , otherwise we'll be stuck in recursive calls between cld_fatal_error and
    // cld_get_config
    cld_config *pc = cld_get_config();
    if (pc != NULL)
    {
        cld_ws_set_content_type(pc->ctx.apa, "text/html");
        cld_ws_printf (pc->ctx.apa, "Application has encountered an unexpected error, process id [%d].\n", 
            cld_getpid());
        cld_ws_printf (pc->ctx.apa, "%s", "<br/>Please contact application owner about this message.<hr/>");
    }
#endif
    // NO CODE HERE OTHER THAN exit(), see LAST PIECE comment above
    exit(0);
}


// 
// Initialize program context. This is called only once for the
// life of the process. pc is program context.
//
void cld_init_config(cld_config *pc)
{
    assert (pc);

    // these are set once and do not need to be reset with each call to vmmain
    pc->trace.f = NULL;
    pc->trace.in_trace = 0;
    pc->trace.in_memory_check = 0;
    pc->debug.sleep = -1;
    pc->debug.lint = 0;
    pc->debug.trace_level = 0;
    pc->debug.memory_check = 0;
    pc->debug.tag = cld_strdup ("");
    pc->ctx.out.was_there_any_output_this_request =0; // must be set for each new request, otherwise
                // we might htink something has been output when nothing was!
    
    reset_cld_config (pc);
}

// 
// Reset program context. This is called for each new web request, or at
// the beginning of command line program.
//
void reset_cld_config(cld_config *pc)
{
    assert (pc);
    // these need to reset with each call to vmmain
    pc->out.buf = NULL;
    pc->out.len = 0;
    pc->out.buf_pos = 0;
    pc->ctx.req = NULL;
    pc->ctx.trim_query_input = 0;
    pc->ctx.cld_report_error_is_in_report = 0;

}


//
// Find number of occurances in string 'str' of a substring 'find'
//
// Returns number of occurances of find in str
//
int cld_count_substring (const char *str, const char *find)
{
    CLD_TRACE("");
    int count = 0;
    int len = strlen (find);
    const char *tmp = str;
    while((tmp = strstr(tmp, find)) != NULL)
    {
       count++;
       tmp += len;
    }
    return count;
}


// 
// Replace string 'find' with string 'subst' in string 'str', which is of size 'strsize' (total size in bytes of buffer 
// that is available). 'all' is 1 if all occurrance of 'find' are to be replaced.
// Output 'last' is the last byte position from which 'find' was searched for, but was not found (i.e.
// last byte position after which there is no more 'find' strings found).
// Returns number of substitutions made, or -1 if not enough memory. If -1, whatever substitutions could have been
// made, were made, in which case use 'last' to know where we stopped.
//
int cld_replace_string (char *str, int strsize, const char *find, const char *subst, int all, char **last)
{
    CLD_TRACE("");
    assert (str);
    assert (find);
    assert (subst);

    int len = strlen (str);
    int lenf = strlen (find);
    int lens = strlen (subst);
    int occ = 0;
    int currsize = len + 1;

    char *curr = str;

    if (last != NULL) *last = NULL;
    while (1)
    {
        // find a string and move memory to kill the found 
        // string and install new one - minimal memmove
        // based on diff
        char *f = strstr (curr, find);
        if (f == NULL) break;
        currsize -= lenf;
        currsize += lens;

        if (currsize > strsize)
        {
            return -1;
        }

        memmove (f + lens, f + lenf, len - (f - str + lenf) + 1);
        memcpy (f, subst, lens);

        // update length
        len = len - lenf + lens;

        curr = f + lens; // next pos to look from 

        if (last != NULL) *last = curr; 
                        // for caller, where to look next, if in
                        // external loop, for 'all==0' only
        occ++;
        if (all == 0) break;
    }
    return occ;
}


//
// Trim string on both left and right and place string back
// into the original buffer. Trims spaces, tabs, newlines.
// str is the string to be cld_trimmed.
// len is the length of string on the input, and it has new length
// on the output. 'len' MUST be the strlen(str0 on input!
//
void cld_trim (char *str, int *len)
{
    CLD_TRACE("");
    assert (str);
    assert (len);
    
    int i = 0;
    // clear leading spaces
    while (isspace (str[i])) i++;
    // move string back, overriding leading spaces
    if (i) memmove (str, str + i, *len - i + 1);
    // update length
    *len = *len -i;
    // start from the end
    i = *len - 1;
    // find the last non-space char
    while (i>=0 && isspace (str[i])) i--;
    // make the end of string there
    str[i + 1] = 0;
    // update length of string
    *len = i + 1;
}

// 
// Returns 1 if 'dir' is a directory,
// 0 if not
//
int cld_is_directory (const char *dir)
{
    CLD_TRACE("");
    struct stat sb;
    if (stat(dir, &sb) == 0 && S_ISDIR(sb.st_mode)) return 1;
    else return 0;
}


//
// Get size of file
// fn is file name.
// Returns size of the file, or -1 if file cannot be stat'
//
size_t cld_get_file_size(const char *fn)
{
    CLD_TRACE("");
    struct stat st;
    if (stat(fn, &st) != 0) return -1;
    return st.st_size;
}


// 
// Checks if input parameter name (in URL) is valid for cloudgizer.
// Valid names are consider to have only alphanumeric characters and
// underscores, and must not start with a digit.
// Returns 1 if name is valid, 0 if not.
//
int cld_is_valid_param_name (const char *name)
{
    CLD_TRACE ("");
    assert (name);

    int i = 0;
    if (!isalpha(name[0])) return 0;
    while (name[i] != 0)
    {
        if (!isalnum(name[i]) && name[i] != '_') return 0;
        i++;
    }
    return 1;
}


// 
// Initialize sequential list storage data
// fdata is storage data variable.
// Data can be stored in order and retrieved in the same order and rewound
// any number of times. Once used, must be purged.
//
void cld_store_init (cld_store_data *fdata)
{
    CLD_TRACE ("");
    assert (fdata != NULL);
    fdata->num_of = 1;
    fdata->store_ptr = 0;
    fdata->retrieve_ptr = 0;
    fdata->item = cld_calloc (fdata->num_of, sizeof (cld_store_data_item));
}

// 
// Store name/value pair, with 'name' being the name and 'data' being the value
// in storage data 'fdata'. Both strings are duplicated and stored in the list.
//
void cld_store (cld_store_data *fdata, const char *name, const char *data)
{
    CLD_TRACE ("");
    assert (fdata != NULL);
    if (fdata->store_ptr >= fdata->num_of)
    {
        fdata->item = cld_realloc (fdata->item, (fdata->num_of+=10) * sizeof (cld_store_data_item));
    }
    fdata->item[fdata->store_ptr].data = (data == NULL ? NULL : cld_strdup (data));
    fdata->item[fdata->store_ptr].name = (name == NULL ? NULL : cld_strdup (name));
    fdata->store_ptr++;
}

// 
// Retrieve name/value pair from storage data 'fdata', with 'name' being the
// name and 'data' being the value. The name/data are simply assigned pointer
// values. Initially, this starts with fist name/value pair put in.
//
void cld_retrieve (cld_store_data *fdata, char **name, char **data)
{
    CLD_TRACE ("");
    assert (fdata != NULL);
    assert (name != NULL);

    if (fdata->retrieve_ptr >= fdata->store_ptr)
    {
        if (data != NULL) *data = NULL;
        if (name!= NULL) *name = NULL;
        return;
    }
    if (data != NULL) *data = fdata->item[fdata->retrieve_ptr].data;
    if (name != NULL) *name = fdata->item[fdata->retrieve_ptr].name;
    fdata->retrieve_ptr++;
}

// 
// Rewind name/value pair in storage 'fdata', so that next cld_retrieve()
// starts from the items first put in.
void cld_rewind (cld_store_data *fdata)
{
    CLD_TRACE ("");
    assert (fdata != NULL);
    fdata->retrieve_ptr = 0;
}


// 
// Purge all data from storage 'fdata' and initialize for another use.
//
void cld_purge (cld_store_data *fdata)
{
    CLD_TRACE ("");
    assert (fdata != NULL);
    fdata->retrieve_ptr = 0;
    while (fdata->retrieve_ptr < fdata->store_ptr)
    {
        if (fdata->item[fdata->retrieve_ptr].data != NULL) 
        {
            cld_free (fdata->item[fdata->retrieve_ptr].data);
        }
        if (fdata->item[fdata->retrieve_ptr].name != NULL) 
        {
            cld_free (fdata->item[fdata->retrieve_ptr].name);
        }
        fdata->retrieve_ptr++;
    }
    if (fdata->item != NULL) cld_free (fdata->item);
    fdata->item = NULL;
    cld_store_init (fdata);
}


// 
// The same as strncpy() except that zero byte is placed at the end.
//
void cld_strncpy(char *dest, const char *src, int max_len)
{
    CLD_TRACE("");
    int len = strlen (src);
    if (len < max_len) 
    {
        memcpy (dest, src, len+1 );
    }
    else
    {
        memcpy (dest, src, max_len-1 );
        dest[max_len - 1] = 0;
    }
}

//
// Initialize an empty string that is allocated on the heap, like malloc
//
inline char *cld_init_string(const char *s)
{
    CLD_TRACE("");
    if (s == NULL) return NULL;
    int l = strlen (s);
    char *res = cld_malloc (l+1);
    memcpy (res, s, l+1);
    return res;
}

// 
// Get timezone that's local to this server.
// Returns string in the format TZ=<timezone>, eg. TZ=MST
//
const char * cld_get_tz ()
{
    //
    // This static usage is okay because the timezone is the SAME for all modules that could
    // run in this process. We can set timezone once for any of the modules, and the rest can
    // use the timezone just fine.
    //
    static int is_tz = 0;
    static char tz[200]; 

    // TZ variable isn't set by default, so we cannot count on it. Functions
    // that operate on time do CHECK if it's set, but unless we set it, it
    // WONT be set
    if (is_tz == 0)
    {
        is_tz = 1;

        // get localtime zone 
        time_t t = time(NULL);
        struct tm *tm = localtime(&t);
        snprintf (tz, sizeof(tz)-1, "TZ=%s", tm->tm_zone);

    }
    return tz;
}

// 
// Read entire file with path 'name' and store file contents in output 'data'.
// Returns -1 if cannot open file, -2 if cannot read, or size of data read.
// Note: zero byte is place after the end, so if return value is 10, it means
// there are 11 bytes, with zero at the end, regardless of whether the data is a
// string or not.
// If there is not enough memory, cld_malloc will error out.
//
int cld_read_whole_file (const char *name, char **data)
{
    CLD_TRACE ("");

    int sz = (int)cld_get_file_size(name);
    FILE *f = fopen (name, "r");
    if (f == NULL)
    {
        return -1;
    }
    *data = cld_malloc (sz + 1);
    if (fread (*data, sz, 1, f) != 1)
    {
        fclose(f);
        return -2;
    }
    (*data)[sz] = 0;
    fclose(f);
    return sz;
}



// 
// Encode string v, producing output result res. enc_type is CLD_WEB (for
// web encoding) or CLD_URL (for url encoding). Pointer to pointer 'res' is allocated
// with sufficient memory in the worst case scenario
// Returns length of an encoded string.
//
int cld_encode (int enc_type, const char *v, char **res)
{
    CLD_TRACE("");
    return cld_encode_base (enc_type, v, strlen(v), res, 1);
}


// 
// Encode string v, producing output result res. enc_type is CLD_WEB (for
// web encoding) or CLD_URL (for url encoding). Pointer to pointer 'res' is allocated
// with sufficient memory in the worst case scenario (if allocate_new is 1), or if it is 0, it MUST
// have enough space to hold CLD_MAX_ENC_BLOWUP(vLen) in it), vLen is the string length of v.
// Returns length of an encoded string.
//
int cld_encode_base (int enc_type, const char *v, int vLen, char **res, int allocate_new)
{
    CLD_TRACE("");
    assert (res != NULL);
    assert (v != NULL);


    if (allocate_new==1)
    {
        *res = (char*)cld_malloc (CLD_MAX_ENC_BLOWUP(vLen)); // worst case, see below for usage
    }
    int i;
    int j = 0;
    if (enc_type == CLD_WEB)
    {
        for (i = 0; i < vLen; i ++)
        {
            switch (v[i])
            {
                case '&': memcpy (*res + j, "&amp;", 5); j+=5; break;
                case '"': memcpy (*res + j, "&quot;", 6); j+=6; break;
                case '\'': memcpy (*res + j, "&apos;", 6); j+=6; break;
                case '<': memcpy (*res + j, "&lt;", 4); j+=4; break;
                case '>': memcpy (*res + j, "&gt;", 4); j+=4; break;
                default: (*res)[j++] = v[i]; break;
            }
        }
    }
    else if (enc_type == CLD_URL)
    {
        for (i = 0; i < vLen; i ++)
        {
            switch (v[i])
            {
                case '%': memcpy (*res + j, "%25", 3); j+=3; break;
                case ' ': memcpy (*res + j, "%20", 3); j+=3; break;
                case '@': memcpy (*res + j, "%40", 3); j+=3; break;
                case '=': memcpy (*res + j, "%3D", 3); j+=3; break;
                case ':': memcpy (*res + j, "%3A", 3); j+=3; break;
                case ';': memcpy (*res + j, "%3B", 3); j+=3; break;
                case '#': memcpy (*res + j, "%23", 3); j+=3; break;
                case '$': memcpy (*res + j, "%24", 3); j+=3; break;
                case '<': memcpy (*res + j, "%3C", 3); j+=3; break;
                case '?': memcpy (*res + j, "%3F", 3); j+=3; break;
                case '&': memcpy (*res + j, "%26", 3); j+=3; break;
                case ',': memcpy (*res + j, "%2C", 3); j+=3; break;
                case '>': memcpy (*res + j, "%3E", 3); j+=3; break;
                case '/': memcpy (*res + j, "%2F", 3); j+=3; break;
                case '"': memcpy (*res + j, "%22", 3); j+=3; break;
                case '+': memcpy (*res + j, "%2B", 3); j+=3; break;
                case '\'': memcpy (*res + j, "%27", 3); j+=3; break;
                default: (*res)[j++] = v[i]; break;
            }
        }
    }
    else 
    {
        assert (1==2);
    }
    (*res)[j] = 0;
    return j;
}

// 
// Write file 'file_name' from data 'content' of length 'content_len'. If 'append' is 1,
// then this is appended to the file, otherwise, file is overwritten (or created if it didn't 
// exist).
// Returns -1 is cannot open file, -2 if cannot write, 1 if okay.
//
int cld_write_file (const char *file_name, const char *content, size_t content_len, int append)
{
    CLD_TRACE("");
    assert(file_name);
    assert(content);
    assert (content[0]!=0);
    FILE *f = fopen (file_name,  append==1 ? "a+" : "w+");
    if (f==NULL) return -1;
    if (content_len==0) content_len=strlen(content);
    if (fwrite(content, content_len, 1, f) != 1)
    {
        fclose(f);
        return -2;
    }
    fclose(f);
    return 1;
}

// 
// Convert integer i to string. 's' is a output parameter (a pointer to string) where string representation
// of integer 'i' will be stored. The memory will be allocated for this. 's' can be NULL, in which case it's
// value will remain the same.
// Returns the allocated string containing the integer as a string.
//
char *cld_i2s (int i, char **s)
{
    CLD_TRACE("");

    int ls = 20;
    char *stemp=(char*)cld_malloc(ls);
    snprintf (stemp, ls - 1, "%d", i);
    if (s!=NULL) *s = stemp;
    return stemp;
}

// 
// Get application home directory which is home directory of currently logged in user plus
// application name. Logged in user is a web server user.
// Returns application home directory or errors out if cannot get it.
// Note: The buffer for home directory is 512 bytes.
//
// IMPORTANT: other code depends on NOT caching this result (i.e. using a flag per module
// to keep home_name calculated once and return other times) - see cld.c for one such usage.
// DO NOT cache home_name without checking out all usage of it.
//
char *cld_home_dir ()
{
    CLD_TRACE("");
    // This static is okay, it is calculated each time this function is called, i.e.
    // it doesn't convey anything beyond a single request.
    static char home_name[512];

    // Typically home directory is obtained once in the beginning and saved somewhere, so 
    // no need cache it
    uid_t uid = geteuid(); 
    struct passwd *pwd = getpwuid(uid); 
    if (pwd == NULL) 
    {
        cld_report_error ("Cannot get home directory, error [%s]\n", strerror(errno));
    }

    snprintf (home_name, sizeof (home_name) , "%s/%s", pwd->pw_dir, cld_handler_name);
    return home_name;
}

//
// Return Cloudgizer version. Cloudgizer does not have minor and patch version (it used to have)
// and now it's major only for simplicity.
//
inline const char * cld_major_version() {return CLD_MAJOR_VERSION;}








