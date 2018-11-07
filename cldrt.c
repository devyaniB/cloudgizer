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
// Main library used at CLD runtime. Most of the functions used
// within markups are implemented here. See also cldrtc.c which includes
// common functions shared between CLD runtime and CLD preprocessor.
//

#include "cld.h"

//  functions (local)
void cld_init_url_response(cld_url_response *s);
size_t cld_write_url_response(void *ptr, size_t size, size_t nmemb, cld_url_response *s);
FILE * cld_create_file_path (char *doc_id, char *path, int path_len);
void cld_init_output_buffer ();
int cld_validate_output ();


// 
// Initialize input_req structure for fetching input URL data
// req is URL structure used to hold input data.
//
void cld_init_input_req (input_req *req)
{
    CLD_TRACE("");
    assert (req);
    int i;
    req->bin_done = 0;
    req->exit_code = 0;
    for (i=0; i < CLD_MAX_NESTED_WRITE_STRING; i++)
    {
        req->write_string_arr[i].string = NULL;
        req->write_string_arr[i].len = 0;
    }
    req->curr_write_to_string = -1; // each write-to-string first increase it
    req->disable_output = 0;
    req->app = NULL;
    req->if_none_match = NULL;
    req->cookies = NULL;
    req->num_of_cookies = 0;
    req->ip.names = NULL;
    req->ip.values = NULL;
    req->ip.num_of_input_params = 0;
    req->sent_header = 0;
    req->url = NULL;
    req->is_shut = 0;
    req->header=NULL; // no custom headers, set to non-NULL for custom headers
}

// 
// Returns length of current top-level write-string (or equivalent API which is
// cld_write_to_string()) string being written.
//
int cld_write_to_string_length ()
{
    CLD_TRACE ("");
    input_req *req = cld_get_config()->ctx.req;
    assert (req->curr_write_to_string < CLD_MAX_NESTED_WRITE_STRING); // overflow if asking within the last level 
                // because the level above it does not exist. We always show the length of previous write-string
                // and that is one level up
    return req->write_string_arr[req->curr_write_to_string+1].len;
}

// 
// Write to string. str is either a CLD-allocated string into which to write
// or NULL, which signifies end of string writing.
// Once non-NULL string str is passed here, all future writing (such as print-noenc
// or print-web etc) goes to this string, until this function is called with NULL.
// Writing to string can be nested, so writing to string2 (while writing to string1)
// will write to string2 until NULL is passed, when it switches back to string1.
//
void cld_write_to_string (char **str)
{
    CLD_TRACE ("");
    input_req *req = cld_get_config()->ctx.req;
    if (str == NULL)
    {
        // stop writing to string
        if (req->curr_write_to_string<0)
        {
            cld_report_error ("Cannot stop writing to string if it was never initiated, or if stopped already");
        }
        if (req->write_string_arr[req->curr_write_to_string].string == NULL)
        {
            cld_report_error ("Previous level of nested writing to string is empty - was it manually emptied?");
        }
        // is_end_write is a signal that cld_flush_printf() can trim the string written. Otherwise, flushing can 
        // happen for any reason at any time and we don't want strings trimmed otherwise - it would produce incorrect strings.
        req->write_string_arr[req->curr_write_to_string].is_end_write = 1;
        cld_flush_printf (0); // finish printing into string before clearing the write-string
        // restore is_end_write to 0 so the next flush doesn't keep trimming (only the flush at THIS juncture
        // should trim! and not any other flush)
        req->write_string_arr[req->curr_write_to_string].is_end_write = 0;
        // no more string to write
        req->write_string_arr[req->curr_write_to_string].string = NULL;
        // Do NOT set req->write_string_arr[req->curr_write_to_string].len = 0 because then function cld_write_to_string_length()
        // couldn't possibly work
        req->curr_write_to_string--;
    }
    else
    {
        req->write_string_arr[req->curr_write_to_string].is_end_write = 0;
        cld_flush_printf (0); // finish outputting to web client so current buffer doesn't end up in the string
            // this must be done prior to increasing curr_write_to_string, otherwise flush will think we're in 
            // the middle of the string writing which hasn't started yet

        // start writing to string
        // Once curr_write_to_string is not -1 (i.e. 0 or more), there is a string writing in progress, even if 
        // nothing has been written to it yet. So the condition for "are we in string writing" is curr_write_to_string!=-1
        req->curr_write_to_string++;
        if (req->curr_write_to_string >= CLD_MAX_NESTED_WRITE_STRING)
        {
            cld_report_error ("Too many nesting levels of writing to string in progress, maximum [%d] nesting levels", CLD_MAX_NESTED_WRITE_STRING);
        }
        if (req->write_string_arr[req->curr_write_to_string].string != NULL)
        {
            req->write_string_arr[req->curr_write_to_string].string = NULL; // so that error reporting stops writing to string
            cld_report_error ("Writing to string in progress, level [%d], was the next-level-of-nesting string manually set?", req->curr_write_to_string);
        }
        //
        // If *str is NULL, we have to have an actual string to write to:
        // So create one if NULL
        if (*str == NULL) *str = CLD_EMPTY_STRING;
        req->write_string_arr[req->curr_write_to_string].string = str;
        req->write_string_arr[req->curr_write_to_string].len = 0; // init where to start writing (from the beginning)
    }
}



// 
// Open trace file and write begin-trace message
// Returns 0 if opened, -1 if not
// Any memory alloc here MUST be malloc since it survives apache mod requests and continues
// over many such requests.
//
int cld_open_trace ()
{
    cld_config *pc = cld_get_config();

    // open trace file, for Apache mod, it will not be NULL (from previous request)
    if (pc->trace.f != NULL)
    {
        fclose (pc->trace.f);
        pc->trace.f = NULL;
    }


    // get time in any case, because we use it for save_HTML()
    // this is done ONLY ONCE per request
    cld_current_time (pc->trace.time, sizeof(pc->trace.time)-1);
    if (pc->debug.trace_level > 0)
    {
        // append if file exists, otherwise open anew
        snprintf(pc->trace.fname, sizeof(pc->trace.fname), "%s/trace-%d-%s", pc->app.log_directory, cld_getpid(), pc->trace.time);
        pc->trace.f = fopen (pc->trace.fname, "a+");
        if (pc->trace.f == NULL) 
        {
            pc->trace.f = fopen (pc->trace.fname, "w+");
            if (pc->trace.f == NULL)
            {
                return -1; 
            }
        }
    }
    return 0;
}

// 
// Close trace file
//
void cld_close_trace()
{
    cld_config *pc = cld_get_config();

    // close trace file
    if (pc->trace.f != NULL)
    {
        fclose (pc->trace.f);
    }
    pc->trace.f = NULL;
    return;
}    



// 
// Run-time construction of SQL statement. 'dest' is the output SQL text, destSize is the size of
// memory available for SQL text in 'dest'. 'num_of_args' is the number of input arguments that follow
// 'format' which is a string that can only contain '%s' as a format placeholder. The input arguments following
// 'format' can be NULL or otherwise (if NULL it's empty string).
// In dynamic queries, the SQL text is a run-time variable, so we don't know the number of '%s' in there ahead of time (i.e. # of inp. params).
// add-query-input is issued at run-time to add parameters. So for example, there could be 1,2, or 3 maybe parameters, or it 
// could be 1 parameter or 4 parameters in such a SQL. However, each time we use add-query-input, we increase the number of 
// parameters. So if there is 1,2 and 3 parameter run-time versions of a query, the number of parameters will be 1+2+3=6. A query with 
// one parameter will have this parameter at index 0 (in list of inp. params), a query with 2 parameters will have input parameters at index 1 and 2 and
// a query with 3 parameters will have input parameters at indexes 3,4 and 5 (assuming this is the order in which they appear in
// source code). So for example, for a 2-parameter query, first input will be NULL, and the last three will be NULL, with
// two parameters in the middle will be the input parameters. So we handle this by simply skipping all NULL parameters and whatever
// remains is the input parameters. So in this case input parameters to this function (after 'format') would be NULL,x,y,NULL,NULL, NULL.
//
// This function makes sure that SQL injection cannot happen by handling single quotes and slashes within strings. Double quotes in the string are  
// allowed because we force ANSI_QUOTES to be set for the session. All input parameters are strings and all are trimmed before being used if trim-query-input 
// is set.
//
// The method is not one of binding but of constructing a full SQL text with data in it, but under sterile conditions not conducive to SQL injection.
//
void cld_make_SQL (char *dest, int destSize, int num_of_args, const char *format, ...) 
{
    CLD_TRACE("");
    assert (dest);
    assert (format);
    
    // Double quotes are allowed in format because ANSI_QUOTES  is set at the session's beginning.
    // So double quotes can NOT be used for string literals, ONLY single quotes.

    int num_of_percents;
    int count_percents;
    count_percents = cld_count_substring (format, "%s");
    num_of_percents = count_percents; // used in a countdown later in this code

    
    // For the format, there has to be an even number of single quotes. An uneven number suggests
    // faulty query. For example, insert ... values 'xyz', 'aaa  - in this case the last string
    // never ends
    int count_single_quote;
    count_single_quote = cld_count_substring (format, "'");
    if (count_single_quote % 2 != 0)
    {
        cld_report_error ("Incorrect number of single quotes, must be an even number, query [%s]", format);
    }

    // Format must be smaller than the destination size to begin with
    int flen = strlen (format);
    if (flen >= destSize - 1)
    {
        cld_report_error ("Destination SQL size too small for format [%s], number of arguments [%d]", format, count_percents);
    }

    memcpy (dest, format, flen + 1);

    char curr_sql_arg[CLD_MAX_SIZE_OF_URL + 1];

    // All %s must be quoted, otherwise in select ... where id=%s, it could be made to be
    // select ... where id=2;drop table x; if input parameters is id=2;drop table x;
    int numPlaceWithQuotes = cld_count_substring (format, "'%s'");
    if (numPlaceWithQuotes != count_percents)
    {
        cld_report_error ("All arguments in SQL statement must be quoted, including numbers, format [%s], number of arguments [%d]", format, count_percents);
    }

    // num_of_args isn't the same as count_percents. num_of_args is the total number of input params, some of which may be NULL
    // count_percentss is the number of '%s' in SQL statement, which must be equal to # of non-NULL input params (among num_of_args params)

    va_list vl;
    va_start (vl, format);

    int i;
    char *curr = dest;
    char *curr_input;
    for (i = 0; i < num_of_args; i++)
    {
        curr_input = va_arg (vl, char *);

        if (curr_input == NULL) // such as with dynamic params if add-query-input is used
        {
            continue;
        }

        num_of_percents--;
        // make sure number of non-NULL params isn't greater than what's expected at run-time
        if (num_of_percents < 0)
        {
            cld_report_error ("Too many non-NULL input parameters in input parameter list for SQL statement [%s], expected [%d] non-NULL run-time arguments", format, count_percents);
        }
        strncpy (curr_sql_arg, curr_input, sizeof (curr_sql_arg) - 1);
        // Escape: single quote and escape backslash in an single quote string
        // Some parameters might not be quoted, and we will catch that as an erro
        // (in db numbers can be quoted)

        if (cld_replace_string (curr_sql_arg, sizeof (curr_sql_arg) - 1, "\\", "\\\\", 1, NULL) == -1)
        {
            va_end (vl);
            cld_report_error ("Argument #%d too large for SQL format [%s], argument [%.100s]", i, format, curr_sql_arg);
        }
        if (cld_replace_string (curr_sql_arg, sizeof (curr_sql_arg) - 1, "'", "''", 1, NULL) == -1)
        {
            va_end (vl);
            cld_report_error ("Argument #%d too large for SQL format [%s], argument [%.100s]", i, format, curr_sql_arg);
        }

        // substitute input parameters, one by one
        // but first trim each value if so by [no-]trim-query-input
        // We worked with a copy of data here, so no need to put anything back (like the zero character at the end we may put)
        char *val = curr_sql_arg;
        int to_trim = cld_get_config()->ctx.trim_query_input;

        if (to_trim==1)
        {
            // trim only if trim-query-input in effect
            // trim left
            while (*val!=0 && isspace(*val)) val++;
            // trim right
            int len_of = strlen (val);
            while (len_of!=0 && isspace(*(val+len_of-1))) len_of--;
            if (len_of!=0)
            {
                *(val+len_of)=0;
            }
        }

        if (cld_replace_string (curr, destSize - 1 - (curr - dest + 1), "%s", val, 0, &curr) == -1)
        {
            va_end (vl);
            cld_report_error ("SQL too large, format [%s], argument [%.100s]", format, curr_sql_arg);
        }
    }

    // make sure number of non-NULL input params isn't lesser than what's expected at run-time
    // num_of_args is # of non-NULL input params in va_arg. # of actual params in va_arg can be different than num_of_percents
    // because va_arg can contain NULLs for dynamic params when add-query-input is used
    if (num_of_percents != 0)
    {
        cld_report_error ("Too few non-NULL input parameters in input parameter list for SQL statement [%s], expected [%d] non-NULL run-time arguments", format, num_of_percents);
    }

    va_end (vl);
    CLD_TRACE ("final statement:[%s]", dest);
}

// 
// Send html header out for a dynamically generated page. It is always no-caching.
// req is input request.
// If HTML output is disabled, NO header is sent.
//
void cld_output_http_header(input_req *req)
{
    CLD_TRACE("");
    assert (req);
    if (req->sent_header == 1) return;
    CLD_TRACE ("sent header: [%d]", req->sent_header);
    if (CTX.req->disable_output == 1) return;
    req->sent_header = 1; // this must be PRIOR to cld_send_header because cld_flush_printf would 
                // complain that header hasn't been sent yet! and cause fatal error at that.
    cld_send_header(req, 0);
}

// 
// Sets cookie that's to be sent out when header is sent. req is input request, cookie_name is the name of the cookie,
// cookie_value is its value, path is the URL for which cookie is valid, expires is the date of exiration.
// SameSite is strict to prevent cross-site request exploitations, for enhanced safety.
// cookies[].is_set_by_program is set to  1 if this is the cookie we changed (i.e. not original in the web input).
//
void cld_set_cookie (input_req *req, const char *cookie_name, const char *cookie_value, const char *path, const char *expires)
{
    CLD_TRACE ("cookie path [%s] expires [%s]", path==NULL ? "NULL":path, expires==NULL ? "NULL":expires);
    assert (req);
    assert (cookie_name);
    assert (cookie_value);

    int ind;
    char *exp = NULL;
    cld_find_cookie (req, cookie_name, &ind, NULL, &exp);
    if (ind == -1)
    {
        if (req->num_of_cookies+1 >= CLD_MAX_COOKIES)
        {
            cld_report_error ("Too many cookies [%d]", req->num_of_cookies+1);
        }
        ind = req->num_of_cookies;
        req->num_of_cookies++;
    }
    else
    {
        cld_free (req->cookies[ind].data);
    }
    char cookie_temp[CLD_MAX_COOKIE_SIZE + 1];
    if (expires == NULL || expires[0] == 0)
    {
        if (path == NULL || path[0] == 0)
        {
            snprintf (cookie_temp, sizeof(cookie_temp), "%s=%s; SameSite=Strict", cookie_name, cookie_value);
            CLD_TRACE("cookie[1] is [%s]", cookie_temp);
        }
        else
        {
            snprintf (cookie_temp, sizeof(cookie_temp), "%s=%s; SameSite=Strict; Path=%s", cookie_name, cookie_value, path);
            CLD_TRACE("cookie[2] is [%s]", cookie_temp);
        }
    }
    else
    {
        if (path == NULL || path[0] == 0)
        {
            snprintf (cookie_temp, sizeof(cookie_temp), "%s=%s; SameSite=Strict; Expires=%s", cookie_name, cookie_value, expires);
            CLD_TRACE("cookie[3] is [%s]", cookie_temp);
        }
        else
        {
            snprintf (cookie_temp, sizeof(cookie_temp), "%s=%s; SameSite=Strict; Path=%s; Expires=%s", cookie_name, cookie_value, path, expires);
            CLD_TRACE("cookie[4] is [%s]", cookie_temp);
        }
    }
    req->cookies[ind].data = cld_strdup (cookie_temp);
    req->cookies[ind].is_set_by_program = 1;
    CLD_TRACE("cookie [%d] is [%s]", ind,req->cookies[ind].data);
}

// 
// Find cookie based on name cookie_name. req is input request. Output: ind is the index in the cookies[] array in
// req, path/exp is path and expiration of the cookie. 
// When searching for a cookie, we search the cookie[] array, which we may have added to or deleted from, so it
// may not be the exact set of cookies from the web input.
// Returns cookie's value.
//
char *cld_find_cookie (input_req *req, const char *cookie_name, int *ind, char **path, char **exp)
{
    CLD_TRACE ("");
    assert (req);
    assert (cookie_name);

    int ci;
    int name_len = strlen (cookie_name);
    for (ci = 0; ci < req->num_of_cookies; ci++)
    {
        CLD_TRACE("Checking cookie [%s] against [%s]", req->cookies[ci].data, cookie_name);
        if (!strncmp (req->cookies[ci].data, cookie_name, name_len) && req->cookies[ci].data[name_len] == '=')
        {
            if (ind != NULL) *ind = ci;
            char *val = req->cookies[ci].data+name_len+1;
            char *semi = strchr (val, ';');
            char *ret = NULL;
            if (semi == NULL)
            {
                ret = cld_strdup (val);
            }
            else
            {
                *semi = 0;
                ret = cld_strdup (val);
                *semi = ';';
            }
            if (path != NULL)
            {
                char *p = strstr (val, "; path=");
                if (p != NULL)
                {
                    semi = strchr (p + 7, ';');
                    if (semi != NULL) *semi = 0;
                    *path = cld_strdup (p + 7);
                    if (semi != NULL) *semi = ';';
                }
                else
                {
                    *path = NULL;
                }
            }
            if (exp != NULL)
            {
                char *p = strstr (val, "; expires=");
                if (p != NULL)
                {
                    semi = strchr (p + 10, ';');
                    if (semi != NULL) *semi = 0;
                    *exp = cld_strdup (p + 10);
                    if (semi != NULL) *semi = ';';
                }
                else
                {
                    *exp = NULL;
                }
            }
            return ret;
        }
    }
    if (ind != NULL) *ind = -1;
    return "";
}

// 
// Delete cookie with name cookie_name. req is input request. "Deleting" means setting value to 'delete' and 
// expiration day to Epoch start. But the cookies is still there and will be sent out in header response, however
// it won't come back since browser will delete it.
// Returns index in cookies[] array of the cookie we just deleted.
// cookies[].is_set_by_program is set to  1 if this is the cookie we deleted (i.e. not original in the web input).
int cld_delete_cookie (input_req *req, char *cookie_name)
{
    CLD_TRACE ("");
    assert (req);
    assert (cookie_name);

    int ci;
    char *path = NULL;
    char *exp = NULL;
    cld_find_cookie (req, cookie_name, &ci, &path, &exp);
    if (ci != -1)
    {
        cld_free (req->cookies[ci].data);
        char del_cookie[300];
        if (path != NULL)
        {
            snprintf (del_cookie, sizeof (del_cookie), "%s=deleted; path=%s; expires=Thu, 01 Jan 1970 00:00:00 GMT", cookie_name, path);
        }
        else
        {
            snprintf (del_cookie, sizeof (del_cookie), "%s=deleted; expires=Thu, 01 Jan 1970 00:00:00 GMT", cookie_name);
        }
        req->cookies[ci].data = cld_strdup (del_cookie);
        req->cookies[ci].is_set_by_program = 1;
        return ci;
    }
    return -1;
}


//
// Send header out. This does it only for web output, i.e. nothing happens for command line program.
// Only changed cookies are sent back (since unchanged ones are already in the browser). Cache is no-cache
// because the html output is ALWAYS generated. Cookies are secure if this is over https connection.
// Charset is always UTF-8.
//
// req can be NULL here, if called from oops page, or req may have very little data in it
// We send header out ONLY:
// 1. if explicitly called via cld_send_header or cld_output_http_header
// 2. it is an error
// 3. we send out a file
// This is important because we do NOT want to send header out just because some text is going
// out - this would break a lot of functionality, such as sending custom headers. 
// HEADER MUST BE EXPLICITLY SENT OUT.
// If 'minimal' is 1, then cookies are not sent. req is input request.
// If req->header is not-NULL (ctype, cache_control, status_id, status_text or control/value), then
// custom headers are sent out. This way any kind of header can be sent.
//
void cld_send_header(input_req *req, int minimal)
{
    CLD_TRACE("");
    assert (req);

#ifdef AMOD
    cld_header *header = req->header;
    const char *secC = "https:";
    cld_config *pc = cld_get_config();
    int secLen = strlen (secC);
    if (header!=NULL  && header->ctype != NULL)
    {
        //
        // Set custom content type if available
        //
        CLD_TRACE("Setting custom content type for HTTP header (%s)", header->ctype);
        cld_ws_set_content_type(pc->ctx.apa, header->ctype);
    }
    else
    {
        cld_ws_set_content_type(pc->ctx.apa, "text/html;charset=utf-8");
    }
    char htemp[300];
    if (minimal == 0 && req != NULL)
    {
        int ci;
        for (ci = 0; ci < req->num_of_cookies; ci++)
        {
            // we send back ONLY cookies set by set-cookie or delete-cookie. Cookies we received and are there
            // but were NOT changed, we do NOT send back because they already exist in the browser. Plus we do NOT
            // keep expired and path, so we would not know to send it back the way it was.
            if (req->cookies[ci].is_set_by_program == 1)
            {
                snprintf (htemp, sizeof(htemp), "%s;HttpOnly;%s", req->cookies[ci].data,
                    !strncasecmp (pc->app.web, secC, secLen) ? "secure":"");
                char *tm;
                CLD_STRDUP(tm, htemp);
                CLD_TRACE("Cookie sent to browser is [%s]", tm);
                cld_ws_add_header (pc->ctx.apa, "Set-Cookie", tm);
            }
        }
    }
    if (header!=NULL  && header->cache_control != NULL)
    {
        //
        // Set custom cache control if available
        //
        CLD_TRACE("Setting custom cache for HTTP header (%s)", header->cache_control);
        cld_ws_set_header (pc->ctx.apa, "Cache-Control", header->cache_control);
    }
    else
    {
        // this is for output from CLD files only! for files, we cache-forever by default
        cld_ws_set_header (pc->ctx.apa, "Cache-Control", "max-age=0, no-cache");
        cld_ws_set_header (pc->ctx.apa, "Pragma", "no-cache");
        CLD_TRACE("Setting no cache for HTTP header (1)");
        // the moment first actual data is sent, this is immediately flushed by apache
    }
    //
    // Set status if available
    //
    if (header!=NULL && header->status_id != 0 && header->status_text != NULL)
    {
        cld_ws_set_status (pc->ctx.apa, header->status_id, header->status_text);
    }
    // 
    // Set any custom headers if available
    //
    if (header!=NULL)
    {
        // add any headers set from the caller
        int i;
        for (i = 0; i<CLD_MAX_HTTP_HEADER; i++)
        {
            if (header->control[i]!=NULL && header->value[i]!=NULL)
            {
                // we use add_header because it allows multiple directives of the same kind
                // set_header allows ONLY ONE instance of a header. We don't know what will go here.
                cld_ws_add_header (pc->ctx.apa, header->control[i], header->value[i]);
            }
            else break;
        }
    }
#else
    CLD_UNUSED (minimal);
#endif
}


// 
// Report an error in program. printf-like function that outputs error to trace file
// (if enabled), and calls oops.v, which usually sends email. Backtrace and web-page-crash
// files are also written. Exit code (for command line) is set to 99.
// Input parameters, web server error code and the actual error message (given by format 
// and subsequent parameters) are written, among other things.
//
void _cld_report_error (const char *format, ...) 
{
    CLD_TRACE("");

    // THIS FUNCTION MUST NOT USE CLD_MALLOC NOR MALLOC
    // as it can be used to report out of memory errors

    // Variables are static, so in case of out of memory
    // we impose less memory requirements, i.e. we already
    // have memory allocated at the beginning of the program
    //
    // NOTE ABOUT static: every Apache request is a NEW one - we do NOT buffer anything
    // because our malloc would clean out everything at the end of each call. This is a bit slower
    // but then any config file changes are immediate, no need to reboot apache.
    // So any static must be clearly with a scope of a SINGLE web call.
    //
    // All static variables in this function are okay because they are used only for the error reporting and do
    // not go across more than one request. 
    //
    // Generally, static variables are okay if they are not initialized, meaning there is
    // no dependency on value across requests (in a single process) or within a processes for any number of requests.
    //

    static char errtext[CLD_MAX_ERR_LEN + 1];
    va_list args;
    va_start (args, format);
    vsnprintf (errtext, sizeof(errtext), format, args);
    va_end (args);
    CLD_TRACE ("Error is %s", errtext);

    cld_config *pc = cld_get_config ();
    if (pc == NULL)
    {
        CLD_FATAL_HANDLER (errtext);
    }

    // We exit right after exiting the current function which rollsback transaction
    // but it's good to be explicit
    cld_check_transaction (2);

    //
    // Make sure tracing is copied over to system buffers, BEFORE proceeding with error handling, 
    // just in case things go bad
    //
    if (pc->trace.f != NULL)
    {
        fflush (pc->trace.f);
    }

    //
    // !!
    // req can be NULL here so must guard for it
    // !!
    //
    input_req *req = pc->ctx.req;
    if (req == NULL)
    {
        CLD_FATAL_HANDLER (errtext);
    }

    // Static variables are fine (for keeping the stack reserved), but
    // ONLY if they do not initialize! If they do, next time around (for 
    // the next request in apache module), they will NOT initialize, and they should.
    static char log_file[300];
    static char time[CLD_TIME_LEN + 1];
    static char email[500 + 1];
    static FILE *fout;
    static char def_err[sizeof (errtext) + 200];
    static char err[20000];
    // End of OK static

    cld_set_exit_code(CLD_ERROR_EXIT_CODE); // set error code when CLD encounters error - to 99

    if (pc->ctx.cld_report_error_is_in_report == 1) 
    {
        CLD_TRACE ("Called cld_report_error more than once, exiting function...");
        // at this point we must not return. cld_report_error MUST be fatal,
        // otherwise lots of code may get executed that never should have,
        // including the code with security concerns!
        exit(0);
    }

    pc->ctx.cld_report_error_is_in_report = 1; // we do not set it back to 0 because
            // cld_report_error is a one time-call for the program (not for apache module! -
            // this is why it is part of context now where it initializes). It still is
            // one time for CGI.


    cld_current_time (time, sizeof(time)-1);
    snprintf (log_file, sizeof (log_file),  "%s/web-page-crash", pc->app.log_directory);

    // send email first, in case it doesn't complete later
    snprintf (email, sizeof(email), "File: [%s], pid: [%d], time: [%s]", log_file, cld_getpid(), time);
    int sm = 0;



    CLD_TRACE ("Error has occurred, trying to open web-page log [%s]", log_file);
    fout = fopen (log_file, "a+");
    if (fout == NULL) 
    {
        fout = fopen (log_file, "w+");
        if (fout == NULL)
        {
            CLD_TRACE ("Cannot open report file, error [%s]", strerror(errno));
            goto repEnd; // do not exit if cannot write, go to oops page
        }
    }
    CLD_TRACE ("Writing to web-page log");
    fseek (fout, 0, SEEK_END);
    long pos = ftell (fout);
    fprintf (fout, "%d: %s: -------- BEGIN WEB PAGE CRASH -------- \n", cld_getpid(), time);
#ifdef AMOD
    int apst = 0;
    const char *apstl = cld_ws_get_status (CTX.apa, &apst);
    fprintf (fout, "%d: %s: Apache status text: %s (status %d)\n", cld_getpid(), time, apstl == NULL ? "":apstl, apst);
#endif
    CLD_TRACE ("Writing PID");
    fprintf (fout, "%d: %s: URL: %s\n", cld_getpid(), time, req->url == NULL ? "<NULL>" : req->url);
    int i;
    if (req != NULL && req->ip.names != NULL && req->ip.values != NULL)
    {
        CLD_TRACE ("Writing input params");
        for (i = 0; i < req->ip.num_of_input_params; i++)
        {
            fprintf (fout, "%d: %s:   Param #%d, [%s]: [%s]\n", cld_getpid(), time, i, 
                req->ip.names[i] == NULL ? "NULL" : req->ip.names[i], 
                req->ip.values[i] == NULL ? "NULL" : req->ip.values[i]);
        }
    }
    CLD_TRACE ("Writing error information");
    fprintf (fout, "%d: %s: ERROR: ***** %s *****\n", cld_getpid(), time, errtext);
    fprintf (fout, "%d: %s: The trace of where the problem occurred:\n", cld_getpid(), time);
    fclose (fout);
    CLD_TRACE ("Getting stack");
    cld_get_stack (log_file);
    CLD_TRACE ("Opening report file");
    fout = fopen (log_file, "a+");
    if (fout == NULL) 
    {
        CLD_TRACE ("Cannot open report file, error [%s]", strerror(errno));
        exit (1);
    }
    fprintf (fout, "%d: %s: -------- END WEB PAGE CRASH -------- \n", cld_getpid(), time);
    fclose (fout);
    CLD_TRACE ("Email status: %d", sm);

    CLD_TRACE ("Pos where log written [%ld], tracelevel [%d]", pos, pc->debug.trace_level);
    if (pos != -1L && pc->debug.trace_level > 0)
    {
        fout = fopen (log_file, "r");
        if (fout != NULL)
        {
            if (fseek (fout, 0, SEEK_END) == 0)
            {
                long pos_of_end = ftell (fout);
                CLD_TRACE ("Pos of end of log [%ld]", pos_of_end);
                if (pos_of_end != -1L)
                {
                    if (fseek (fout, pos, SEEK_SET) == 0)
                    {
                        CLD_TRACE ("Positioned at [%ld]", pos);
                        // 4 times as a maximum for html subst
                        // but since memory is at premium in a tight spot of error reporting
                        // we assume there won't be more than 1/4 chars for substitutions bellow
                        // (i.e. 500 ampersands, less then, greater than and new lines)
                        // if there are, the output may look mangled, but show source should
                        // show it anyways
                        int sz = sizeof(err)-1;
                        long max_to_read = sz * 3 / 4;
                        max_to_read = (max_to_read > (pos_of_end - pos) ? (pos_of_end - pos) : max_to_read);
                        if (err != NULL)
                        {
                            if (fread (err, max_to_read, 1, fout) == 1)
                            {
                                CLD_TRACE ("Read bytes from log [%ld]", pos_of_end - pos);
                                err[max_to_read] = 0;
                                cld_replace_string (err, sz, "&", "&amp;", 1, NULL);
                                cld_replace_string (err, sz, "<", "&lt;", 1, NULL);
                                cld_replace_string (err, sz, ">", "&gt;", 1, NULL);
                                cld_replace_string (err, sz, "\n", "<br/>", 1, NULL);
                            }
                        }
                    }
                }
            }
            fclose (fout);
        }
        else
        {
            CLD_TRACE ("Cannot open report file, error [%s]", strerror(errno));
        }
    }
repEnd:
    CLD_TRACE ("Finishing up error reporting");
    if (err == NULL)
    {
        uid_t uid = geteuid(); 
        struct passwd *pwd = getpwuid(uid); 
        if (pwd == NULL) 
        {
            snprintf (def_err, sizeof (def_err), "Could not produce full error description (couldnot find user effective ID), available error message is:\n[%s]", errtext);
        }
        else
        {
            snprintf (def_err, sizeof (def_err), "Could not produce full error description (because CGI user %s has no privilege to write to directory [%s], or because 'trace' parameter in 'debug' in the same directory is set to 0), available error message is:\n[%s]", pwd->pw_name, pc->app.log_directory, errtext);
        }
    }
    CLD_TRACE ("Calling oops");
    (*(CTX.callback.oops_function))(req, err == NULL ? def_err : err);
    // this (to the end) executes only if out_oops couldn't do it
    CLD_TRACE ("Before shut");
    cld_shut (req);
}



// 
// Decode string encoded previously (web or url encoding). enc_type is CLD_WEB or
// CLD_URL. String v (which is encoded at the entry) holds decoded value on return.
// Return value is the length of decoded string.
//
int cld_decode (int enc_type, char *v)
{
    CLD_TRACE("");
    assert (v != NULL);

    int i;
    int j = 0;
    if (enc_type == CLD_WEB)
    {
        for (i = 0; v[i] != 0; i ++)
        {
            if (v[i] == '&')
            {
                if (!strncmp (v+i+1, "amp;", 4))
                {
                    v[j++] = '&';
                    i += 4;
                }
                else if (!strncmp (v+i+1, "quot;", 5))
                {
                    v[j++] = '"';
                    i += 5;
                }
                else if (!strncmp (v+i+1, "apos;", 5))
                {
                    v[j++] = '\'';
                    i += 5;
                }
                else if (!strncmp (v+i+1, "lt;", 3))
                {
                    v[j++] = '<';
                    i += 3;
                }
                else if (!strncmp (v+i+1, "gt;", 3))
                {
                    v[j++] = '>';
                    i += 3;
                }
                else v[j++] = v[i];
            }
            else
            {
                v[j++] = v[i];
            }
        }
        v[j] = 0;
    }
    else if (enc_type == CLD_URL)
    {
        for (i = 0; v[i] != 0; i ++)
        {
            if (v[i] == '%')
            {
                if (!strncmp (v+i+1, "25", 2))
                {
                    v[j++] = '%';
                    i += 2;
                }
                else if (!strncmp (v+i+1, "20", 2))
                {
                    v[j++] = ' ';
                    i += 2;
                }
                else if (!strncmp (v+i+1, "40", 2))
                {
                    v[j++] = '@';
                    i += 2;
                }
                else if (!strncmp (v+i+1, "3D", 2))
                {
                    v[j++] = '=';
                    i += 2;
                }
                else if (!strncmp (v+i+1, "3A", 2))
                {
                    v[j++] = ':';
                    i += 2;
                }
                else if (!strncmp (v+i+1, "3B", 2))
                {
                    v[j++] = ';';
                    i += 2;
                }
                else if (!strncmp (v+i+1, "23", 2))
                {
                    v[j++] = '#';
                    i += 2;
                }
                else if (!strncmp (v+i+1, "24", 2))
                {
                    v[j++] = '$';
                    i += 2;
                }
                else if (!strncmp (v+i+1, "3C", 2))
                {
                    v[j++] = '<';
                    i += 2;
                }
                else if (!strncmp (v+i+1, "3F", 2))
                {
                    v[j++] = '?';
                    i += 2;
                }
                else if (!strncmp (v+i+1, "26", 2))
                {
                    v[j++] = '&';
                    i += 2;
                }
                else if (!strncmp (v+i+1, "2C", 2))
                {
                    v[j++] = ',';
                    i += 2;
                }
                else if (!strncmp (v+i+1, "3E", 2))
                {
                    v[j++] = '>';
                    i += 2;
                }
                else if (!strncmp (v+i+1, "2F", 2))
                {
                    v[j++] = '/';
                    i += 2;
                }
                else if (!strncmp (v+i+1, "22", 2))
                {
                    v[j++] = '"';
                    i += 2;
                }
                else if (!strncmp (v+i+1, "2B", 2))
                {
                    v[j++] = '+';
                    i += 2;
                }
                else if (!strncmp (v+i+1, "27", 2))
                {
                    v[j++] = '\'';
                    i += 2;
                }
                else v[j++] = v[i];
            }
            else
            {
                v[j++] = v[i];
            }
        }
        v[j] = 0;
    }
    else 
    {
        assert (1==2);
    }
    return j;
}


// 
// Create a file with document id of doc_id (a number as a string), under base path of 'path'
// with string allocated size of path_len. Directories are formed under 'path' by dividing document id by max # of files
// per directory, and each directory starts with 'd', for example <path>/d11. Files under directory start with
// 'f' and are followed by a full number, eliminating file overwrites if we change max # of files per 
// directory. Once file name is created here, it is used in propertyDocument table in the form of a full
// path that's recorded once (so file name is NEVER recalculated again), based on doc_id as a key in 
// propertyDocument table. 
// Returns FILE * to opened file, which is initially empty. If for some reason file existed, it is deleted
// first.
//
FILE *cld_create_file_path (char *doc_id, char *path, int path_len)
{
    CLD_TRACE("");
    cld_config *pc = cld_get_config();

    long dir_id = atol(doc_id) / CLD_MAX_FILES_PER_UPLOAD_DIR;
    const char *dir_pattern = "%s/d%ld";
    const char *file_pattern = "%s/d%ld/f%s";

    // When creating file path, use the minimum file names, meaning 'd' for directory and 'f' for file, to minimize the size of
    // lookup tables and maximize speed. 
    // Another important consideration is changing the CLD_MAX_FILES_PER_UPLOAD_DIR.  Files MUST NOT be overwritten if we change 
    // this, and it may change over time. In our scheme, each file has a unique number (meaning we do not MOD doc_id to get a file number,
    // and we SHOULD NOT USE % (MOD)). So if each file has a unique number, then regardless of how the files are shuffled per directory, they
    // will NEVER overlap. For instance, if we used mod, we could have d0 directory with files f1,f2, f3 (for 1,2,3), d1 with f0,f1,f2 (for 4,5,6) etc. Then if we change mod from
    // 4 to 20, file number 7 would be f7 in d0, but file number 20 would be  f0 in d1, which would OVERWRITE EXISTING FILE!!!!
    // By using this system:
    // 1. we split files into directories (say if 1000 files per directory, directories are d0, d1, d2 for each 1000 files0
    // 2. each file has UNIQUE number so in our above example, files would be in d0:f1,f2,f3 in d1:f4,f5,f6, then switching from 4 to 20 for max_files_per...
    //    we would have f7 in d0 up to f19 in d0, and then in d1 we would have f20. But EXISTING FILES f1-f6 WOULD REMAIN UNTOUCHED!!
    snprintf (path, path_len, file_pattern, pc->app.file_directory, dir_id, doc_id);

    // overwrite files if already exists which should not happen really
    FILE *f = fopen (path, "r");
    if (f != NULL)
    {
        CLD_TRACE("Deleting existing file [%s]", path);
        fclose (f);
        unlink (path);
    }

    CLD_TRACE("Creating file [%s]", path);
    f = fopen (path, "w");
    if (f == NULL)
    {
        snprintf (path, path_len , dir_pattern, pc->app.file_directory, dir_id);
        CLD_TRACE ("Trying to create directory [%s]", path);
        if (mkdir (path, 0700) != 0)
        {
            CLD_TRACE ("mkdir errored with [%s], trying to create a file anyway", strerror (errno));
        }
        snprintf (path, path_len , file_pattern, pc->app.file_directory, dir_id, doc_id);
        CLD_TRACE("Creating file [%s] after dir creation", path);
        f = fopen (path, "w");
    }
    if (f == NULL)
    {
        cld_report_error ("Cannot create file [%s], error [%s]", path, strerror (errno));
    }
    return f;
}

// 
// Get new document id from a table cldDocumentIDGenerator (which must be created prior 
// to using here). doc_id is the output buffer for document id, and the length of this
// buffer is doc_id_len.
//
void cld_get_document_id (char *doc_id, int doc_id_len)
{
    CLD_TRACE("");
    int nrow;
    unsigned int er;
    const char *errm="";

    char insert_sequence[500];
    cld_make_SQL (insert_sequence, sizeof (insert_sequence) - 1, 0, "insert into cldDocumentIDGenerator () values ()");
    if (cld_execute_SQL (insert_sequence, &nrow, &er, &errm) != 1) 
    {
        cld_report_error ("Cannot create file sequence, error [%d], error message [%s]", er, errm);
    }
    if (nrow != 1)
    {
        cld_report_error ("Cannot create file sequence [%d]", nrow);
    }
    cld_get_insert_id(doc_id, doc_id_len);
}

// 
// Get input parameters from web input in the form of
// name/value pairs, meaning from a GET URL or a POST.  
// req is type containing name/value pairs and # of input params,
// method if NULL, environment REQUEST_METHOD is read, otherwise method can be GET or POST -
// this is useful for passing URL from command line.
// If POST, we read from input stream. If GET, we read from input string 
// based on environment variable QUERY_STRING
// Returns 1 if ok, 0 if not ok but handled (such as Forbidden), otherwise Errors out.
// Other information is obtained too such as HTTP_REFERRED that could be used to disallow
// viewing images unless referred to by this site (not a functionality here, it
// can be implemented).
// ETag/If-non-match is obtained for cache management. Cookies are obtained from the client
// ONLY the first time this is called in a request - we may alter cookies later, so if cld_get_input()
// is called again in the same request, cookies are NOT obtained again.
// Input parameters are stored in req variable, where they can be obtained from
// by the program. Files are uploaded automatically and are parameterized in the form of xxx_location, xxx_filename,
// xxx_ext, xxx_size, xxx_id. So if input parameter was myparam, it would be 
// myparam_location, myparam_filename etc. _location is the local file path where file is
// stored. _filename is the actual HTML filename parameter. _ext is extension (with . in 
// front of it, lower cased). _size is the size of the file. _id is the id produced by
// cld_get_document_id() and also id is what _location is based on. If file was empty, then
// all params but _filename are empty.
//
int cld_get_input(input_req *req, const char *method, const char *input)
{
    CLD_TRACE("");
    assert (req);
    req->ip.num_of_input_params = 0;
    req->ip.names = NULL;
    req->ip.values = NULL;

    cld_config *pc = cld_get_config();
    const char *req_method = NULL;
    const char *qry = NULL;
    const char *cont_type = NULL;
    const char *cont_len = NULL;
    int post_len = 0;
    char *content = NULL;
    char *orig_content = NULL;
    const char *cookie = NULL;
    req->ip.num_of_input_params = 0;

    req->sent_header = 0; 
                // for this request, httpd header not sent out yet
                // this is useful when page has inclusions

    // some env vars are obtained right away, other are rarely used
    // and are obtaineable from $$ variables
    CLD_STRDUP (req->referring_url, cld_ctx_getenv ("HTTP_REFERER"));
    CLD_TRACE ("Referer is [%s]", req->referring_url);
    CLD_TRACE ("CLD config base url is [%s], received from browser [%s]", pc->app.web, req->referring_url);
    // when there is a redirection to home page, referring url is empty
    

    int lweb=0;
    const char *col = strchr (pc->app.web, ':');
    if (col != NULL)
    {
        col += 3; // get passed two forward slashes
    }
    else
    {
        col = pc->app.web;
    }
    char *slash = strchr (col, '/');
    if (slash == NULL)
    {
        lweb = strlen (pc->app.web);
    }
    else
    {
        lweb = slash - pc->app.web;
    }
    CLD_TRACE ("lweb is [%d]", lweb);

    if (strncasecmp (pc->app.web, req->referring_url, lweb) && req->referring_url[0] != 0) req->from_here = 0; else req->from_here = 1;

    const char *nm = cld_ctx_getenv ("HTTP_IF_NONE_MATCH");
    if (nm != NULL)
    {
        CLD_STRDUP (req->if_none_match, nm);
        CLD_TRACE("IfNoneMatch received [%s]", nm);
    }

    // this function is often called in "simulation" of a request. ONLY the first request gets cookies
    // from the client (which is HTTP_COOKIE). After this first request, we may alter cookies in memory,
    // and so we do NOT get cookies again from the client.
    if (req->cookies == NULL)
    {
        // make a copy of cookies since we're going to change the string!
        cookie = cld_strdup (cld_ctx_getenv ("HTTP_COOKIE"));
        req->cookies = cld_calloc (CLD_MAX_COOKIES, sizeof (cld_cookies)); // regardless of whether there are cookies in input or not
                                // since we can set them. This also SETS TO ZERO is_set_by_program which is a MUST, so HAVE to use cld_calloc.
        if (cookie != NULL && cookie[0] != 0)
        {
            CLD_TRACE ("Cookie [%s]", cookie);
            int tot_cookies = 0;
            while (1)
            {
                if (tot_cookies >= CLD_MAX_COOKIES) cld_report_error("Too many cookies [%d]", tot_cookies);
                char *ew = strchr (cookie, ';');
                if (ew != NULL)
                {
                    *ew = 0;
                    ew++;
                    while (isspace(*ew)) ew++;
                }
                req->cookies[tot_cookies].data = cld_strdup (cookie);
                CLD_TRACE("Cookie [%s]",req->cookies[tot_cookies].data);
                tot_cookies++;
                if (ew == NULL) break;
                cookie = ew;
            }
            req->num_of_cookies = tot_cookies;
        }
        else 
        {
            req->num_of_cookies = 0;
        }
    }

    // request method, GET or POST
    // method, input override environment
    if (method != NULL)
    {
        req_method = method;
    }
    else
    {
        req_method = cld_ctx_getenv ("REQUEST_METHOD");
    }
   
    if (req_method == NULL) 
    {
        cld_report_error ("REQUEST_METHOD environment variable is not found");
    }
    int text_len = 0; // length just for text inputs

    int is_multipart = 0;

    CLD_TRACE ("Request Method: %s", req_method);
    if (!strcasecmp(req_method, "GET"))
    {
        // for GET, get input data
        // if method,input used, then we only take input for GET, not POST
        // because we convert any POST to GET in the main() code and any files
        // are already saved as actual files
        if (method != NULL)
        {
            qry = input;
        }
        else
        {
            qry = cld_ctx_getenv ("QUERY_STRING");
        }
        if (qry == NULL)
        {
            content = (char*)cld_calloc (text_len = 2, sizeof(char));
        }
        else
        {
            content = (char*)cld_calloc (text_len = (strlen (qry) + 2), sizeof (char));
            strcpy (content, qry);
        }
    }
    else if (!strcasecmp(req_method, "POST"))
    {
#ifndef AMOD
        cld_report_error ("Cannot use POST unless within a web server");
        exit(0);
#endif
        // handle POST request, first check content type
        cont_type = cld_ctx_getenv ("CONTENT_TYPE");
        const char *mult = "multipart/form-data;";
        char *mform = NULL;
        if ((mform = strcasestr (cont_type, mult)) != NULL)
        {
            if (mform == cont_type || *(mform - 1) == ';' || isspace (*(mform - 1)))
            {
                is_multipart = 1;
            }
        }
        if (cont_type != NULL && (!strcasecmp (cont_type, 
            "application/x-www-form-urlencoded") || is_multipart == 1))
        {
            // size of input data
            cont_len = cld_ctx_getenv ("CONTENT_LENGTH");
            if (cont_len == NULL)
            {
                cld_report_error ("Missing content length");
            }
            post_len = atoi (cont_len);
            if (post_len == 0)
            {
                cld_report_error ("Content length is zero");
            }
            if (is_multipart)
            {
                if (post_len >= pc->app.max_upload_size)
                {
                    // when uploading, if total size too large, display error page
                    (*(pc->ctx.callback.file_too_large_function)) (req, (int)(pc->app.max_upload_size/(1024*1024)));
                }
            }
            else
            {
                if (post_len >= CLD_MAX_SIZE_OF_URL)
                {
                    cld_report_error ("Web input larger than the limit of [%d] bytes (1)", CLD_MAX_SIZE_OF_URL);
                }
            }
            content = (char*)cld_malloc (text_len = (post_len + 2));
            // get input data
#ifdef AMOD
            if (cld_ws_util_read (pc->ctx.apa, content, post_len) != 1)
            {
                cld_report_error ("Error reading input data from POST");
            }
            int apst = 0;
            const char *apstl = cld_ws_get_status (pc->ctx.apa, &apst);
            CLD_UNUSED(apstl);
            if (apst == 413)
            {
                cld_ws_set_status (pc->ctx.apa, 200, "200 OK");
                (*(pc->ctx.callback.file_too_large_function)) (req, (int)(pc->app.max_upload_size/(1024*1024)));
                return 0;
            }
#endif
            content [post_len] = content[post_len+1] = 0;

        }
        else
        {
            cld_forbidden ("Unsupported page type", cont_type == NULL ? "NULL" : cont_type);
            return 0;
        }
    }
    else 
    {
       cld_forbidden ("Unsupported request method", req_method);
       return 0;
    }

    if (is_multipart == 1)
    {
        char *new_cont = (char*)cld_malloc (CLD_MAX_SIZE_OF_URL + 1);
        int new_cont_ptr = 0;

        // Based on RVM2045 (MIME types) and RVM1867 (file upload in html form)
        // Boundary is always CRLF (\r\n) and for 'multipart' type, the content-transfer-encoding must
        // always be 7bit/8bit/binary, i.e. no base64
        const char *boundary_start = "boundary=";
        char *bnd = strcasestr (cont_type, boundary_start);
        if (bnd == NULL)
        {
            cld_report_error ("Cannot find boundary in content type header [%s]",cont_type);
        }
        if (bnd != cont_type && !isspace(*(bnd - 1)) && *(bnd - 1) != ';') 
        {
            cld_report_error ("Cannot find boundary in content type header [%s]",cont_type);
        }
        char *b = bnd + strlen (boundary_start); // b is now boundary string up new line or ;
        int boundary_end =  strcspn (b, "\n;");
        b[boundary_end] = 0; // b is now boundary string
        int boundary_len = strlen (b);
        cld_trim (b, &boundary_len);
        // look for multi-part elements, one by one
        char *c = content; // start with the beginning of content
        char *past_end = content + post_len;
        int remainder_len;
        while (1)
        {
            char *file_name = NULL;
            char *name = NULL;
            char *name_val = NULL;
            char *cont_type = NULL;
            int cont_type_len = 0;

            remainder_len = past_end - c; // calculate how much we advanced in the input data

            char *el = memmem(c, remainder_len, b, boundary_len);// look for boundary in content
            //char *el = strstr (c, b); 

            // boundary is always preceded by -- but we don't look for that
            // if boundary is superceded by -- it's the last one
            if (el == NULL)  break;
            el += boundary_len;
            if (*(el + 1) == '-' && *(el + 2) == '-') break;
            const char *c1 = "Content-Disposition:";
            char *prev = el;
            el = strcasestr (el, c1); // pos of 'content-disposition'
            if (el == NULL) break;
            if (el != prev && !isspace(*(el - 1)) && *(el - 1) != ';') break;
            char *end_of_element = strchr (el, '\n');
            if (end_of_element != NULL) *end_of_element = 0; // mark the end of content-disp. line 
                                        // so we don't go beyond it
            el += strlen (c1);

            char *beg_of_line = el; // now we're past content-disposition
            // Find name
            const char *c2 = "name=";
            prev = el;
            el = strcasestr (el, c2); // pos of name=
            if (el == NULL) break;
            if (el != prev && !isspace (*(el - 1)) &&  *(el - 1) != ';') break;
            el += strlen (c2);
            char *element_end = el + strcspn (el, ";"); // end of name=
            char char_end = *element_end;
            *element_end = 0;
            name = el;
            int name_len = strlen (name);
            cld_trim (name, &name_len);
            CLD_STRDUP (name, name);
            if (name[0] == '"') 
            { 
                name[name_len - 1] = 0; 
                name++;
            }
            *element_end = char_end; // restore char

            // find file name, this one is optional
            const char *c3 = "filename=";
            el = beg_of_line; // look for file name in the line again
            prev = el;
            el = strcasestr (el, c3); // pos of filename=
            if (el != NULL) 
            {
                if (prev != el && !isspace (*(el - 1)) && *(el - 1) != ';') break;
                el += strlen (c3);
                // we use strcspn since there may not be ; but rather the string just ends
                char *element_end = el + strcspn (el, ";"); // end of filename=
                char char_end = *element_end;
                *element_end = 0;
                file_name = el;
                int filename_len = strlen (file_name);
                cld_trim (file_name, &filename_len);
                CLD_STRDUP (file_name, file_name);
                if (file_name[0] == '"') 
                { 
                    file_name[filename_len - 1] = 0;
                    file_name++;
                }
                *element_end = char_end;
            }
            else 
            {
                CLD_STRDUP (file_name, "");
            }

            // after looking up name/filename, go to next line. If empty, the following
            // line is value. If not it could be Content-Type or something else
            char *cval = end_of_element + 1;
            char *end_of_val = strchr (cval, '\n');
            if (end_of_val == NULL) break;
            *end_of_val = 0;
            int len_cont_val = strlen (cval);
            cld_trim (cval, &len_cont_val);
            if (cval[0] == 0)
            {
                // the following line is the actual value of parameter [name], name_val is value
                name_val = end_of_val + 1;
                // now find the boundary
                char *end_name = memmem(name_val, past_end - name_val, b, boundary_len);// look for boundary in content
                if (end_name == NULL) break;
                *(end_name - 4) = 0; // account for CRLF prior to boundary and for '--'
                c = end_name - 3; // continue right after the end, in the middle of CRLF
                int len_of_name_val = (end_name - 4 - name_val);
                // name_val is the value
                // len_of_name_val is the length
                cld_trim (name_val, &len_of_name_val);
                CLD_STRDUP (name_val, name_val);
            }
            else
            {
                // Now, we should find Content-Type or something else
                // We ignore all that is here since we will always just
                // save the file as binary, whatever it is. It is ip to application
                // to figure out
                // this is file, and we should have filename
                // but not fatal if we do not
                CLD_STRDUP (name_val, "");
                cont_type = end_of_val + 1;
                char *nlBeforeCont = strchr (cont_type, '\n');
                if (nlBeforeCont == NULL) break;
                cont_type = nlBeforeCont + 1;
                // now find the boundary
                char *end_file = memmem(cont_type, past_end - cont_type, b, boundary_len);// look for boundary in content
                if (end_file == NULL) break;
                *(end_file - 4) = 0; // account for CRLF prior to boundary and for '--'
                c = end_file - 3; // continue right after the end, in the middle of CRLF
                cont_type_len = (end_file - 4 - cont_type);
                // cont_type is now the file
                // cont_type_len is the binary length
            }
            int avail;
            char *enc;
            enc = NULL;
            // name of attachment input parameter
            cld_encode (CLD_URL, name_val, &enc);
            int would_write = snprintf (new_cont + new_cont_ptr, avail = CLD_MAX_SIZE_OF_URL - new_cont_ptr - 2, "%s=%s&", name, enc);
            cld_free (enc);
            if (would_write >= avail)
            {
                cld_report_error ("Web input larger than the limit of [%d] bytes (2)", CLD_MAX_SIZE_OF_URL);
            }
            new_cont_ptr += would_write;
            if (file_name[0] != 0)
            {
                // provide original (client) file name
                enc = NULL;
                cld_encode (CLD_URL, file_name, &enc);
                int would_write = snprintf (new_cont + new_cont_ptr, avail = CLD_MAX_SIZE_OF_URL - new_cont_ptr - 2, "%s_filename=%s&", name, enc);
                cld_free (enc);
                if (would_write  >= avail)
                {
                    cld_report_error ("Web input larger than the limit of [%d] bytes (3)", CLD_MAX_SIZE_OF_URL);
                }
                new_cont_ptr += would_write;
            }
            // write file
            // generate unique number for file and directory, create dir if it doesnt exist
            // as well as file. We can only tell that file name was not uploaded if file name is
            // empty. All files appear 'uploaded'. If a file was not specified, it still exists
            // in the content, it's just empty. The only way to tell it was not uploaded is 
            // by empty file_name.

            if (cont_type != NULL)
            {
                if (file_name[0] != 0)
                {
                    // get extension of filename
                    int flen = strlen (file_name);
                    int j = flen - 1;
                    char *ext = "";
                    while (j > 0 && file_name[j] != '.') j--;
                    if (file_name[j] == '.')
                    {
                        CLD_STRDUP (ext, file_name + j); // .something extension captured
                        if (!strcasecmp (ext, ".jpeg")) cld_copy_data (&ext, ".jpg");
                        if (!strcasecmp (ext, ".jpg")) cld_copy_data (&ext, ".jpg");
                        if (!strcasecmp (ext, ".pdf")) cld_copy_data (&ext, ".pdf");
                    }
                    //lower case extension
                    flen = strlen (ext);
                    for (j = 0; j < flen; j++) ext[j] = tolower (ext[j]);

                    char *doc_id = NULL;
                    char write_dir[1024 + 1];
                    FILE *f = cld_make_document (&doc_id, write_dir, sizeof (write_dir)-1);

                    // write the actual uploaded file contents to local file
                    if (fwrite(cont_type, cont_type_len, 1, f) != 1)
                    {
                        cld_report_error ("Cannot write file [%s], error [%s]", write_dir, strerror (errno));
                    }
                    fclose (f);

                    // provide location where file is actually stored on server
                    enc = NULL;
                    cld_encode (CLD_URL, write_dir, &enc);
                    int would_write = snprintf (new_cont + new_cont_ptr, avail = CLD_MAX_SIZE_OF_URL - new_cont_ptr - 2, "%s_location=%s&", name, enc);
                    cld_free (enc);
                    if (would_write  >= avail)
                    {
                        cld_report_error ("Web input larger than the limit of [%d] bytes (4)", CLD_MAX_SIZE_OF_URL);
                    }
                    new_cont_ptr += would_write;

                    // provide extension of the file
                    would_write = snprintf (new_cont + new_cont_ptr, avail = CLD_MAX_SIZE_OF_URL - new_cont_ptr - 2, "%s_ext=%s&", name, ext);
                    if (would_write  >= avail)
                    {
                        cld_report_error ("Web input larger than the limit of [%d] bytes (5)", CLD_MAX_SIZE_OF_URL);
                    }
                    new_cont_ptr += would_write;

                    // provide size in bytes of the file
                    would_write = snprintf (new_cont + new_cont_ptr, avail = CLD_MAX_SIZE_OF_URL - new_cont_ptr - 2, "%s_size=%d&", name, cont_type_len);
                    if (would_write  >= avail)
                    {
                        cld_report_error ("Web input larger than the limit of [%d] bytes (6)", CLD_MAX_SIZE_OF_URL);
                    }
                    new_cont_ptr += would_write;

                    // provide id of file
                    would_write = snprintf (new_cont + new_cont_ptr, avail = CLD_MAX_SIZE_OF_URL - new_cont_ptr - 2, "%s_id=%s&", name, doc_id);
                    if (would_write  >= avail)
                    {
                        cld_report_error ("Web input larger than the limit of [%d] bytes (7)", CLD_MAX_SIZE_OF_URL);
                    }
                    new_cont_ptr += would_write;


                }
                else
                {
                    // no file uploaded, just empty filename as an indicator
                    int would_write = snprintf (new_cont + new_cont_ptr, avail = CLD_MAX_SIZE_OF_URL - new_cont_ptr - 2, "%s_filename=&", name);
                    if (would_write >= avail)
                    {
                        cld_report_error ("Web input larger than the limit of [%d] bytes (8)", CLD_MAX_SIZE_OF_URL);
                    }
                    new_cont_ptr += would_write;
                }
            }

        }
        if (new_cont_ptr > 0) new_cont[new_cont_ptr - 1] = 0; // the extra '&' which is always appended
        // Now the URL is actually built from the POST multipart request that can be parsed as an actual request
        content = new_cont; // have URL (built) and pass it along as if it were a regular URL POST
    }

    orig_content = cld_strdup (content);


    // Convert URL format to a number of zero-delimited chunks
    // in form of name-value-name-value...
    int j;
    int i;
    int had_equal = 0;
    for (j = i = 0; content[i]; i++)
    {
        content[i] = (content[i] == '+' ? ' ' : content[i]);
        if (content[i] == '%')
        {
            content[j++] = CLD_CHAR_FROM_HEX (content[i+1])*16+
                CLD_CHAR_FROM_HEX (content[i+2]);
            i += 2;
        }
        else
        {
            if (content[i] == '&')
            {
                if (had_equal == 0)
                {
                    cld_report_error ("Malformed URL request [%s], encountered ampersand without prior name=value", orig_content);
                }
                content[j++] = 0;
                had_equal = 0;
            }
            else if (content[i] == '=')
            {
                had_equal = 1;
                (req->ip.num_of_input_params)++;
                content[j++] = 0;
            }
            else
                content[j++] = content[i];
        }
    }
    content[j++] = 0;
    content[j] = 0;


    req->ip.names = (const char**)cld_calloc (req->ip.num_of_input_params, sizeof (char*));
    req->ip.values = (char**)cld_calloc (req->ip.num_of_input_params, sizeof (char*));

    j = 0;
    int name_length;
    int value_len;
    for (i = 0; i < req->ip.num_of_input_params; i++)
    {
        name_length = strlen (content + j); 
        (req->ip.names)[i] = content +j;
        if (cld_is_valid_param_name (req->ip.names[i]) != 1)
        {
            cld_report_error ("Invalid input parameter name [%s], can contain alphanumeric characters or underscores only", req->ip.names[i]);
        }

        j += name_length+1;
        value_len = strlen (content + j); 
        (req->ip.values)[i] = cld_malloc (value_len + 1);
        memcpy ((req->ip.values)[i], content +j, value_len + 1);
        int trimmed_len = value_len;
        cld_trim ((req->ip.values)[i], &trimmed_len);// trim the input parameter for whitespaces (both left and right)
        j += value_len+1;

        if (pc->debug.trace_level > 0)
        {
            int k;
            for (k = 0; k < i; k++)
            {
                if (!strcmp (req->ip.names[k], req->ip.names[i]))
                {
                    cld_report_error ("Input parameter [%s] is specified more than once in URL input", req->ip.names[i]);
                }
            }
        }
        CLD_TRACE ("Index: %d, Name: %s, Value: %s", i, (req->ip.names)[i], (req->ip.values)[i]);
    }
    // do not free content, names are used from it 
    req->url = cld_strdup(orig_content);
    req->len_URL = text_len;


    CLD_TRACE ("URL input [%s]", orig_content);
    cld_free (orig_content);

    return 1;
}

// 
// In URL list of inputs, find an index for an input with a given name
// req is input request. 'name' is the name of input parameters. Search is
// case sensitive.
// Returns value of parameters, or "" if not found.
//
char *cld_get_input_param (const input_req *req, const char *name)
{
    CLD_TRACE("");
    assert (req);
    assert (name);

    int i;
    CLD_TRACE ("Number of input data [%d], looking for [%s]", req->ip.num_of_input_params, name);
    for (i = 0; i < req->ip.num_of_input_params; i++)
    {
        if (!strcmp (req->ip.names[i], name))
        {
            CLD_TRACE ("Found input [%s] at [%d]", req->ip.values[i], i);
            return req->ip.values[i];
        }
    }
    CLD_TRACE ("Did not find input");
    return cld_calloc (1, sizeof(char)); // i.e. dynamic ""
}

// 
// Append string 'from' to string 'to'.
// 'to' will be a new pointer to allocated data that contains to+from
//
inline void cld_append_string (const char *from, char **to)
{
    CLD_TRACE("");
    assert (from);
    assert (to);
    assert (*to);
    int off = strlen (*to);
    cld_copy_data_at_offset (to, off, from);
}


// 
// Copy 'value' to 'data' at offset 'off'. 
// Returns number of bytes written excluding zero at the end.
// 'data' will be a pointer to allocated data that has *data+(value at offset off of data)
// This is a base function used in other string manipulation routines.
//
inline int cld_copy_data_at_offset (char **data, int off, const char *value)
{
    CLD_TRACE ("");
    assert (data != NULL);

    if (*data == NULL) 
    {
        assert (off==0);
        CLD_STRDUP (*data, value);
        return 0;
    }
    else
    {
        if (*data == value) 
        {
            assert (off==0);
            return 0; // copying to itself, with SIGSEGV
        }
        if (value == NULL) value = "";
        int len_val = strlen (value);
        // cld_realloc and cld_free use cld_check_memory() which checks the following:
        // cld_check_memory() will check if data is valid memory allocated with cld_*alloc* functions
        // If not, it will SIGSEG or assert here. What it checks is 1) magic numbers under the buffer
        // 2) magic number above the buffer 3) pointer address in memory table 4) size.
        // In short, it is highly unlikely any pointer that's not a product of cld_ functions to
        // pass muster here.
        // This was done to increase confidence that the memory we're handling is valid. If you need
        // to assure you have valid memory, you can use cld_check_memory()
        //
        // cld_realloc will handle if *data points to CLD_EMPTY_STRING, i.e. if it's uninitialized
        //
        *data = cld_realloc (*data, off+len_val + 1);
        memcpy (*data+off, value, len_val+1);
        return len_val; // returns bytes written, not the new length
    }
}


// 
// Copy string from 'value' to 'data', with 'data' being the output pointer.
// Returns the number of bytes written excluding zero at the end.
//
inline int cld_copy_data (char **data, const char *value)
{
    CLD_TRACE ("");
    return cld_copy_data_at_offset(data, 0, value);
}


// 
// Check if string 's' is a number. Return 1 if it is, 0 if not.
// Number can have plus/minus in front and can have one dot somewhere
// in the middle. Outputs: 'prec' is precision: total number of digits, 'scale' is the number of 
// digits after the decimal point. If prec and scale aren't NULL, they are filled.
// Same for 'positive', if number is positive it is 1, otherwise 0.
//
int cld_is_number (const char *s, int *prec, int *scale, int *positive)
{
    CLD_TRACE("");
    assert (s);
    int i = 0;
    if (prec != NULL ) *prec = 0;
    if (scale != NULL) *scale = 0;
    int dot_pos = 0;
    int sign = 0;
    if (positive!=NULL) *positive=1;
    while (s[i] != 0) 
    {
        if (isspace(s[i])) 
        {
            i++;
            continue;
        }
        if (!isdigit(s[i]))
        {
           if (s[i]=='+' || s[i]=='-')
           {
               if (i != 0)
               {
                    // + or - isn't the first
                    return 0;
               }
               else
               {
                   sign = 1;
                   if (s[i]=='-' && positive!=NULL) *positive = 0;
               }
           }
           else if (s[i]=='.' && i>0)
           {
               if (dot_pos > 0)
               {
                   // two dots
                   return 0;
               }
              dot_pos = i; 
           }
           else
           {
               return 0;
           }
        }
        i++;
    }
    if (dot_pos > 0)
    {
        int c_scale= i - dot_pos - 1;
        if (c_scale == 0)
        {
            // this is for example 1234. 
            // i.e. no digits after dot
            return 0;
        }
        if (scale != NULL) *scale = c_scale;
    }
    else
    {
        if (scale != NULL) *scale = 0;
    }
    if (dot_pos > 0) i--;
    if (sign > 0) i--;
    // for example in -123.4, c_prec would be be 4 because 6th byte would be zero and we decrease i
    // after the loop: If there is a single dot, we decrease it by 1. If there is a single +
    // or -, we decrease it by 1. The result is the precision.
    if (prec != NULL) *prec = i;
    if (i == 0) return 0; // no digits, not a number
    return 1;
}

// 
// Returns 1 if string 's' is a positive unsigned integer.
//
int cld_is_positive_int (const char *s)
{
    CLD_TRACE("");
    assert (s);
    int i = 0;
    while (s[i] != 0) 
    {
        if (!isdigit(s[i]))
        {
           return 0;
        }
        i++;
    }
    return 1;
}


// 
// Execute a program
// This function executes a program 'cmd' (which must be a full path) and:
// 1. takes input arguments 'argv' with total number of them 'num_args'.
//    (argv[num_args] must be NULL, so if arguments are 'x' and 'y', then argv[0] is 'x', argv[1] is 'y' and 
//     argv[2] is NULL and num_args is 2)
// 2. input string 'inp' of length 'inp_len' is passed as stdin to the program. If inp is NULL or inp[0]==0,
//      then, do NOT write to stdin of a program being executed.
// 3. output of the program (both stdout and stderr) is saved in 'out_buf' of length 'out_len' (the rest is discarded)
// Return value is the exit status.
//
int cld_exec_program_with_input(const char *cmd, const char *argv[], int num_args, const char *inp, int inp_len, char *out_buf, int out_len)
{
    CLD_TRACE("");
    assert (inp);
    assert (cmd);
    assert (argv);
    assert (out_len >0);

    if (argv[num_args] != NULL)
    {
        cld_report_error ("Number of arguments does not match last NULL");
    }

    pid_t pid;
    int pf[2];
    // pf[0] is read, pf[1] is write of pipe
    // when fork happens, read of parent is write of child and vice versa
    if (pipe(pf) == -1) 
    {
        cld_report_error ("Cannot create pipes, error [%s]", strerror(errno));
    }

    // Here [] and not char *!!!! Because mkstemp will modify it!
    char templ[] = "/tmp/vmXXXXXX";

    // mkstemp creates secure file (0600 permissions) on all glibc versions of 2.07 and later
    // which Centos 7 and above should be in any case. Thus use of this function does NOT 
    // present security risk if used with such glibc versions.
    int ofd = mkstemp (templ);
    if (ofd == -1)
    {
        cld_report_error ("Cannot open temporary file [%s], error [%s]", templ, strerror (errno));
    }
    unlink (templ); // unlink it, so when closed, it's gone
    
    // note dup2() cannot get EINTR (interrrupt) on LINUX, so no checking
    // dup2(a,b) means that b is copy of a, and both can be used to read or write. So in the context of what we do:
    // 1. if there is something written to b, it will be redirected to a too. b is no longer relevant if we don't care.
    // 2. if there is something to read from a , it will be read from b. a is no longer relevant if we don't care.
    pid = fork();
    if (pid == -1) 
    { 
        cld_report_error ("Cannot create child, error [%s]", strerror(errno));
    }
    else if (pid == 0) 
    {
        // child
        dup2(ofd,STDERR_FILENO); // make any output go to temp file
        dup2(ofd,STDOUT_FILENO); // including stdout and stderr

        close(pf[1]);    // close write-end of pipe
        dup2(pf[0],STDIN_FILENO);  // duplicate pipe-in to stdin.
        close(pf[0]);   // close pipe-in (duplicate)
        execv(cmd, (char *const*)argv); // will get data from parent
        _exit(1);  // failed to exec, do not flush stdout twice (if exit()
            // were called, it would call atexit() of parent and flush its stdout
            // and html output to apache would be duplicated!
    }
    else 
    { 
        // parent
        close (pf[0]); // close read-end of pipe, and just write

        // if there is no input, do NOT send anything to stdin of executing program
        if (inp!=NULL && inp[0]!=0)
        {
            FILE *fr = fdopen (pf[1], "w");
            if (fr == NULL)
            {
                cld_report_error ("Cannot open write pipe, error [%s]", strerror(errno));
            }
            CLD_TRACE("Program input [%.*s]", inp_len, inp);
            if (fwrite(inp, inp_len, 1, fr) != 1) 
            {
                cld_report_error ("Cannot provide input data [%s] to program [%s], error [%s]", inp, cmd, strerror (errno));
            }
            fclose (fr);
        }

        // close pipe to stdin, regardless of whether we wrote something or not
        close(pf[1]);

        int st;
        while (wait (&st) != pid) ; // wait until child finishes


        lseek (ofd, SEEK_SET, 0);
        int rd = read (ofd, out_buf, out_len - 1);
        if (rd >= 0)
        {
            out_buf[rd] = 0;
        }
        close (ofd); // close output in parent

        return st;
    }
    return 1; // will never actually reach here, only for compiler joy

}

// 
// Get debugging options, such as tracing and linting
// These options are stored in a 'debug' file in 'trace' directory
// These options are obtained for each request.
// Double slash is a comment in debug file
//
void cld_get_debug_options()
{
    CLD_TRACE(""); // this will not show up until tracing has been established
                // only the calls after will. Tracing calls debugOptions, and debugOptions
                // calls tracing, leading to recursion, except that we check for this

    FILE *f;
    char trace_file[200];
    cld_config *pc = cld_get_config();

    snprintf (trace_file, sizeof (trace_file), "%s/%s", pc->app.log_directory, CLD_DEBUGFILE);
    CLD_TRACE("Checking debug file [%s]", trace_file);

    f = fopen (trace_file, "r");
    if (f == NULL) return; // cannot open, may have privileges incorrect,
                    // we just silently ignore it and use default options

    char line[200];
    while (1)
    {
        if (fgets (line, sizeof (line) - 1, f) != NULL)
        {
            int len = strlen (line);
            cld_trim (line, &len);
            if (line[0] == '/' && line[1] == '/') continue; //comment line

            char *eq = strchr (line, '=');
            if (eq == NULL) continue; // bad line or empty line

            // divide line into name, value
            *eq = 0;
            len = strlen (line);
            cld_trim (line, &len); // name

            len = strlen (eq + 1);
            cld_trim (eq + 1, &len); // value
            
            // now line is the option, eq+1 is the value
            if (!strcasecmp (line, "LINT"))
            {
                if (!strcasecmp (eq + 1, "1"))
                {
                    pc->debug.lint  = 1;
                }
            }
            else if (!strcasecmp (line, "SLEEP"))
            {
                pc->debug.sleep = atoi(eq+1);
            }
            else if (!strcasecmp (line, "TRACE"))
            {
                pc->debug.trace_level = atoi(eq+1);
            }
            else if (!strcasecmp (line, "MEMORYCHECK"))
            {
                pc->debug.memory_check = atoi(eq+1);
            }
            else if (!strcasecmp (line, "TAG"))
            {
                pc->debug.tag = cld_strdup (eq+1);
                int tag_len = strlen (pc->debug.tag);
                cld_trim (pc->debug.tag, &tag_len);
            }
        }
        else break; // either eof or error, either way
                // we won't handle or report, because there is no 
                // venue yet to report it in, we're opening those now
    }

    CLD_TRACE("Debug: lint:[%d], tracing:[%d], sleep [%d]", pc->debug.lint, pc->debug.trace_level, pc->debug.sleep);
        
    fclose (f);
    return;
}


// 
// Returns 1 if HTML output is disabled (any web output in general, not just "HTML"). When disabled, calls such as 
// print-web do not work. Typically this is used for delivering documents
// over the web to the browser.
//
int cld_is_disabled_output()
{
    CLD_TRACE ("");
    return CTX.req->disable_output;
}

// 
// Disable HTML output. This is typically done to output binary files.
//
void cld_disable_output()
{
    CLD_TRACE ("");
    CTX.req->disable_output = 1;
}

// 
// Enable HTML output.
//
void cld_enable_output()
{
    CLD_TRACE ("");

    // We do NOT clear pc->out.buf_pos=0 here, because if html output WAS disabled, NOTHING was written to the 
    // buffer (except for string writings). We do this check in cld_printf()! So no need to clear here.

    CTX.req->disable_output = 0;

}

// 
// Set exit code for  command line program. 'ec' is the exit code.
//
void cld_set_exit_code(int ec)
{
    CLD_TRACE ("");
    CTX.req->exit_code = ec;
}


//
// Enables command line mode (i.e. batch processing). Programs executed from command line
// must have this enabled.
//
void cld_enable_batch_processing()
{
    CLD_TRACE ("");
    CTX.req->disable_output = 1;
    CTX.req->bin_done = 1; // prevent error on cld_shut()
}


// Save HTML to a file to be lint-ed, if specified in debug file
// Linting checks xhtml syntax and displays the error at the top of browser window.
// Returns 1 if okay, 0 if failed.
//
int cld_save_HTML ()
{
    CLD_TRACE("");

    cld_config *pc = cld_get_config();
    FILE *f = NULL;
    char fname[300];
    
    if (pc->debug.lint == 1 && pc->ctx.req->disable_output != 1)
    {
        // we use pc->trace.time and not cld_current_time() because save_HTML is called from flush_printf, which can 
        // be called many times during the output of the web page. So effectively we would create multiple file names and 
        // multiple files, and check them separately, which will likely result in an error, even if those files put together
        // had no error
        snprintf(fname, sizeof(fname), "%s/%s_%d.out", pc->app.log_directory, pc->trace.time, cld_getpid());
        f = fopen (fname, "r");
        // we can write to stdout in multiple pieces 
        // so file is opened for APPEND, but if it doesnt exist, it is created
        if (f == NULL) 
        {
            f = fopen (fname, "w+");
            if (f == NULL) 
            {
                CLD_TRACE("Cannot open save file [%s], error [%s]", fname, strerror (errno));
                return 0;
            }
        }
        else
        {
            fclose (f); // since we just opened it for 'r'eading
            f = fopen (fname, "a+");
            if (f == NULL) 
            {
                CLD_TRACE("Cannot open save file [%s], error [%s]", fname, strerror (errno));
                return 0;
            }
        }
        CLD_TRACE("writing lint file [%s]", fname);
        int res = fprintf (f, "%s", pc->out.buf == NULL ? "" : pc->out.buf);
        CLD_TRACE ("Written [%d] bytes", res);
        fclose (f);
        f = NULL;

    }
    return 1;
}
 
// 
// Lint given text (for proper XHTML).
// Returns if okay, or errors out if there is a problem. Error line in trace file will show the result of linting.
// Does linting only if lint is set to 1 in debug file, otherwise does nothing.
// This is strictly for debugging, not production.
//
void cld_lint_text(const char *html)
{
    CLD_TRACE("");
    cld_config *pc = cld_get_config();
    if (pc->debug.lint == 1)
    {
        char fname[300];
        char lexec[100 + 2 * sizeof (fname) + 1];
        int res;

        //
        // Use double underscore to differential from html page lint. Since this lint crashes program if it fails,
        // there is no need to enumerate
        //
        snprintf(fname, sizeof(fname), "%s/%s__%d.out", pc->app.log_directory, pc->trace.time, cld_getpid());

        //
        // Write text to a file
        //
        int write_st;
        if ((write_st = cld_write_file (fname, html, strlen(html), 0)) != 1)
        {
            cld_report_error ("Cannot write file [%s], error [%d]", fname, write_st);
        }

        snprintf (lexec, sizeof (lexec) , 
            "cat %s|xmllint --html --noout - 2>> %s.err",
            fname, fname);
        CLD_TRACE ("lint text: [%s]", lexec);
        int sres = system (lexec);
        CLD_TRACE ("system(): [%d]", sres);

        snprintf (lexec, sizeof (lexec) , "%s.err", fname);
        res = (int)cld_get_file_size (lexec); // check if linting output is non-null
        CLD_TRACE ("lint size: [%d]", res);
        if (res > 0)
        {
            // if there is a problem, report an error
            cld_report_error ("Error in linting, file [%s], error [%d], text linted [%s]", fname,  write_st, html);
        }
        else
        {
            if (unlink (lexec) != 0)
            {
                CLD_TRACE("Could not delete temp file [%s], error [%s]", lexec, strerror(errno));
            }
            if (unlink (fname) != 0)
            {
                CLD_TRACE("Could not delete temp file [%s], error [%s]", fname, strerror(errno));
            }
        }
    }
}



// 
// Lint web output (for proper XHTML).
// Returns -1 if error written, 1 otherwise.
// If there is an error, .err file will remain and red/yellow display at the top of the browser
// window will attempted to be placed.
// Does linting only if lint is set to 1 in debug file and html output is not disabled.
// This is strictly for debugging, not production.
//
int lint()
{
    CLD_TRACE("");
    cld_config *pc = cld_get_config();
    if (pc->debug.lint == 1 && pc->ctx.req->disable_output != 1)
    {
        char fname[300];
        char lexec[100 + 2 * sizeof (fname) + 1];
        int res;

        snprintf(fname, sizeof(fname), "%s/%s_%d.out", pc->app.log_directory, pc->trace.time, cld_getpid());
        snprintf (lexec, sizeof (lexec) , 
            "cat %s|xmllint --html --noout - 2>> %s.err",
            fname, fname);
        CLD_TRACE ("lint: [%s]", lexec);
        int sres = system (lexec);
        CLD_TRACE ("system(): [%d]", sres);

        snprintf (lexec, sizeof (lexec) , "%s.err", fname);
        res = (int)cld_get_file_size (lexec); // check if linting output is non-null
        CLD_TRACE ("lint size: [%d]", res);
        if (res > 0)
        {
            // if there is a problem, reject page and exit
            // there is no ELSE AMOD because for batch mode, HTML OUTPUT IS DISABLED!
#ifdef AMOD
            // output header (if not already output)
            cld_output_http_header(pc->ctx.req);
            // output error message
            cld_printf (CLD_NOENC, "%s%s%s", "<div style='position:fixed;top:0;left:0;z-index:100000;color:red;background-color:yellow'>Error in lint of HTML, log file [",
                            lexec, "]</div>");
            // no need to flush, since lint() is called from a final flush (called ultimately from cld_shut() ONLY).
#endif
            return -1;
        }
        else
        {
            if (unlink (lexec) != 0)
            {
                CLD_TRACE("Could not delete temp file [%s], error [%s]", lexec, strerror(errno));
            }
            if (unlink (fname) != 0)
            {
                CLD_TRACE("Could not delete temp file [%s], error [%s]", fname, strerror(errno));
            }
        }
    }
    return 1;
}


// 
// Flush output from the program, be it to the web or to strings.
// Print out whatever is in buffers (pc->out.buf) which can be empty.
//
// 'fin' is 1 if this is called on cld_shut(), i.e. on the very exit of page
// This can be called when a certain buffer is full, or on write_to_string
// or on any random moment,so NO ASSUMPTIONS on what moment it can happen
// This handles both writes to the web (i.e. to the browser) and writing to the string.
//
// Returns -1 if no output is possible (not in writing string AND output disabled), or the
// number of bytes written (0 or more).
//
//
int cld_flush_printf(int fin)
{
    CLD_TRACE("");
    cld_config *pc =cld_get_config();
    //
    // We insure that CTX.out.was_there_any_output_this_request is set to 0
    // for the next request (regardless of which module will be used next by this process) by 
    // setting this value to 0 in cld_init_config(). 
    //
    int res = 0;

    // is there anything in the buffer
    int any_here = (pc->out.buf_pos > 0 ? 1 : 0);

    // we have CTX.out.was_there_any_output_this_request to avoid the last flush falling exactly empty (i.e. the previous flush did all of it and the 
    // fin=1 (i.e. from cld_shut() is empty - in which case we may mistakenly think there was NEVER any output!
    if (pc->ctx.out.was_there_any_output_this_request == 0 && any_here != 0) pc->ctx.out.was_there_any_output_this_request = 1;

    CLD_TRACE("any here [%d]", any_here);


    // even when disabled, do debugging output to file 
    // this is only for debugging (if saving is enabled in debug file)
    // no debugging for writing to string, only if not writing to string (==-1)
    // Also, check any_here. This flag is set before we set pc->out.buf_pos to 0, and it
    // represents if there is any data to be flushed at all at the entry to this function. We check it
    // together with pc->out.buf!=NULL, as a condition if there is any data to be written. If we don't check this,
    // then whatever was written last (to web or to a string) will still be in the buffer and will go the save HTML
    // for linting, clearly being wrong. We check this condition below too, for actual flushing to web or string.
    if (pc->ctx.req->curr_write_to_string == -1 && pc->out.buf != NULL && any_here == 1) cld_save_HTML ();

    // The code to do linting MUST be after cld_save_HTML() because that's where the code is saved to a file that we lint (saved only if linting active).
    // Also, the code to do linting MUST be before actual flushing before, because if we do write some error message into HTML, then we'd have to flush again.
    // This code also MUST be before pc->out.buf_pos = 0 setting below, because we may write HTML in lint(), and if buf_pos is now 0, it will just overwrite some HTML,
    // causing mess in the output.
    // Finally, this code must be only if fin==1, i.e. if this is final flushing, and it must be prior to setting pc->ctx.out.was_there_any_output_this_request back to 0 (which is
    // at the end of this function).
    // Check for lint ONLY at the end of request - this ensures lint output does NOT happen in the middle of something, although if html output is wrong (say missing last part)
    // it could end up being like that, no guarantees. We check in the lint  if the html output is disabled.
    // Check lint ONLY if this is the end of output (which it is here called from cld_shut() with fin==1), and if NOT the string output (which it shouldn't compile
    // like that but we check anyway), and if nothing ever output prior to now, don't bother.
    // Lint will output any error AFTER the html has been generated, so it will be out of <html>...</html>. This renders okay
    // for now, but no guarantees in the future, when a more clever approach may be needed.
    if (fin == 1 && pc->ctx.req->curr_write_to_string == -1 && pc->ctx.out.was_there_any_output_this_request == 1) lint();

    //
    // to_write MUST be calculated after linting() because linting can change the number of bytes to write!
    //
    int to_write = pc->out.buf_pos; // bytes to write before zeroing buf_pos


    // since we flush here, the position to write will be 0 afterwards
    // no matter what we end up doing below
    pc->out.buf_pos = 0; // we will flush it just now, start from the beginning again



    // disable_output (disabled web output) does NOT print anything ever
    // for the entire duration of web page.  Make sure this isn't writing to string 
    // (since we can write to string when HTML output is disabled (or if 
    // batch processing is enabled))
    if (pc->ctx.req->disable_output == 1 && pc->ctx.req->curr_write_to_string == -1) 
    {
        return 1;// no text output for binary
    }

    // check only at the end, even if it was writing to a variable, it has to have some
    // output after that, 
    if (pc->ctx.out.was_there_any_output_this_request == 0 && fin == 1)
    {
        // nothing was written ! (a check for web server only, variables can be empty)
        if (pc->ctx.req->curr_write_to_string != -1) cld_report_error ("No output generated by the program");
    }

    if (pc->out.buf != NULL && any_here == 1)
    {
        if (pc->ctx.req->curr_write_to_string != -1)
        {
            // TODO if this is a first flush to string (not of buffer, but of string), we can just copy the pointer pc->out.buf to string and use buf_pos as length
            // next flushes must be copied
            pc->ctx.req->write_string_arr[pc->ctx.req->curr_write_to_string].len += (res=cld_copy_data_at_offset (pc->ctx.req->write_string_arr[pc->ctx.req->curr_write_to_string].string , pc->ctx.req->write_string_arr[pc->ctx.req->curr_write_to_string].len, pc->out.buf));
            
            // some pc->out.buff will have new line at the end and some wont'. If a line has a non-cld character, as in <? ...?>X<?...?> (X is a non-cld)
            // then there is a new-line. Otherwise not. When we write the string, we don't want any trailing whitespaces. We already trim the line at the
            // beginning so there is no trailing at the front.
            // (the reason for above is that if there is only cld-chars, it is printed as a blank line in source and in html which is confusing. It is
            // only in write-string where this matters, and we don't want to have inconsistencies.
            // BUT, this is ONLY, if this is ending-of-writing-string - i.e. only for the trail characters at the end of write-string construct

            if (pc->ctx.req->write_string_arr[pc->ctx.req->curr_write_to_string].is_end_write == 1)
            {
                while (isspace((*(pc->ctx.req->write_string_arr[pc->ctx.req->curr_write_to_string].string))[pc->ctx.req->write_string_arr[pc->ctx.req->curr_write_to_string].len-1])) pc->ctx.req->write_string_arr[pc->ctx.req->curr_write_to_string].len--;  
                (*(pc->ctx.req->write_string_arr[pc->ctx.req->curr_write_to_string].string))[pc->ctx.req->write_string_arr[pc->ctx.req->curr_write_to_string].len] = 0;
            }
        }
        else
        {
            CLD_TRACE("To flush [%s] writing [%d]", pc->out.buf, to_write);
            // We may call flushing just before write-string (or maybe elsewhere) and we'd get here. If there was nothing to write,
            // we'd get an error "No header sent prior to html data". Avoid that here since there's no data to flush anyway.
            if (to_write==0) 
            {
                return 0;
            }
            // check if we're reporting of error already. This is because when we report error, we go to print out data
            // and this way we would be caught in calling it over and over again (it meaning cld_report_error)
            // there is no ELSE AMOD because for batch mode, HTML OUTPUT IS DISABLED!
            if (pc->ctx.req->sent_header == 0 && pc->ctx.cld_report_error_is_in_report == 0) cld_report_error ("No header sent prior to html data");
#ifdef AMOD
            res = cld_ws_write (pc->ctx.apa, pc->out.buf, to_write);
            if (res < 0) CLD_TRACE ("Error in writing, error [%s]", strerror(errno));
            else CLD_TRACE("Wrote [%d] bytes", res);
            int flush_res = cld_ws_flush (pc->ctx.apa);
            CLD_TRACE("Flushed to web [%d]", flush_res);

#endif
        }
    }


    return res; 
}


// 
// Clean up of print on end of request, so next time around, it can be used from scratch
//
void cld_printf_close()
{
    CLD_TRACE("");
    cld_config *pc =cld_get_config();


    pc->out.buf_pos = 0; // just in case we reuse this for multiple prints
            // which right now, we don't, but we could
    if (pc->out.buf != NULL) cld_free (pc->out.buf);
    pc->out.buf = NULL;
    pc->out.len = 0;
}


// 
// Output string 's'. 'enc_type' is either CLD_WEB, CLD_URL or 
// CLD_NOENC. Returns number of bytes written.
// "Output" means either to the browser (web output) or to the string.
// For example, if within write-string construct, it's to the string, 
// otherwise to the web (unless HTML output is disabled).
//
// There are only 2 output functions: cld_puts and cld_printf, and they both
// call cld_puts_final(). NO OTHER way of output should be present and NOTHING
// else should call cld_puts_final.
//
//
int cld_puts (int enc_type, const char *s)
{
    assert(s);
    CLD_TRACE ("");

    if (cld_validate_output()!=1) return 0;

    cld_config *pc = cld_get_config();


    int buf_pos_start = pc->out.buf_pos;
    int vLen = strlen(s);
    int res = 0;
    if (enc_type==CLD_NOENC)
    {
       // no encoding just put the string out
       return cld_puts_final (s, vLen); 
    }
    while (1)
    {
        // resize buffer to needed size and encode directly into the buffer
        // without having to memcpy needlessly
        if (CLD_MAX_ENC_BLOWUP(vLen) > pc->out.len -1-pc->out.buf_pos)
        {
            pc->out.len = pc->out.len + CLD_PRINTF_ADD_LEN;
            char *oldBuf;
            pc->out.buf = (char*)cld_realloc (oldBuf = pc->out.buf, pc->out.len);
            continue;
        }
        else
        {
            char *write_to = pc->out.buf+pc->out.buf_pos;
            res = cld_encode_base (enc_type, s, vLen, &(write_to), 0);
            pc->out.buf_pos += res;
        }
        break;
    }
    CLD_TRACE ("HTML>> [%s]", pc->out.buf + buf_pos_start);
    return res;
}


// 
// Initialize output buffer (used in writing to web and strings)
// so it starts from scratch
//
void cld_init_output_buffer ()
{
    cld_config *pc = cld_get_config();
    pc->out.len = CLD_PRINTF_ADD_LEN;
    pc->out.buf = (char*) cld_malloc (pc->out.len);
    pc->out.buf_pos = 0;
}


// 
// Check if output can happen, if it can, make sure output buffer is present
// and if it needs flushing, flush it.
//
int cld_validate_output ()
{
    CLD_TRACE("");

    // if output is disabled, do NOT waste time printing to the bufer!!
    // UNLESS this is a write to a string, in which case write it!!
    // If we allow writing, then say output is disabled and we write and then flush happens at any time
    // and the program CRASHES because header wasn't sent!!!!
    cld_config *pc = cld_get_config();
    if (pc->ctx.req->disable_output == 1 && pc->ctx.req->curr_write_to_string == -1) return  0;

    // if no buffer, create one
    if (pc->out.buf == NULL)
    {
        // reset pc->out to a default buffer with no data
        cld_init_output_buffer ();
    }

    // as we print out, we add to the buffer. Its size starts with CLD_PRINTF_ADD_LEN
    // and this is added with more printing. Once it reaches CLD_PRINTF_MAX_LEN, the 
    // output is flushed
    // Note: the number we check if buf_pos (which is the exact number of bytes written
    // to the buffer MINUS the zero byte at the end), and not pc->out.len, because 'len'
    // is just the SIZE of the buffer, which can be rather large at time, but have very 
    // few bytes in it at the same time (depending on what's written in it currently).
    if (pc->out.buf_pos >= CLD_PRINTF_MAX_LEN)
    {
        cld_flush_printf (0); // flush output
    }
    return 1;
}

// 
// ** IMPORTANT: THis function is one of the TWO outputters (this and cld_puts) meaning 
// these are SOLE callers of cld_puts_final. This is to ensure there is no circumvention of 
// disabled output or anything else.
//
// Outpus to web or to strings. enc_type is CLD_WEB, CLD_URL or CLD_NOENC
// and the rest is a printf-like format.
//
int cld_printf (int enc_type, const char *format, ...) 
{
    CLD_TRACE ("");
    
    if (cld_validate_output()!=1) return 0;
    cld_config *pc = cld_get_config();

    int tot_written = 0;

    va_list args;
    va_start (args, format);
    // TODO If this is CLD_NOENC, we can print this (snprintf) directly to pc->out.buf, avoiding extra copying of buffer
    // So there would be two while(1) loops, one for CLD_URL/CLD_WEB, one for CLD_NOENC. A different kind of cld_encode()
    // can be devised to encode directly into pc->out.buf as well. This would eliminate one copy from the entire output!
    while (1)
    {
        // tot_written (i.e. the return value) is what would have been written if there was enough space EXCLUDING the null byte.
        // bytes_left (i.e. second parameter) is the number of bytes available to write INCLUDING the null byte.
        // So if bytes_left is 255, and the return value is 255, it means the space needed was 255+1 (because the return value 
        // excludes the null byte!), so if return value is grear OR EQUAL to the size available (ie. second parameter), then we need to realloc.
        int bytes_left = pc->out.len - pc->out.buf_pos;
        tot_written = vsnprintf (pc->out.buf + pc->out.buf_pos, bytes_left, format, args);
        if (tot_written >= bytes_left)
        {
            pc->out.len += CLD_PRINTF_ADD_LEN;
            pc->out.buf = cld_realloc (pc->out.buf, pc->out.len);
            va_end (args); // must restart the va_list before retrying!
            va_start (args, format);
            continue;
        }
        else 
        {
            // buf_pos does NOT include trailing zero, even though we put it there. 
            pc->out.buf_pos += tot_written;
            break;
        }
    }
    va_end (args);
    int ret = 0;
    switch (enc_type)
    {
        case CLD_URL: 
        case CLD_WEB:; // has to have ; because declaration (char *final... CANNOT be
                      // after label (which is case ...:)
            char *final_out = NULL;
            pc->out.buf_pos-=tot_written;
            int final_len = cld_encode (enc_type, pc->out.buf+pc->out.buf_pos, &final_out);
            ret = cld_puts_final (final_out, final_len);
            cld_free (final_out);
            break;
        case CLD_NOENC:
            // nothing to do, what's printed to output buffer is there to stay unchanged
            break;
        default: cld_report_error ("Unknown encoding type [%d]", enc_type);
    }
    return ret;
}

// ** IMPORTANT:
// ** This function can be called only from cld_printf or cld_puts!! The reason is this way we control
// ** exactly what goes out and there's nothing to circumvent that.
//
// Outputs to web or string.
// Print to memory so we can trace html output, and also lint it (if needed for web output)
// Major purposes of this function is to collect all output into a 
// single memory chunk to improve performance of outputting. The memory can grow as
// needed to accomodate unlimited writes (but limited by virtual memory).
// When memory grows beyond some limit, we flush the output to the web server and start over.
//
// 'final_out' is the string being output, and final_len is its length. Returns
// number of bytes written excluding zero at the end.
//
int cld_puts_final (const char *final_out, int final_len)
{
    CLD_TRACE("");


    cld_config *pc = cld_get_config();


    // here we add to the buffer, which may be periodically flushed, for example
    // if CGI writes a huge report and then sends it somewhere, but doesn't display
    // to web, this output can be huge and more than available memory.
    int buf_pos_start = pc->out.buf_pos;
    int res = 0;
    while (1)
    {
        // if we need to write more than currently allocated memory, add more
        // final_len is the length of string, we check we have final_len+1 bytes left (at least)
        if (final_len > pc->out.len -1-pc->out.buf_pos)
        {
            pc->out.len = pc->out.len + CLD_PRINTF_ADD_LEN;
            char *oldBuf;
            pc->out.buf = (char*)cld_realloc (oldBuf = pc->out.buf, pc->out.len);
            continue;
        }
        else
        {
            memcpy (pc->out.buf + pc->out.buf_pos, final_out, final_len + 1);
            pc->out.buf_pos += final_len;
            res = final_len;
        }
        break;
    }
    if (res == 0) return 0; // return number of bytes written, minus null at the end
    CLD_TRACE ("HTML>> [%s]", pc->out.buf + buf_pos_start);
    return res;
}


// 
// Shut down the request, 'giu' is the request. 
// This will flush any outstanding output to the web, and for command line it will
// set the exit code and disconnect from the database (for the web version, we are
// ALWAYS connected, there is no disconnect on our part). If we were outputting 
// binary file and didn't go through, send error response.
// We NEVER EXIT with report-error, but we do exit with a signla caught (say SIGSEGV)
// except if web server informed us to do so by sending a termination signal (see chandle.c)
//
void cld_shut(input_req *giu)
{

    // cld_shut can be called from cld_report_error AND at the end of cld_main
    // it should not be called twice
    if (giu != NULL && giu->is_shut ==1) return;

    if (giu != NULL) giu->is_shut = 1;

    CLD_TRACE("Shutting down");
    if (giu == NULL)
    {
        CLD_FATAL_HANDLER ("Input request is NULL");
    }
    cld_config *pc = cld_get_config();
    if (pc == NULL)
    {
        CLD_FATAL_HANDLER ("config is NULL");
    }

    // print out first, so that web client can get information sent out so far
    // because the following code CAN fail, in which case cld_report_error (if already
    // called) may be called again, and in that case, it will just exit
    cld_flush_printf (1); // 1==final output
    cld_printf_close();

    int ec = giu->exit_code;

    // we NEVER close connection for web-server module
#ifndef AMOD
    cld_close_db_conn ();
#endif

    if (giu->disable_output == 1 && giu->bin_done == 0)
    {
        cld_cant_find_file("Could not find server file (unknown)");
    }

// trace for apache module is opened once for request, and it closes when it ends here
    cld_close_trace ();


#ifdef AMOD
    CLD_UNUSED(ec);
#else
    curl_global_cleanup();
    exit (ec);
#endif

#ifdef AMOD
    //
    // Perform one final 'chunked' output to assure 0 bytes are sent
    // but ONLY if this is HTML output. For files, we send byte size, so cannot mix
    // with chunked output.
    //
    if (giu->disable_output == 0)
    {
        cld_ws_finish (pc->ctx.apa);
    }
#endif


}


// 
// Output error message if file requested (image, document.., a binary file in 
// general) could not be served.
// reason is why we couldnt deliver the file.
//
void cld_cant_find_file (const char *reason)
{
    CLD_TRACE ("");
    cld_config *pc = cld_get_config();
    // never print out for batch mode
#ifdef AMOD
    cld_ws_set_status (pc->ctx.apa, 404, "404 Not Found");
    cld_ws_set_header (pc->ctx.apa, "Status", "404");
    cld_ws_set_header (pc->ctx.apa, "Cache-Control", "max-age=0, no-cache");
    cld_ws_set_header (pc->ctx.apa, "Pragma", "no-cache");
    cld_ws_printf (pc->ctx.apa, "<!DOCTYPE html><html><body>Document requested not found: %s.</body></html>", reason);
    // flush any buffers from sending data to client (as apache or tcp may buffer)
    int flush_res = cld_ws_flush (pc->ctx.apa);
    CLD_TRACE("Flushed web data [%d]", flush_res);
#else
    CLD_UNUSED (reason);
#endif
    pc->ctx.req->bin_done = 1;
}


// 
// Output error message if request method is not supported (such as HEAD, PUT, OPTIONS, DELETE, TRACK or TRACE)
// Output is 403 Forbidden.
// 'reason' is the reason why it's forbidden, 'detail' is any detail that can be provided
//
void cld_forbidden (const char *reason, const char *detail)
{
    CLD_TRACE ("");
#ifdef AMOD
    cld_config *pc = cld_get_config();
    cld_ws_set_status (pc->ctx.apa, 403, "403 Forbidden");
    cld_ws_set_header (pc->ctx.apa, "Status", "403 Forbidden");
    cld_ws_set_header (pc->ctx.apa, "Cache-Control", "max-age=0, no-cache");
    cld_ws_set_header (pc->ctx.apa, "Pragma", "no-cache");
    cld_ws_printf (pc->ctx.apa, "<!DOCTYPE html><html><body>%s [%s].</body></html>", reason, detail);
    // flush any buffers from sending data to client (as apache or tcp may buffer)
    int flush_res = cld_ws_flush (pc->ctx.apa);
    CLD_TRACE("Flushed web data [%d]", flush_res);
#else
    // nothing to do for batch mode
    CLD_UNUSED (reason);
    CLD_UNUSED (detail);
#endif
}


// 
// Output a file. HTML output must be disabled for this to work. Typically, binary files
// (images, documents) are served to the web browser this way.
// fname is file name. 'header' is the header to be output (which must be set by the caller).
// This handles 'if-none-match' (timestamp) so that if web client already has this binary file,
// only the cache confirmation is sent back thus improving performance ('not modified' response).
// File must exist or it will return 'document requested not found' to the client.
//
void cld_out_file (const char *fname, cld_header *header)
{
    CLD_TRACE("");
    cld_config *pc = cld_get_config();
    assert (header->ctype!=NULL);
    if (pc->ctx.req->disable_output  == 0)
    {
        CLD_TRACE ("Cannot send output when web output is enabled");
        return;
    }

    if (strstr (fname, "..") != NULL)
    {
        //
        // We do not serve files with .. in them to avoid path traversal attacks. Files must
        // not traverse backwards EVER.
        //
        cld_cant_find_file("File path insecure, rejected");
        return;
    }


    struct stat attr;
    if (stat(fname, &attr) != 0)
    {
        CLD_TRACE ("Cannot stat file name [%s], error [%s]", fname, strerror (errno));
        cld_cant_find_file("Cannot stat file");
        return;
    }
    long tstamp = (long)attr.st_mtime;

    FILE *f = fopen (fname, "r");
    if (f == NULL)
    {
        if (pc->ctx.req->bin_done == 0)
        {
            CLD_TRACE("Cannot open [%s], error [%s]", fname, strerror(errno));
            cld_cant_find_file("Cannot open file");
            return;
        }
        // same as cld_shut - 404
    }
    else
    {
        //
        // We're using integers throughout Cloudgizer
        //
        fseek(f, 0, SEEK_END);
        long fsize_l = ftell(f);
        if (fsize_l >= (long)INT_MAX)
        {
            CLD_TRACE ("File size too long [%ld]", fsize_l);
            cld_cant_find_file("File too long");
            return;
        }
        int fsize = (int) fsize_l;
        fseek(f, 0, SEEK_SET);

        // 
        // check if file has already been delivered to the client
        //
        CLD_TRACE("IfNoneMatch [%s], tstamp [%ld]", pc->ctx.req->if_none_match == NULL ? "" : pc->ctx.req->if_none_match, tstamp);
        if (pc->ctx.req->if_none_match != NULL && tstamp == atol(pc->ctx.req->if_none_match))
        {
            //
            // File NOT modified
            //
            CLD_TRACE("File not modified! [%s]", fname);
#ifdef AMOD
            cld_ws_set_status (pc->ctx.apa, 304, "304 Not Modified");
            cld_ws_set_header (pc->ctx.apa, "Status", "304 Not Modified");
            if (header->cache_control!=NULL)
            {
                CLD_TRACE("Setting cache [%s] for HTTP header (2)", header->cache_control);
                cld_ws_set_header (pc->ctx.apa, "Cache-Control", header->cache_control);
            }
            else
            {
                CLD_TRACE("Setting no cache for HTTP header (3)");
                cld_ws_set_header (pc->ctx.apa, "Cache-Control", "max-age=0, no-cache");
                cld_ws_set_header (pc->ctx.apa, "Pragma", "no-cache");
            }
            // flush any buffers from sending data to client (as apache or tcp may buffer)
            int flush_res = cld_ws_flush (pc->ctx.apa);
            CLD_TRACE("Flushed web data [%d]", flush_res);
#endif
            fclose (f);
            pc->ctx.req->bin_done = 1; // set when request for binary file has been handled
            return;
        }

        // 
        // read file to be sent to the client
        //
        CLD_TRACE("File read and to be sent [%s]", fname);
        char *str = cld_malloc(fsize + 1);
        if (fread(str, fsize, 1, f) != 1)
        {
            CLD_TRACE ("Cannot read [%d] bytes from file [%s], error [%s]", fsize, fname, strerror(errno));
            cld_cant_find_file("Cannot read file");
            return;
        }
        fclose(f);

        //
        // The data read is in 'str' and the size of data is 'fsize'
        // which is what the following code expects.
        //

#ifdef AMOD
        char tm[50];
        char *val;
#endif
        if (header->etag==1)
        {
            CLD_TRACE("Will send etag [%ld]", tstamp);
        }
        else
        {
            CLD_TRACE("Will NOT send etag [%ld]", tstamp);
        }


        if (header->ctype[0] == 0)
        {
            //
            // Content type is missing, we assume it's HTML
            //
            CLD_TRACE("Sending HTML, no content type");
            // html has to be empty because we have own set of headers
#ifdef AMOD
            if (header->etag==1)
            {
                snprintf (tm, sizeof(tm), "%ld", tstamp);
                CLD_STRDUP (val, tm);
                cld_ws_set_header (pc->ctx.apa, "Etag", val);
            }
#endif

            // outputting the header for html will not work if web output is disallowed
            // we will allow it momentarily and then go back to what it was.
            int saved = pc->ctx.req->disable_output;
            pc->ctx.req->disable_output = 0;
            cld_output_http_header(pc->ctx.req); // all extract tags like Etag must come before because this will
                        // end the header section with two CRLF
            pc->ctx.req->disable_output = saved;
        }
        else
        {
            // 
            // Send file and appropriate header first
            //
            char disp_name[500];
            CLD_TRACE("Header disp is [%s]", header->disp==NULL?"NULL":header->disp);
            if (header->disp != NULL)
            {
                if (header->file_name != NULL)
                {
                    char *enc = NULL;
                    cld_encode (CLD_URL, header->file_name, &enc);
                    snprintf (disp_name, sizeof(disp_name), "%s; filename*=UTF8''%s", header->disp, enc);
                } 
                else 
                {
                    snprintf (disp_name, sizeof(disp_name), "%s", header->disp);
                }
            }
#ifdef AMOD
            CLD_STRDUP (val, header->ctype);
            cld_ws_set_content_type(pc->ctx.apa, val);
            snprintf (tm, sizeof(tm), "%d", fsize);
            CLD_STRDUP (val, tm);
            cld_ws_set_content_length(pc->ctx.apa, val);
            if (header->disp != NULL)
            {
                cld_ws_set_header (pc->ctx.apa, "Content-Disposition", disp_name);
            }
            if (header->cache_control!=NULL)
            {
                CLD_TRACE("Setting cache [%s] for HTTP header (4)", header->cache_control);
                cld_ws_set_header (pc->ctx.apa, "Cache-Control", header->cache_control);
            }
            else
            {
                CLD_TRACE("Setting no cache for HTTP header (5)");
                cld_ws_set_header (pc->ctx.apa, "Cache-Control", "max-age=0, no-cache");
                cld_ws_set_header (pc->ctx.apa, "Pragma", "no-cache");
            }

            // send etag for client to send back when asking again. If ETag is the same
            // for this file name, then it will be considered cached at the client and
            // 'not modified' message will be sent back.
            if (header->etag==1)
            {
                snprintf (tm, sizeof(tm), "%ld", tstamp);
                CLD_STRDUP (val, tm);
                cld_ws_set_header (pc->ctx.apa, "Etag", val);
            }

            // add any headers set from the caller
            int i;
            for (i = 0; i<CLD_MAX_HTTP_HEADER; i++)
            {
                if (header->control[i]!=NULL && header->value[i]!=NULL)
                {
                    // we use add_header because it allows multiple directives of the same kind
                    // set_header allows ONLY ONE instance of a header. We don't know what will go here.
                    cld_ws_add_header (pc->ctx.apa, header->control[i], header->value[i]);
                }
                else break;
            }
            
#endif
        }
#ifdef AMOD
        // Send actual file contents
        if (cld_ws_write (pc->ctx.apa, str, fsize) != fsize)
        {
            pc->ctx.req->bin_done = 1; 
            CLD_TRACE ("Cannot write [%d] bytes to client from file [%s], error [%s]", fsize, fname, strerror(errno));
            // In this case, since cld_ws_write is synchronous, the server couldn't send all the data and it closed the connection
            // with the client. Nothing else to do for us.  
        }
        int flush_res = cld_ws_flush (pc->ctx.apa);
        CLD_TRACE("Flushed to web (1) [%d]", flush_res);
#endif
        pc->ctx.req->bin_done = 1;
    }
}



//
// Initialize html header. Default is cached forever (practically).
// 'header' is the html header structure
//
void cld_init_header (cld_header *header)
{
    CLD_TRACE("");
    assert (header!=NULL);
    header->etag=1; // send etag by default, so even if we cache forever, but browser decides it can't, it can still benefit from etag
    header->ctype="text/html"; // must always be set
    header->disp=NULL; // default is show object, not download
    header->file_name=NULL; // this is only if disp is not NULL
    // No status if set to 0
    header->status_id=0;
    header->status_text=NULL;
    // Default header for non-dynamic content such as images, or in general documents.
    // We deliver documents based on a database ID number which never changes if the
    // document doesn't. Any change and the ID changes too. 
    header->cache_control= "public, max-age=2000000000, post-check=2000000000, pre-check=2000000000";  // default is cache forever (actually 53 years) - we are staying within an signed int, so to work anywhere
    int i;
    // any number of headers. The first from index 0 that has control or value NULL is where we stop looking. So no gaps.
    for (i = 0; i<CLD_MAX_HTTP_HEADER; i++)
    {
        header->control[i]=NULL;
        header->value[i]=NULL;
    }
}


// 
// Execute program with command-line arguments and capture its exist status, and get only the first line of it's 
// stdout or stderr. cmd is the full path of program. argv is the list of arguments (a number of which is
// 'num_args'), and first line of output is stored in 'buf' buffer with available length of buf_len bytes.
// argv[num_args] must be NULL, so if arguments are 'x' and 'y', then argv[0] is 'x', argv[1] is 'y' and 
// argv[2] is NULL and num_args is 2.
// Returns exit status of program execution.
//
int cld_exec_program_out_data (const char *cmd, const char *argv[], int num_args, char *buf, int buf_len)
{
    CLD_TRACE("");
    assert (buf);
    assert (cmd);
    assert (argv);
    assert (buf_len > 0);

    if (argv[num_args] != NULL)
    {
        cld_report_error ("Number of arguments does not match last NULL");
    }

    pid_t pid;
    int pf[2];
    // pf[0] is read, pf[1] is write of pipe
    // when fork happens, read of parent is write of child and vice versa
    if (pipe(pf) == -1) 
    {
        cld_report_error ("Cannot create pipes, error [%s]", strerror(errno));
    }
    
    // note dup2() cannot get EINTR (interrrupt) on LINUX, so no checking
    // dup2(a,b) means that b is copy of a, and both can be used to read or write. So in the context of what we do:
    // 1. if there is something written to b, it will be redirected to a too. b is no longer relevant if we don't care.
    // 2. if there is something to read from a , it will be read from b. a is no longer relevant if we don't care.
    pid = fork();
    if (pid == -1) 
    { 
        cld_report_error ("Cannot create child, error [%s]", strerror(errno));
    }
    else if (pid == 0) 
    {
        // child
        close(pf[0]);    // close read-end of pipe
        dup2(pf[1],STDOUT_FILENO);  // duplicate pipe-out to stdout
        dup2(pf[1],STDERR_FILENO);  // duplicate pipe-out to stderr
        close(pf[1]);   // close pipe-out (duplicate)
        execv(cmd, (char *const*)argv); // will send data to parent
        _exit(1);  // failed to exec, do not flush stdout twice (if exit()
            // were called, it would call atexit() of parent and flush its stdout
            // and html output to apache would be duplicated!
    }
    else 
    { 
        // parent
        close (pf[1]); // close write-end of pipe, and just read from read-end
        FILE *fr = fdopen (pf[0], "r");
        if (fr == NULL)
        {
            cld_report_error ("Cannot open read pipe, error [%s]", strerror(errno));
        }
        char *res = fgets (buf, buf_len - 1, fr); 
        if (res == NULL) // either end or error
        {
            int err = ferror (fr);
            if (err) // if error, quit
            {
                cld_report_error( "Error [%s] reading pipe [%s]", strerror (errno), cmd);
            }
            else
            {
                // no output at all from the program
                buf[0] = 0;
            }
        }
        fclose (fr);
        close(pf[0]);
        int st;
        while (wait (&st) != pid) ; // wait until child finishes
        return st;
    }
    return 1; // will never actually reach here, only for compiler joy
}

// 
// Execute program with command-line arguments and capture its exist status, and pass and open file pointer 
// as its stdin, get the stdout/stderr to an open file pointer. This is the most generic of program execution 
// functions as input and output are theoretically unlimited.
// cmd is the full path of program. argv is the list of arguments (a number of which is
// 'num_args'), and fin is input file and fout is output. If *fout is NULL a temporary file will be created
// to place output to, and then you can access that file (via *fout) after the function call. If *fout is NOT NULL,
// then output goes to this file that you opened prior to calling this function.
// argv[num_args] must be NULL, so if arguments are 'x' and 'y', then argv[0] is 'x', argv[1] is 'y' and 
// argv[2] is NULL and num_args is 2.
// Returns exit status of program execution.
//
int cld_exec_program_with_in_out (const char *prg, const char *argv[], int num_args, FILE *fin, FILE **fout)
{
    CLD_TRACE("");
    assert (argv);
    assert (prg);

    if (argv[num_args] != NULL)
    {
        cld_report_error ("Number of arguments does not match last NULL");
    }

    pid_t pid;

    // create temp file if none provided
    // tmpfile() is considered secure because it is deleted on creation. Using readlink() to obtain its
    // name yields /tmp/... file name, however such file cannot be opened by an intruder and thus is considered
    // safe.
    if (*fout == NULL) *fout = tmpfile();

    // if cannot create temp file, report an error
    if (*fout==NULL)
    {
        cld_report_error ("Cannot create temporary file, error [%s]", strerror(errno));
    }
    
    // note dup2() cannot get EINTR (interrrupt) on LINUX, so no checking
    // dup2(a,b) means that b is copy of a, and both can be used to read or write. So in the context of what we do:
    // 1. if there is something written to b, it will be redirected to a too. b is no longer relevant if we don't care.
    // 2. if there is something to read from a , it will be read from b. a is no longer relevant if we don't care.
    pid = fork();
    if (pid == -1) 
    { 
        cld_report_error ("Cannot create child, error [%s]", strerror(errno));
    }
    else if (pid == 0) 
    {
        // child
        dup2(fileno(*fout),STDOUT_FILENO);  // use fin and fout as stdin and stdout for program
        dup2(fileno(*fout),STDERR_FILENO);  // in case there is out, it goes to file,st is <>0
        dup2(fileno(fin), STDIN_FILENO);
        execv(prg, (char *const*)argv); // will send data to parent
        _exit(1);  // failed to exec, do not flush stdout twice (if exit()
            // were called, it would call atexit() of parent and flush its stdout
            // and html output to apache would be duplicated!
    }
    else 
    { 
        int st;
        while (wait (&st) != pid) ; // wait until child finishes
        fseek (*fout, 0, SEEK_SET);
        fclose (fin);
        return st;
    }
    return 1; // will never actually reach here, only for compiler joy
}


// 
// Create a new unique document and return FILE pointer to a newly created file associated
// with a document. Output is: document_id is the newly generated unique ID (from cldDocumentIDGenerator
// table that must be present as part of Cloudgizer installation)), write_dir is the full path file name
// for which FILE * was opened. write_dir_len is the length of write_dir buffer.
// Returns FILE* to the file opened.
// The name of the file opened (which is write_dir) is based on the document_id.
// The goal is to create a file and have a unique ID for it that can be used for future tracking and use
// of the file.
//
FILE *cld_make_document (char **document_id, char *write_dir, int write_dir_len)
{
    CLD_TRACE("");
    assert (document_id != NULL);
    assert (write_dir != NULL);

    // initial document id (did)
    char *did_init = "                              ";
    int did_init_len = strlen (did_init);
    char *did = cld_init_string (did_init);
    
    // get document id from cldDocumentIDGenerator table
    cld_get_document_id (did, did_init_len);
    cld_copy_data (document_id , did);

    // create file path (into write_dir) for the file, based on document id, open the file and return its FILE *
    return cld_create_file_path (did, write_dir, write_dir_len);
}

// 
// Get environment variable. It can be a system environment variable or a
// web server environment variable (obtained from web server). 
// 'var' is the name of the variable.
// Returns value of environment variable.
// If this is command line program, only system environment variables are searched.
// If this is web program, first we search web environment, and if not found (only then)
// the system environment.
//
const char *cld_ctx_getenv (const char *var)
{
    CLD_TRACE("");
#ifndef AMOD
    // command line program
    const char *r = getenv (var);
    if (r == NULL) return ""; else return r;
#else
    // web program
    const char *r = cld_ws_get_env (CTX.apa, var);
    if (r == NULL || r[0] == 0)
    {
        CLD_TRACE("Environment variable [%s] not found in Web server, searching system", var);
        // not found in web environment, search system
        r = getenv (var);
        if (r == NULL) return ""; else return r;
    }
    else return r;
#endif
}


// 
// Returns web address from config file
//
const char *cld_web_address ()
{
    CLD_TRACE ("");
    cld_config *pc = cld_get_config ();
    return pc->app.web;
}


//
// Upper-cases string 's', returns upped value as well
// 's' is both input and output param.
//
inline char *cld_upper(char *s)
{
    CLD_TRACE("");
    assert (s);
    int l = 0;
    while (s[l] != 0) {s[l] = toupper(s[l]); l++;}
    return s;
}

// 
// Lower-cases string 's' and returns it.
// 's' is both input and output param.
//
inline char *cld_lower(char *s)
{
    CLD_TRACE("");
    assert (s);
    int l = 0;
    while (s[l] != 0) {s[l] = tolower(s[l]); l++;}
    return s;
}

// 
// Lock file 'filepath'. Outputs lock_fd which is the file descriptor of the locked (and
// open) file. File is to be closed with close() to release the lock.
// Returns 0 if cannot lock, -1 if cannot open file, 1 if locked,-2 invalid path
// File is created for read/write and read/write permission for owner only, and
// the lock is exclusive on the entire file..
// This function will wait until the lock is released.
// The purpose is a mechanism for resource locking. The file itself isn't meant
// to hold any useful data.
//
int cld_lockfile(const char *filepath, int *lock_fd)
{
    CLD_TRACE ("");
    struct flock lock;
    int fd;

    /* Invalid path? */
    if (filepath == NULL || *filepath == '\0')
        return -2;

    /* Open the file. */
    do 
    {
        fd = open(filepath, O_RDWR | O_CREAT, 0600);
    } while (fd == -1 && errno == EINTR);

    if (fd == -1) 
    {
        return -1;
    }

    /* Application must NOT close input/output/error, or those may get occupied*/

    /* Exclusive lock, cover the entire file (regardless of size). */
    lock.l_type = F_WRLCK;
    lock.l_whence = SEEK_SET;
    lock.l_start = 0;
    lock.l_len = 0;
    if (fcntl(fd, F_SETLK, &lock) == -1) 
    {
        /* Lock failed. Close file and report locking failure. */
        close(fd);
        return 0;
    }
    if (lock_fd != NULL) *lock_fd = fd;
    // success, the file is open and locked, and will remain locked until the process ends
    // or until the caller does close (*lock_fd);
    return 1;
}

// 
// Get a copy of input parameters (from input request 'req') into
// input parameters 'ip'. The input parameters are held in the same
// structure under 'req'. So the same members 'num_of_input_params',
// 'names', and 'values' that describe input parameters are in 'ip'
// as well.
//
void cld_get_input_params (input_req *req, cld_input_params *ip )
{
    CLD_TRACE ("");
    assert (req);
    assert (ip);
    int i;
    ip->num_of_input_params = req->ip.num_of_input_params;
    ip->names = cld_malloc (ip->num_of_input_params * sizeof (char**));
    ip->values = cld_malloc (ip->num_of_input_params * sizeof (char**));
    
    for (i = 0; i < ip->num_of_input_params; i++)
    {
        ip->names[i] = cld_strdup (req->ip.names[i]);
        ip->values[i] = cld_strdup (req->ip.values[i]);
    }
}

// 
// Given input parameters 'ip' (be it from input request
// or from a copy obtained via cld_get_input_params()), create
// a name=value portion of an URL string to which these parameters
// correspond. For example, the returned string could be 
// name1=value1&name2=value2.
// Returns the name=value portion of URL from which input parameters
// in 'ip' would have been (or had been) derived.
//
char *cld_construct_input_params (cld_input_params *ip)
{
    CLD_TRACE("");
    assert (ip);
    int i;
    char *res = CLD_EMPTY_STRING;
    int l = 0;
    int first = 1;
    for (i = 0; i < ip->num_of_input_params; i++)
    {
        if (ip->values[i][0] == 0) continue;
        if (first == 0) l += cld_copy_data_at_offset (&res, l, "&");

        l += cld_copy_data_at_offset (&res, l, ip->names[i]);
        l += cld_copy_data_at_offset (&res, l, "=");
        l += cld_copy_data_at_offset (&res, l, ip->values[i]);
        if (first == 1) first = 0;
    }
    return res;
}

// 
// Construct a full URL that corresponds to input parameters 'ip'. See
// cld_construct_input_params(). The difference is that the return string here
// has a web address (from config file), so it is a valid URL. For example, the 
// return value could be https://myweb.com/go.service?name1=value1&name2=value2
// Returns full URL based on input parameters from 'ip', and web address from
// config parameter file.
//
char *cld_construct_url (cld_input_params *ip)
{
    CLD_TRACE("");
    assert (ip);
    char *res = cld_init_string (cld_web_address());
    int l = strlen (res);
    l += cld_copy_data_at_offset (&res, l, "?");
    l += cld_copy_data_at_offset (&res, l, cld_construct_input_params (ip));
    return res;
}

// 
// Given input parameters 'ip'. and input parameter name 'name' and replacement value for it
// 'new_value', replace the value for input parameter 'name' with value 'new_value'. If the name
// 'name' existed in input parameters 'ip', its value is changed. If the name 'name' did not exist
// in input parameters 'ip', then it is added t input parameters along with new_value being its
// value.
// So this is either replace or add an input parameter.
// Returns 1 if input parameter 'name' existed (and value was replaced), or 2 if 'name' did not
// exist and name/value was added to input parameters.
//
int cld_replace_input_param (cld_input_params *ip, const char *name, const char *new_value)
{
    CLD_TRACE ("");
    assert (new_value);
    assert (ip);
    assert (name);

    int i;
    for (i = 0; i < ip->num_of_input_params; i++)
    {
        if (!strcmp (ip->names[i], name))
        {
            cld_copy_data (&(ip->values[i]), new_value);
            return 1;
        }
    }
    // param not there, add it
    ip->num_of_input_params++;
    ip->names = cld_realloc (ip->names, sizeof(char*)*ip->num_of_input_params);
    ip->values = cld_realloc (ip->values, sizeof(char*)*ip->num_of_input_params);
    ip->names[ip->num_of_input_params - 1]  = cld_init_string (name);
    ip->values[ip->num_of_input_params - 1]  = cld_init_string (new_value);
    return 2;
}

// 
// In string src (which must have been allocated with cld_malloc!) replace string 'search' with
// 'subst_with'. We will allocate enough memory so that src has sufficient buffer space, if that's needed. 
// If all is 1, subst all, otherwise just the first occurrance.
// Returns the buffer length of src.
//
int cld_subst (char **src, const char *search, const char *subst_with, int all)
{
    CLD_TRACE("");
    int search_len = strlen (search);
    int subst_len = strlen (subst_with);
    int src_buf_len;
    
    //
    // Since src must have been allocated by cld_malloc, get the size of allocated buffer
    // If src was not allocated by cld_malloc, we will FAIL here.
    //
    cld_check_memory(*src, &src_buf_len);

    //
    // Check if substitution string is smaller that searched-for string. If so, replacement of string
    // MUST succeed since we DO have enough space to do it.
    // `
    if (search_len >= subst_len)
    {
        if (cld_replace_string (*src, src_buf_len, search, subst_with, all, NULL) == -1)
        {
            cld_report_error ("Internal error [%s], [%s], [%s], [%d], [%d]", *src, search, subst_with, src_buf_len, all);
        }
        return src_buf_len;
    }

    //
    // Find out how may instances we need to replace, in order to know if we have enough buffer space.
    //
    int count_search;
    int new_len;
    if (all == 1)
    {
        count_search = cld_count_substring (*src, search);
        if (count_search == 0)
        {
            //
            // none found, just return !!
            //
            return src_buf_len;
        }
    }
    else
    {
        //
        // This is if caller asks for ONLY ONE replacement (or none if not found).
        //
        char *is_found = strstr (*src, search);
        if (is_found != NULL)
        {
            count_search = 1;
        }
        else
        {
            //
            // none found, just return !!
            //
            return src_buf_len;
        }
    }

    //
    // Reallocate to fit: if the buffer was already big enough, this won't be expensive
    //
    *src = cld_realloc (*src, new_len = src_buf_len + count_search*(subst_len - search_len));

    //
    // Replace string, we now have enough buffer, it MUST succeed
    //
    if (cld_replace_string (*src, new_len, search, subst_with, all, NULL) == -1)
    {
        cld_report_error ("Internal error [%s], [%s], [%s], [%d], [%d]", *src, search, subst_with, new_len, all);
    }

    return new_len;
}


// 
// Read config file and get fields from it (and also set other parameters): 
// . version (global version of some URL, excluding binary documents, used in upgrading files such as CSS), 
// . log_directory (tracing), 
// . html_directory (where html static files are), 
// . max_upload size (maximum upload size for binary documents), 
// . uparams (any parameters starting with underscore _), 
// . web (web address of the server up to and excluding question mark ?), 
// . email (emaill address used to send emails), 
// . file_directory (where uploads and other documents are held), 
// . tmp_directory (directory for temporary files), 
// . db (location of file containing database credentials), 
// . sock (location of database server connection file). 
// . ignore_mismatch - if yes, then ignore the mismatch of libraries (cld installed vs application built with)
// Out of these file, the ones that are not coded in config (i.e. they are fixed) are html_directory (always html), file_directory (always file), tmp_directory (always tmp),
// log_directory (always trace), db file (always .db). Out of config parameters (those actually in config file), sock, ignore_mismatch, and  max_upload_size have default value and can be omitted.
// version MUST be specified. 
// max_upload_size default is 5 million bytes, and sock default value is /var/lib/mysql/mysql.sock (which is correct often and does not need be changed).
//
// Returns 0 if cannot open config file or cannot figure out home directory, 1 if okay.
//
int cld_get_runtime_options(const char **version, const char **log_directory, const char **html_directory, long *max_upload_size, cld_store_data *uparams, const char **web, const char **email, const char **file_directory, const char **tmp_directory, const char **db, const char **sock, const char **ignore_mismatch)
{
    FILE *f;

    char conf_name[512];

    snprintf (conf_name, sizeof (conf_name) , "%s/config", cld_home_dir());
    f = fopen (conf_name, "r");
    if (f == NULL) 
    {
        return 0;
    }

    // version IS mandatory, this controls rollout of the software. Its purpose is to change the URL when we want all files caches at client to be invalidated
    // because all our files (except dynamically generated from .v of course) are cached forever. This is the ONLY way to force them to change.
    *version=NULL;

    char line[512];
    cld_store_init (uparams);
    // db is not mandatory since not every app will use database
    
    // email not mandatory
    *email = "admin@localhost.localdomain";

    // these are preset, and cannot be changed
    *log_directory = cld_init_string ("~/" CLD_TRACE_DIR);
    cld_subst ((char**)log_directory, "~", cld_home_dir(), 0);

    *file_directory = cld_init_string ("~/file");
    cld_subst ((char**)file_directory, "~", cld_home_dir(), 0);

    *tmp_directory = cld_init_string ("~/tmp");
    cld_subst ((char**)tmp_directory, "~", cld_home_dir(), 0);

    *html_directory = cld_init_string ("~/html");
    cld_subst ((char**)html_directory, "~", cld_home_dir(), 0);

    *db = cld_init_string ("~/.db");
    cld_subst ((char**)db, "~", cld_home_dir(), 0);

    // these are mandatory
    *web = NULL;

    // max_upload_size not mandatory
    *max_upload_size = 5000000;
    // mariadb_socket not mandatory since not every app will use database
    *sock = "/var/lib/mysql/mysql.sock";
    // by default do NOT ignore mismatch
    *ignore_mismatch="no";

    while (1)
    {
        if (fgets (line, sizeof (line) - 1, f) != NULL)
        {
            int len = strlen (line);
            cld_trim (line, &len);
            if (line[0] == '/' && line[1] == '/') continue; //comment line

            char *eq = strchr (line, '=');
            if (eq == NULL) continue; // bad line or empty line

            // divide line into name, value
            *eq = 0;
            len = strlen (line);
            cld_trim (line, &len); // name

            len = strlen (eq + 1);
            cld_trim (eq + 1, &len); // value
            
            // now line is the option, eq+1 is the value
            if (!strcasecmp (line, "VERSION"))
            {
                *version = cld_strdup (eq + 1);
            }
            else if (!strcasecmp (line, "MAX_UPLOAD_SIZE"))
            {
                *max_upload_size  = atol (eq + 1);
                long upper_limit = 1024*1024*1024;
                if (*max_upload_size <1024 || *max_upload_size > upper_limit)
                {
                    cld_report_error( "Max_upload_size in 'config' configuration file must be a number between 1024 and %ld", upper_limit);
                }
            }
            else if (!strcasecmp (line, "EMAIL_ADDRESS"))
            {
                *email = cld_strdup(eq + 1);
            }
            else if (!strcasecmp (line, "WEB_ADDRESS"))
            {
                // append /go.service to web address
                char *web_exe = cld_init_string (eq+1);
                cld_append_string ("/go.", &web_exe);
                cld_append_string (cld_handler_name, &web_exe);
                *web = (const char*)web_exe;
            }
            else if (!strcasecmp (line, "IGNORE_MISMATCH"))
            {
                *ignore_mismatch = cld_strdup (eq + 1);
            }
            else if (!strcasecmp (line, "MARIADB_SOCKET"))
            {
                *sock = cld_strdup (eq + 1);
            }
            else if (line[0] == '_') // custom parameter, for user purposes
            {
                const char *custom_line = cld_init_string (eq+1);
                cld_subst ((char**)&custom_line, "~", cld_home_dir(), 1);
                cld_store (uparams, line, custom_line);
            }
        }
        else break; // either eof or error, either way
                // we won't handle or report, because there is no 
                // venue yet to report it in, we're opening those now
    }
    if (*version == NULL) cld_report_error ("version parameter not specified in configuration file [%s]", conf_name);
    if (*web == NULL) cld_report_error ("web parameter not specified in configuration file [%s]", conf_name);

    fclose (f);
    return 1;
}




// 
// Initialize URL response (response received in cld_post_url_with_response(), i.e. when
// calling web service somewhere). 's' is URL response.
//
void cld_init_url_response(cld_url_response *s) 
{
    CLD_TRACE("");
    s->len = 0;
    s->ptr = cld_malloc(s->len+1);
    s->ptr[0] = '\0';
}

// 
// Write url response data to url response 's'. 'ptr' is the data received as a response from a 
// web call in cld_post_url_with_response(), 'size'*'nmemb' is ptr's length. This is curl's callback
// function where pieces of response are channeled here, so this function by nature is cumulative.
// The signature of this function stems of the default curl's handler, which is a write() into a file.
// Returns the number of bytes "written" into our buffer, and that must match the number of bytes
// passed to it.
//
size_t cld_write_url_response(void *ptr, size_t size, size_t nmemb, cld_url_response *s)
{
    CLD_TRACE("");
    // new len (so far plus what's incoming)
    size_t new_len = s->len + size*nmemb;

    // reallocate buffer to hold new len (the input from curl doesn't include zero byte, so +1)
    s->ptr = cld_realloc(s->ptr, new_len+1);

    // append incoming data after current input
    memcpy(s->ptr+s->len, ptr, size*nmemb);

    // zero terminate
    s->ptr[new_len] = '\0';

    // set new len
    s->len = new_len;


    return size*nmemb;
}


// 
// Issue web call, which is url string, and obtain the response in 'result' which will be allocated here.
// If there is an SSL certificate (authority for this system having https capability for instance), it is 
// specified in 'cert'. Any error is reported in 'error'.
// 'cookiejar' is the file name (full path) of where the cookies are to be stored. Cookies are read from here before
// the call and store here after the call. When communicating with the same server, cookiejar serves the purpose of
// holding all known cookies.
// Returns 1 if okay, 0 if error.
// Redirections in response are handled with up to 5 at a time.
//
int cld_post_url_with_response(const char *url, char **result, char **error, const char *cert, const char *cookiejar)
{
    CLD_TRACE("URL posted [%s]", url);
    CURL *curl;
    CURLcode res;

    // this static is okay because EVERY return sets it to zero. We could do tries-- with each return and 
    // technically by the time we get back to the first call (in a series of recursive calls), it should be zero
    // again, but this way we're sure. See code below, we don't do anything with the result other than to pass it up,
    // so the unwind of recursive calls happens without any further action.
    // Also this static is for a single request only.
    static int tries = 0;
    assert (url != NULL);
    assert (result != NULL);

    if (error != NULL && *error != NULL) *error = CLD_EMPTY_STRING;

    // keep track of the depth of recursive calls to this function
    // EVERY RETURN FROM THIS FUNCTION MUST HAVE TRIES-- to keep this correct.
    tries++;
    if (tries>=5)
    {
        tries = 0;
        if (error != NULL) *error = cld_strdup ("Too many redirections in URL");
        return 0; // too many redirections followed, error out
    }

    curl = curl_easy_init();
    if (curl) 
    {
        cld_url_response s;
        cld_init_url_response (&s);

        if (cookiejar!=NULL)
        {
            // the same file is used to read cookies and to write them.
            // CURL will keep this file the same way a browser would do. Cookies
            // can be received from server in one call and then read in another even
            // if they weren't sent in that call. 
            curl_easy_setopt(curl, CURLOPT_COOKIEFILE, cookiejar); 
            curl_easy_setopt(curl, CURLOPT_COOKIEJAR, cookiejar);
        }

        if (cert == NULL)
        {
            curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0);
            curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0);
            // this is with-no-cert
        }
        else
        {
            curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 1);
            curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1);
            if (cert[0] == 0)
            {
                // this is default CA authority, installed on system
            }
            else
            {
                // this is with-cert <location> where cert is this location
                curl_easy_setopt(curl, CURLOPT_CAINFO, cert);
            }
        }
        curl_easy_setopt(curl, CURLOPT_URL, url);
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, cld_write_url_response);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &s);

        res = curl_easy_perform(curl);
        if(res != CURLE_OK)
        {
            if (error != NULL) *error = cld_strdup (curl_easy_strerror(res));
            tries = 0;
            return 0;
        }
        else
        {
            long response_code;
            res = curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &response_code);
            if((res == CURLE_OK) &&
                   ((response_code / 100) != 3)) 
            {
                /* a redirect implies a 3xx response code */ 
                // this is if NOT a redirection (3xx). The actual result is obtained ONLY from the actual data response and NOT from redirection.
                *result = s.ptr;
            }
            else 
            {
                char *location;
                res = curl_easy_getinfo(curl, CURLINFO_REDIRECT_URL, &location);
                                                              
                if((res == CURLE_OK) && location) 
                {
                      /* This is the new absolute URL that you could redirect to, even if
                       *            * the Location: response header may have been a relative URL. */ 
                      CLD_TRACE("Redirecting to [%s]", location);
                      cld_free (s.ptr); // free the old result
                      //
                      // Recursive call to this function is done so that its result is always immediately
                      // passed back to the caller, so that it is a clean winding and unwinding. There is no unwinding followed
                      // by winding followed by unwinding etc. There is only winding and then unwinding back to the original caller.
                      // So 'tries' is increased up to the last recursive call, and after that one returns without a recursion it goes
                      // back to the original one without any interruption. THat's why we can set tries to zero right away.
                      // So, when 'res' is obtained it MUST BE immediate passed back.
                      //
                      int res = cld_post_url_with_response(location, result, error, cert, cookiejar);
                      tries = 0;
                      return res;
                }
                else
                {
                    // no location? result is empty by default
                }
           }
        }

        curl_easy_cleanup(curl);
    }
    else 
    {
        if (error != NULL) *error = cld_strdup ("Cannot initialize URL library");
        tries = 0;
        return 0;
    }
    tries = 0;
    return 1;
}


// 
// Copy file src to file dst. 
// Returns -1 if cannot open source, -2 if cannot open destination, -3 if cannot read source,
// -4 if cannot write destination
// Uses 8K buffer to copy file.
//
int cld_copy_file (const char *src, const char *dst)
{
    CLD_TRACE("");

    int f_src = open(src, O_RDONLY);
    if (f_src < 0) return -1;
    int f_dst = open(dst, O_WRONLY|O_CREAT, S_IRWXU);
    if (f_dst < 0) 
    {
        CLD_TRACE ("Cannot open [%s] for writing, error [%s]", dst, strerror(errno));
        close (f_src);
        return -2;
    }
    char buf[8192];

    while (1) 
    {
        ssize_t res = read(f_src, &buf[0], sizeof(buf));
        if (res == 0) break;
        if (res < 0) 
        {
            CLD_TRACE ("Cannot read [%s], error [%s]", src, strerror(errno));
            close (f_src);
            close (f_dst);
            return -3;
        }
        ssize_t rwrite= write(f_dst, &buf[0], res);
        if (rwrite != res) 
        {
            CLD_TRACE ("Cannot write [%s], error [%s]", dst, strerror(errno));
            close(f_src);
            close(f_dst);
            return -4;
        }
    }
    close (f_src);
    close (f_dst);
    return 1;
}


// 
// Encode data in Base64. 'in' is the input, of length in_len. 'out' is allocated
// here and is the result of encoding, and the length of it is in out_len.
// Note: all b64 data must be produced on a single line, per openssl docs.
//
void cld_b64_encode(const char* in, size_t in_len, char** out, size_t* out_len)
{ 
    CLD_TRACE("");
    BIO *buff, *b64_filter;
    BUF_MEM *ptr;

    // start b64
    b64_filter = BIO_new(BIO_f_base64());
    buff = BIO_new(BIO_s_mem());
    buff = BIO_push(b64_filter, buff);

    // set flags
    BIO_set_flags(buff, BIO_FLAGS_BASE64_NO_NL);// all on the same line
    BIO_set_close(buff, BIO_CLOSE);

    // write originals
    BIO_write(buff, in, in_len);
    BIO_flush(buff);

    // get encoded
    BIO_get_mem_ptr(buff, &ptr);
    (*out_len) = (ptr->length);
    (*out) = (char *) cld_malloc(((*out_len) + 1));
    memcpy(*out, ptr->data, (*out_len));
    (*out)[(*out_len)] = '\0';

    // cleanup
    BIO_free_all(buff);
}
 
// 
// Decode Base64 data. 'in' is b64 data of length 'in_len', and the output is allocated
// as 'out' of length out_len.
// Note: all b64 data must be on a single line, per openssl docs.
//
void cld_b64_decode (const char* in, size_t in_len, char** out, size_t* out_len) 
{
    CLD_TRACE("");
    BIO *buff, *b64_filter;

    // start b65
    b64_filter = BIO_new(BIO_f_base64());

    // feed input
    buff = BIO_new_mem_buf((void *)in, in_len);
    buff = BIO_push(b64_filter, buff);
    (*out) = (char *) malloc(in_len * sizeof(char));

    // flags
    BIO_set_flags(buff, BIO_FLAGS_BASE64_NO_NL);
    BIO_set_close(buff, BIO_CLOSE);

    // read decoded
    (*out_len) = BIO_read(buff, (*out), in_len);
    (*out) = (char *) cld_realloc((void *)(*out), ((*out_len) + 1));
    (*out)[(*out_len)] = '\0';

    // cleanup
    BIO_free_all(buff);
}


// 
// Send email. 'sendmail' is used to do this. 'from' is the sender email. 'to' is the recipients' email(s).
// 'subject' is the subject and 'message' is the message.
// 'headers' is any additional headers, such as content-type etc. It can be NULL or empty string ("") if just plain text email is sent.
// Returns the exit status of sendmail.
// Cloudgizer uses postfix as MTA (mail transport agent), and postfix's own sendmail interface.
int cld_sendmail(const char *from, const char *to, const char *subject, const char *headers, const char *message)
{
    assert(from);
    assert(to);
    assert(subject);
    assert(message);

    CLD_TRACE("");
    const char *prg = CLD_MAILPROGRAM;
    //
    // Require recipients extracted from command line (-t), and specify bounce email address (which is 
    // email from config file). Don't treat dot (.) as the end of input (rather end-of-input is it).
    //
    const char *argv[] = {CLD_MAILPROGRAM_NAME, "-oi", "-t", "-f", cld_get_config()->app.email, NULL};

    // allocate enough space, will add 1000 for markups
    int mlen = strlen (message)+strlen(from)+strlen(to)+strlen(subject);
    int in_len;
    char *in = (char*)cld_malloc (in_len = mlen + 1000);


    //
    // If 'headers' specified and not empty, insert headers and then a new line, because we always need a
    // blank line before the body. If headers is empty or NULL, we will have a blank line before the body. So this way
    // we always have a blank line before the body, with or without additional headers.
    //
    int lw = snprintf (in, in_len , "To: %s\r\nFrom: %s\r\nSubject: %s\r\n%s%s\r\n%s\r\n", to, from, subject,
        headers==NULL || headers[0]==0 ? "":headers, 
        headers==NULL || headers[0]==0 ? "":"\r\n", 
        message);
    if (lw >= in_len-1)
    {
        // should not really happen, we allocated enough memory
        cld_report_error ("Mail message too large [%s], would have written [%d] bytes, available only [%d]", in, lw, in_len - 1);
    }

    // get some output from sendmail in case there is any (usually not)
    char ob[200];
    int st =cld_exec_program_with_input(prg, argv, 5, in, lw, ob, sizeof(ob));
    if (ob[0]!=0)
    {
        CLD_TRACE("Sendmail produced output [%s]", ob);
    }
    cld_free (in);
    return st;
}

void cld_exec_program (const char *program, int num_args, const char **program_args, int *status, char **program_output, int program_output_length)
{
    CLD_TRACE("");

    int i =0;
    int last_slash=-1;
    while (program[i]!=0)
    {
        if (program[i]=='/')
        {
            last_slash=i;
        }
        i++;
    }
    const char *program_name;
    if (last_slash==-1)
    {
        program_name=program;
    }
    else
    {
        program_name=program+last_slash;
    }
    program_args[0] = program_name;
    CLD_TRACE("Program name for execution is [%s]", program_args[0]);
    assert(program_output_length>0);
    *program_output=(char*)cld_malloc(program_output_length);
    *status = cld_exec_program_with_input(program, program_args, num_args, "", 0, *program_output, program_output_length-1);
}




// 
// Get base name of URL. If protocol is missing (such as http://), returns empty string.
// For example, for http://myserver.com/go.service?..., the base name is myserver.com
// Returns base name of 'url'.
//
char *cld_web_name(const char *url)
{
    CLD_TRACE("");
    char *prot = strstr(url,"://");
    if (prot==NULL) 
    {
        return "";
    }
    char *web_name=cld_strdup(prot+3);
    char *end = strchr (web_name,'/');
    if (end!=NULL)
    {
        *end = 0;
    }
    return web_name;
}


// 
// Output 'text' but first substitute all new lines with <br/>
//
void cld_print_web_show_newline (const char *text)
{
    CLD_TRACE("");
    char *nextbr=NULL;
    while (1)
    {
        nextbr=strchr (text,'\n');
        if (nextbr==NULL) 
        {
            cld_printf(CLD_WEB, "%s", text);
            break;
        }
        // can NOT do *nextbr=0, results in sigseg, because we
        // pass literal text to this function and C treats is
        // a constant that can NOT be changed. So we calculate
        // number of chars to print.
        int disp_len = nextbr-text;
        cld_printf(CLD_WEB, "%.*s", disp_len, text);
        cld_printf(CLD_NOENC, "<br/>");
        text=nextbr+1;
        if (text[0]==0) break;
    }
}



// 
// Break down 'value' string into pieces, with 'delim' being the delimiter.
// For example, 'x+y+z' could be 'value' and 'delim' could be '+'. 
// The result is stored into datatype cld_broken's variable 'broken'.
// This variable has 'num_pieces' as a number of values broken into, and 
// 'pieces[]' array that holds this number of pieces.
void cld_break_down (char *value, const char *delim, cld_broken *broken)
{
    CLD_TRACE("");

    char *found_delim;

    // setup memory for 128 pieces and expand later if necessary
#define MAX_BREAK_DOWN 128

    int tot_break = MAX_BREAK_DOWN;

    broken->pieces = (char**)cld_malloc(tot_break * sizeof(char*));

    int curr_break = 0;
    char *curr_value = value;
    int len_delim = strlen(delim);

    while (1)
    {
        found_delim = strstr (curr_value, delim);
        if (found_delim!=NULL) *found_delim = 0;

        // trim it without cld_trim because its faster this way - no copying at the front end
        int piece_len = 0;
        if (found_delim!=NULL) 
        {
            piece_len = found_delim-curr_value;
        }
        else
        {
            piece_len = strlen (curr_value);
        }
        // trim on the back
        while (piece_len>0 && isspace(*(curr_value+piece_len-1))) piece_len--;
        *(curr_value+piece_len) = 0;
        // trim on the front
        while (isspace(*curr_value)) curr_value++;

        // store value to pieces[] array
        (broken->pieces)[curr_break] = curr_value;

        // end when no delimiter found
        if (found_delim==NULL) 
            break;
        else
            curr_value = found_delim + len_delim; 

        // move forward, expand buffer if needed
        curr_break++;
        if (curr_break >= tot_break)
        {
            tot_break += MAX_BREAK_DOWN;
            broken->pieces = (char**) cld_realloc (broken->pieces, tot_break * sizeof(char*));
        }
    }
    broken->num_pieces = curr_break+1;
}


//
// Returns GMT time (now, in the future, or the past)
//
// Input parameter timezone is the name of TZ (timezone) variable to be set. So to get GMT time
// then timezone should be "GMT", if it is Mountain Standard then it is "MST" etc.
//
// Input parameters year,month,day,hour,min,sec are the time to add to current time (can be negative too).
// So for example ..(0,0,1,0,0,1) adds 1 day and 1 second to the current time, while
// .. (0,-1,0,0,0) is one month in the past.
//
// Returns GMT time suitable for many purposes, including cookie time (expires).
//
// The format of the return string is thus important and must NOT be changed.
// This time MUST be system time. Used ONLY for system purposes, such as
// cookie time, which must be system since system delivers Date in HTTP header
// to browser, so we MUST use system time in browser as well.
//
// This function will RESTORE the timezone back to what it was when the program first started. So it
// will temporarily set TZ to timezone variable, but before it exits, it will restore TZ to what it was.
//
char *cld_time (const char *timezone, int year, int month, int day, int hour, int min, int sec)
{
    CLD_TRACE ("");

    char set_gm[200];
    
    // set timezone to be used
    snprintf (set_gm, sizeof(set_gm), "TZ=%s", timezone);

    //make sure timezone is always GMT prior to using time function
    putenv(set_gm);
    tzset();

    // get absolute time in seconds
    time_t t = time(NULL);
    struct tm tm = *localtime(&t);
    struct tm future;       /* as in future date */

    // get future time
    future.tm_sec = tm.tm_sec+sec;
    future.tm_min = tm.tm_min+min;;
    future.tm_hour = tm.tm_hour+hour;
    future.tm_mday = tm.tm_mday+day;
    future.tm_mon = tm.tm_mon+month;
    future.tm_year = tm.tm_year+year; // years into the future 
    future.tm_isdst = -1;          /* try automaitic, may not work only within 1 hour before DST switch and 1 hour after*/

    // verify time is correct
    t = mktime( &future );
    if ( -1 == t )
    {
        //
        // Set result of cld_get_tz to mutable char *, since putenv does NOT 
        // modify its parameter. The result of cld_get_tz must NOT be modified.
        //
        putenv((char*)cld_get_tz());
        tzset();
        cld_report_error ("Error converting [%d-%d-%d] to time_t time since Epoch\n", future.tm_mon + 1, future.tm_mday, future.tm_year + 1900);
    }

    // convert time into GMT string suitable for cookies (this function is for
    // cookies ONLY or for anything that needs GMT time in this format)
#define GMT_BUFFER_SIZE 50
    char *buffer=(char*)cld_malloc(GMT_BUFFER_SIZE);
    size_t time_succ = strftime(buffer,GMT_BUFFER_SIZE-1, "%a, %d %b %Y %H:%M:%S GMT", &future);
    if (time_succ == 0)
    {
        cld_report_error ("Error in storing time to buffer, buffer is too small [%d]\n", GMT_BUFFER_SIZE);
    }
    
    // go back to default timezone. See above about casting cld_get_tz()
    // to (char*)
    putenv((char*)cld_get_tz());
    tzset();

    CLD_TRACE("Time is [%s]", buffer);
    return buffer;
}



// Get string representation of integer 'var' into 'data'.
// Returns bytes written.
// 'data' will be a new pointer to allocated data.
//
inline int cld_copy_data_from_int (char **data, int val)
{
    CLD_TRACE ("");
    char n[30];
    snprintf (n, sizeof (n), "%d", val);
    return cld_copy_data (data, n);
}


//
// Return application name
// 
const char *cld_app_name ()
{
    return cld_handler_name;
}
