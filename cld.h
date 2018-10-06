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
// Include file for CLD run time
//

#ifndef _CLD_INC

#define _CLD_INC

// needed for crash handling (obtaining source file name and line numbers)
#ifndef _GNU_SOURCE
#define _GNU_SOURCE 
#endif


// Version. We use major plus minor only, as in 1.3,2.1,3.7... Release notes from N to N+1
// should state the differences. 
#define CLD_MAJOR_VERSION "1.2"

// database (MariaDB) related
#include <mysql.h>
#include <mysql/errmsg.h>
#include <assert.h>
#include <mysqld_error.h>


// Includes
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <assert.h>
#include <time.h>
#include <stdarg.h>
#include <errno.h>
#include <ctype.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <pwd.h>
#include <limits.h>
#include <fcntl.h>
// Hash/encryption
#include "openssl/sha.h"
#include "openssl/evp.h"
#include "openssl/aes.h"
#include "openssl/bio.h"
#include "openssl/buffer.h"
// Web calls
#include <curl/curl.h>
#include <stdint.h>


// 
// Globals
//
// Empty string. Its own value is the pointer itself, which is used
// as a differentiator between allocated and un-allocated strings.
// A string that points to 'CLD_EMPTY_STRING' is not allocated, and cannot be
// realloc-ed, otherwise it can.
//
extern char *CLD_EMPTY_STRING;


// 
// Defines
// 
#define CTX (cld_get_config()->ctx) // context of execution
#define CLD_DEFINE_STRING(x) char *x = CLD_EMPTY_STRING // define string as empty for use with cld_malloc etc
#define CLD_INIT_STRING(x) x = CLD_EMPTY_STRING // initialize existing string as empty for use with cld_malloc etc
#define CLD_SALT_LEN 8 // length of Cloudgizer salt used in encryption/decryption
#define CLD_TRACE_DIR "trace" // the name of trace directory is always 'trace'
// since we're only supporting Centos 7+, this is what comes with it, or if it's not there, user needs to install
// When we go to Centos 8, the code for Centos 7 remains in its own branch, and we set something else here for 8
// (that is, if the default changes)
#define CLD_MAILPROGRAM "/usr/lib/sendmail"
#define CLD_MAILPROGRAM_NAME "sendmail"
// for cld_<memory handling> inline or not?  Currently not, otherwise would be 'inline' instead of empty
#define CLD_TRACE_LEN 12000 // max length of line in trace file and max length of line in verbose output of 
#define CLD_FATAL_HANDLER(e) cld_fatal_error(e, __FILE__, __LINE__) // fatal handler, when all else fails
#define CLD_MAX_NESTED_WRITE_STRING 5 // max # of nests of write-string
// max # of custom header a programmer can add to custom reply when replying with a file
#define CLD_MAX_HTTP_HEADER 16
#define CLD_SECURITY_FIELD_LEN 80 // max length of a field in .db file (such as user name, password, db name etc)
#define CLD_MAX_SQL_SIZE 32000 // max possible size of SQL statement in CLD
#define CLD_PRINTF_ADD_LEN (32*1024) // this is for single cld_printf/cld_puts calls, chunks in which output buffer is increased 
#define CLD_PRINTF_MAX_LEN (128*1024) /* max length of printing to buffer before flushing, this MUST BE GREATER than CLD_PRINTF_ADD_LEN by more than 2x
                                so we don't flush after only one buffer*/
#define CLD_DEBUGFILE "debug" // the name of debug file in trace directory is always 'debug'
#define CLD_MAX_SIZE_OF_URL 32000 /* maximum length of browser url (get) */
#define CLD_MAX_ERR_LEN 12000 /* maximum error length in report error */
#define CLD_MAX_FILES_PER_UPLOAD_DIR  30000 /* files per directory in file directory */
#define CLD_ERROR_EXIT_CODE 99 // exit code of command line program when it hits any error
// constants for encoding
#define CLD_URL 1
#define CLD_WEB 2
#define CLD_NOENC 3
// constants for generation of queries' code (use no result, create empty result, normal execution)
// these constants are used in generated SQL code by CLD
#define CLD_QRY_USE_EMPTY 1
#define CLD_QRY_CREATE_EMPTY 2
#define CLD_QRY_NORMAL 0
// Max cookies per request and max length of a single cookie
#define CLD_MAX_COOKIES 256
#define CLD_MAX_COOKIE_SIZE 2048
#define CLD_TIME_LEN 200 // max length of time strings
// maximum number of bytes needed to encode either URL or WEB encoded string
#define CLD_MAX_ENC_BLOWUP(x) ((x)*6+1)
#define CLD_MAX_QUERY_OUTPUTS 1000 // maximum # of output parameters for each query, per query, essentially # of result columns


// 
// Data type definitions
//

// Response for web call. Chunked responses are added to the string ptr determined by total dynamic length of len.
//
typedef struct cld_s_url_response {
    char *ptr;
    size_t len;
} cld_url_response;
// 
// Debug information obtained from trace/debug file
//
typedef struct s_debug_app
{
    int memory_check; // if 1, perform memory check with each CLD_TRACE. Trace does NOT have to be enabled.
    int trace_level; // trace level, currently 0 (no trace) or 1 (trace)
    int trace_size;  // # of stack items in stack dump after the crash (obtained at crash from backtrace())
    int lint; // to lint or not to lint XHTML dynamic output
    char *tag; // tag used for ... anything at all
    int sleep; // # of seconds to sleep on startup BEFORE getting the input parameter and processing request
} debug_app;
// 
// Name/value pair for sequential list API
//
typedef struct cld_store_data_item_s
{
    char *data;
    char *name;
} cld_store_data_item;
// 
// Information needed to traverse and rewide the sequential list API, plus the array of items itself
typedef struct cld_store_data_s
{
    cld_store_data_item *item; // array of items
    int num_of; // # of items
    int store_ptr; // end of list
    int retrieve_ptr; // where to get next one
} cld_store_data;
// 
// Configuration context data for application, read from config file. Does not change during a request.
//
typedef struct s_app_data
{
    const char *version; // version of software online (used to invalidate client caches via changing URL)
    const char *db; // location of 0600 permission (outside of web document root) file with db credentials
    const char *log_directory; // directory for tracing
    const char *tmp_directory; // directory for temporary files
    const char *file_directory; // directory for uploads
    const char *html_directory; // directory for documents used by web pages (html, css, images, etc)
    const char *email; // application catch-all email
    const char *web; // web site URL for app
    long max_upload_size; // maximum upload size for any file
    const char *mariadb_socket; // path to mariadb server socket file, typically /var/lib/mysql/mysql.sock
    const char *ignore_mismatch; // yes or no from config file, to ignore or not version mismatch of cld library
    cld_store_data user_params; // user parameters from XXXXXX.conf (those starting with _)
} app_data;
// 
// Run-time information for tracing
//
typedef struct s_conf_trace
{
    int in_memory_check; // if 1, the caller is checking memory which originated from previous memory checking
    int in_trace; // if 1, the caller that is attempting to use tracing function which originated in tracing code
    FILE *f; // file used for tracing file, located in trace directory
    char fname[300]; // name of trace file
    char time[CLD_TIME_LEN + 1]; // time of last tracing
} conf_trace;
// 
// The buffer for outputting. This includes string writing (such as write-string) and any web output (such as
// outputting HTML code).
typedef struct s_out_HTML
{
     char *buf; // output buffer to hold html
     int len; // length  of buffer currently allocated
     int buf_pos; // current # of bytes in buff, MINUS the zero byte at the end
} out_HTML;
// 
// Input parameters from a request (URL input parameters or POST without uploads).
//
typedef struct s_cld_input_params
{
    const char **names; // URL names for GET/POST request
    char **values; // URL values for GET/POST request
    int num_of_input_params; // # of name/values in GET/POST request
} cld_input_params;
// 
// Write string (write-string markup) information
typedef struct write_string_t
{
    char **string; // Actual data. If not-NULL, cld_flush (printf etc) goes to a string
    int len; // length of written-to-string
    int is_end_write; // 1 if we did end-write-string just now, or 0 otherwise (during write)
} write_string;
// 
// Cookies. Array of cookies from input requests and any changes.
//
typedef struct cld_cookies_s
{
    // These are cookies as received from the client mixed with cookies set by the program
    char *data; // cookie string
    char is_set_by_program; // if 1, this cookie has been changed (deleted, or added)
} cld_cookies;
// header structure to send back file to a web client
typedef struct s_cld_header
{
    const char *ctype; // content type
    const char *disp; // header content disposition
    const char *file_name; // file name being sent
    const char *cache_control; // cache control http header
    int etag; // if 1,include etag which is the time stamp of last modification date of the file
    // the status_* are for status setting. status_id is the status id (such as 302) and status_text is it's corresponding text (such as '302 Found')
    // The example here is for redirection, but can be anything
    int status_id;
    const char *status_text;
    // the following are for generic http header of any kind, in fact content type, cache control etc. can all be done here while leaving others empty
    const char *control[CLD_MAX_HTTP_HEADER+1];
    const char *value[CLD_MAX_HTTP_HEADER+1];
} cld_header;
// 
// Input request. Overarching structure that contains much information not just about
// input request, but about current configuration, run-time state of the program.
typedef struct input_req_s
{
    app_data *app; // context, could be obtained as pc->app, but here is for convenience
    char *url; // original URL string
    int len_URL; // amount of memory allocated for 'url'
    int sent_header; // 1 if http header sent already
    char *if_none_match; // IF_NONE_MATCH from HTTP request
    int disable_output; // if 1, HTML output is disabled (but binary files or custom output of any kind can be done)
    write_string write_string_arr[CLD_MAX_NESTED_WRITE_STRING]; // holds a stack for write-string
    int curr_write_to_string; // current write-to-string nest level from 0 to CLD_MAX_NESTED_WRITE_STRING-1
    int bin_done; // if 1, output of binary file is done
    int exit_code; // exit code for command line program
    // cookies
    cld_cookies *cookies;
    int num_of_cookies;
    cld_input_params ip; // URL input params
    char *referring_url; // where we came from 
    int from_here; // did the current request come from this web server? 0 if not, 1 if yes.
    void *data; // global data - it can be used any way you like in application code
    int is_shut; // 1 if cld_shut already called
    cld_header *header; // if NULL, do nothing (no custom headers), if not-NULL use it to set custom headers
} input_req;
// 
// Context of execution. Contains inut request, apache web server structure, flags
//
// We use oops/file_too_large (and maybe others in the future) as set at runtime (see below).
//
typedef void (*oops_ptr)(input_req *,const char *);
typedef void (*file_too_large_ptr)(input_req *, int);
typedef struct s_context
{
    int trim_query_input; // if 1, URL input parameters (other than uploads) are trimmed
    input_req *req; // input request (see definition)
    void *apa; // apache structure (request_req * in apapche)
    int cld_report_error_is_in_report; // 1 if in progress of reporting an error 
    //
    // Handling of static variables in shared library:
    // 1. A static variable (i.e. in a function) can stay where it is if it is NOT initialized. Being uninitialized means this variable will get
    // its value during one request's lifetime, and it will be consumed as well (otherwise it would be a bug, and compiler is set to find usage of
    // uninitialized variables. This way its usage is clearly unrelated to cross-requests in the same process.
    // 
    // 2. A local static variable must be handled if it is initialized. 
    // 2.1 If the value of this variable is eternal across all modules (i.e. doesn't change
    // in the process ever regardless of which module is currently running in it), then it can stay as it, because it's value is always
    // correct. For example server timezone flag stays set forever in the process because timezone is the same for all modules.
    // 2.2 If the value of this variable changes depending on the module  then in MUST be handled. Most likely it means it will be
    // converted into a variable that is defined IN THE APPLICATION CODE (i.e. generated in cld.c) and then we defined a pointer here, and this pointer
    // is then assigned the address of variable defined in the application code (again, generated in cld.c) - this is all done at process startup automatically
    // by the code we generated. This way, such static variable is under visibility=hidden in application, so each module has its own copy, and common code here 
    // operates on a different variable with each request, as it should. There could be other ways to refactor this variable, but this is the most common
    // one. An example of this is g_con in db substructure within this context structure here.
    // 2.3 The exception to having to handle static variable that's initialized is if the program WILL stop after this request. Because program has to
    // restart afterwards, all static variables (in all shared libraries and the program) will be re-initialized when it restarts. In shared libraries, code is shared, but
    // data is NOT. Each program has its own copy of static variables in any shared library. On restart, all static variables of all shared libraries are initialized, 
    // as soon as they are needed or sooner.
    // 2.4 Another exception may be localized use of static variable such as in recursive function. If handled properly, this static variable is re-initialized when
    // such function is done.
    //

    //
    // Database module uses these. They are however  defined as hidden visibility variables in CLD application,
    // and each application sets these below to point to such hidden variables. This way, each module KEEPS connection
    // between requests, EVEN if the same process (which houses this module and other modules) switches between modules
    // for each requests.
    // We keep a static value in the application, and application sets the pointer below to this application static value.
    //
    // In the comments below 'static' variable means 'logically' static, i.e. it persists to some extent in the process. These used to be actual
    // static variable in earlier code versions.
    //
    struct db_s
    {
        MYSQL **g_con; // connection to db - persists in one process for many requests (no dropping/reconnection)
        int *is_begin_transaction; // are we in transaction in this process
        int *has_connected; // are we connected to db at this moment
    } db;


    //
    // User callbacks - must be set from module as pointer, so that dso can properly call modules's callback
    //
    struct callback_s
    {
        //
        // Oops and file_too_large (or any other functions from application) must be set as pointers
        // and called as such, b/c otherwise we might call other modules' oops for example!
        //
        oops_ptr oops_function;
        file_too_large_ptr file_too_large_function;
    } callback;


    //
    // Outputting of html data. 
    //
    struct out_s
    {
        // This isn't a pointer because this value is per-request only, i.e. it doesn't carry from request to request. THe
        // purpose of having it here is to make sure it's set to 0 for each new request.
        // Flag signifying if there was any html output. It must be reset to zero for each new request. 
        int was_there_any_output_this_request; // 0 if no html output yet in this request, 1 otherwise
    } out;
    
} context;
// 
// Configuration and run-time status. Config file, debug file, tracing, output buffers, context of request
//
typedef struct s_cld_config
{
    // these stay the same once set
    app_data app; // does not change during a request
    debug_app debug; // does not change during a request

    // these change during a request
    conf_trace trace; // tracing info
    out_HTML out; // output buffers (write-string and output of HTML)
    context ctx; // context of execution, not config, but convenient to
                // have it handy. That is why it's separate type. Changes at run-time
} cld_config;
//
// Type used  for getting data out of db from a result buffer. Iterator through columns and rows.
//
typedef struct s_cld_iter
{
    char **md;
    int rows;
    int cols; 
    int tot_rows;
    int tot_cols;
    int tot_item;
} cld_iter;
// 
// Structure for breaking up strings into pieces based on delimiters
//
typedef struct cld_broken_s
{
    char **pieces; // array of pieces
    int num_pieces; // #num of pieces in 'pieces'
} cld_broken;
//
// Information we collect about each shared library that is linked here
//
typedef struct s_so_info 
{
    // Module information, used in addr2line 
    void *mod_addr; // module start load address
    void *mod_end; // module end load address
    char mod_name[256]; // module name
} so_info;




// 
// Macros and function call related
//
#define CLD_UNUSED(x) (void)(x)
#define  CLD_TRACE(...) trace_cld(1, __FILE__, __LINE__, __FUNCTION__,  __VA_ARGS__)
#define  cld_report_error(...) {_cld_report_error(__VA_ARGS__);exit(0);}
#define cld_report_error_no_exit(...) _cld_report_error(__VA_ARGS__)
#define CLD_STRDUP(x, y) {const char *__temp = (y); (x) = cld_strdup (__temp == NULL ? "" : __temp); if ((x) == NULL) { cld_report_error("Out of memory");}}
#define CLD_CHAR_FROM_HEX(x) (((x)>'9') ? (((x)>='a') ? ((x)-'a'+10) : ((x)-'A'+10)) : ((x)-'0')) /* for conversion in URL - ASCII ONLY!
                        numbers are lower than capital letter are lower than lower case letters!! */
#define CLD_TO_HEX(x) ((x) <= 9 ? '0' + (x) : 'A' - 10 + (x))
#define CLD_HEX_FROM_BYTE(p,x) ((p)[0] = CLD_TO_HEX(((x)&0xF0)>>4), (p)[1] = CLD_TO_HEX((x)&0x0F))
#define CLD_MEMINLINE 
// The actual calls for memoryhandling
#define cld_malloc __cld_malloc
#define cld_realloc __cld_realloc
#define cld_free __cld_free
#define cld_strdup __cld_strdup
#define cld_calloc __cld_calloc


// 
//
// Function declarations
//
//
void cld_init_input_req (input_req *iu);
int cld_open_trace ();
void cld_close_trace();
char *cld_i2s (int i, char **s);
void cld_make_SQL (char *dest, int destSize, int num_of_params, const char *format, ...) __attribute__ ((format (printf, 4, 5)));
void cld_output_http_header(input_req *iu);
void cld_send_header(input_req *iu, int minimal);
void _cld_report_error (const char *format, ...) __attribute__ ((format (printf, 1, 2)));
int cld_encode (int enc_type, const char *v, char **res);
void cld_get_document_id (char *doc_id, int doc_id_len);
int cld_get_input(input_req *req, const char *method, const char *input);
char *cld_get_input_param (const input_req *iu, const char *name);
int cld_is_positive_int (const char *s);
int cld_exec_program_with_input(const char *cmd, const char *argv[], int num_args, const char *inp, int inp_len, char *out_buf, int out_len);
void cld_get_debug_options();
int lint();
int cld_save_HTML ();
int cld_flush_printf(int fin);
void cld_printf_close();
int cld_printf (int enc_type, const char *format, ...) __attribute__ ((format (printf, 2, 3)));
void cld_shut(input_req *giu);
void cld_cant_find_file (const char *reason);
int cld_exec_program_out_data (const char *cmd, const char *argv[], int num_args, char *buf, int buf_len);
int cld_exec_program_with_in_out (const char *prg, const char *argv[], int num_args, FILE *fin, FILE **fout);
FILE * cld_make_document (char **document_id, char *write_dir, int write_dir_len);
const char *cld_ctx_getenv (const char *var);
void cld_replace_all (int v, const char *look, const char *subst);
void cld_current_time (char *outstr, int out_str_len);
inline cld_config *cld_get_config();
void cld_fatal_error (const char *errtext, const char *fname, int lnum);
void cld_init_config(cld_config *pc);
void reset_cld_config(cld_config *pc);
int cld_count_substring (const char *str, const char *find);
int cld_replace_string (char *str, int strsize, const char *find, const char *subst, int all, char **last);
void cld_trim (char *str, int *len);
int cld_is_directory (const char *dir);
size_t cld_get_file_size(const char *fn);
void cld_memory_init ();
CLD_MEMINLINE void *__cld_malloc(size_t size);
CLD_MEMINLINE void *__cld_calloc(size_t nmemb, size_t size);
CLD_MEMINLINE void *__cld_realloc(void *ptr, size_t size);
CLD_MEMINLINE void __cld_free (void *ptr);
CLD_MEMINLINE char *__cld_strdup (const char *s);
void cld_done ();
void cld_get_stack(const char *fname);
MYSQL *cld_get_db_connection (const char *fname);
void cld_close_db_conn ();
void cld_begin_transaction();
int cld_commit();
int cld_rollback();
void cld_get_insert_id(char *val, int sizeVal);
int cld_DML (char *s, int *rows, unsigned int *er);
void cld_select_table (const char *s, int *nrow, int *ncol, char ***col_names, char ***data);
void cld_data_iterator_init (cld_iter *d, char **data, int rows, int cols);
char *cld_data_iterator_next (cld_iter *d, int *brk);
void cld_data_iterator_fill_array (char **data, int nrow, int ncol, char ****arr);
void cld_get_dml_row (char ****arr, int rowcount, unsigned int err, const char *insert_id);
void cld_get_empty_row (char ****arr, int ncol);
void trace_cld(int trace_level, const char *fromFile, int fromLine, const char *fromFun, const char *format, ...)
    __attribute__((format(printf, 5, 6)));
int cld_get_credentials(char* host, char* name, char* passwd, char* db, const char *fname);
char *cld_sha( const char *val );
int cld_ws_util_read (void * rp, char *content, int len);
const char *cld_ws_get_env(void * vmr, const char *n);
void cld_ws_set_content_type(void *rp, const char *v);
void cld_ws_set_content_length(void *rp, const char *v);
void cld_ws_set_header (void *rp, const char *n, const char *v);
void cld_ws_add_header (void *rp, const char *n, const char *v);
void cld_ws_send_header (void *rp);
int cld_ws_write (void *r, const char *s, int nbyte);
int cld_ws_flush (void *r);
void cld_ws_finish (void *rp);
int cld_main (void *r);
void cld_ws_set_status (void *rp, int st, const char *line);
int cld_ws_printf (void *r, const char *fmt, ...) __attribute__ ((format (printf, 2, 3)));
const char *cld_ws_get_status (void *rp, int *status);
void posix_print_stack_trace();
int cld_is_disabled_output();
void cld_enable_output();
void cld_disable_output();
void cld_set_crash_handler(const char *dir);
void cld_handle_request();
inline int cld_copy_data (char **data, const char *value);
const char *cld_web_address ();
int cld_puts_final (const char *final_out, int final_len);
inline char *cld_init_string(const char *s);
int cld_puts (int enc_type, const char *s);
inline int cld_copy_data_at_offset (char **data, int off, const char *value);
int cld_is_valid_param_name (const char *name);
void cld_write_to_string (char **str);
int cld_write_to_string_length ();
int cld_check_memory(void *ptr, int *sz);
int _cld_check_memory(void *ptr);
void cld_set_cookie (input_req *req, const char *cookie_name, const char *cookie_value, const char *path, const char *expires);
char *cld_find_cookie (input_req *req, const char *cookie_name, int *ind, char **path, char **exp);
int cld_delete_cookie (input_req *req, char *cookie_name);
int cld_decode (int enc_type, char *v);
inline char *cld_lower(char *s);
inline char *cld_upper(char *s);
void cld_location (char **fname, int *lnum, int set);
void cld_store_init (cld_store_data *fdata);
void cld_store (cld_store_data *fdata, const char *name, const char *data);
void cld_retrieve (cld_store_data *fdata, char **name, char **data);
void cld_rewind (cld_store_data *fdata);
void cld_purge (cld_store_data *fdata);
int cld_get_enc_key(const char *password, const char *salt, EVP_CIPHER_CTX *e_ctx, EVP_CIPHER_CTX *d_ctx);
char *cld_aes_encrypt(EVP_CIPHER_CTX *e, const unsigned char *plaintext, int *len, int is_binary);
char *cld_aes_decrypt(EVP_CIPHER_CTX *e, unsigned char *ciphertext, int *len, int is_binary);
void cld_enable_batch_processing();
void cld_set_exit_code(int ec);
int cld_lockfile(const char *filepath, int *lock_fd);
void cld_get_input_params (input_req *req, cld_input_params *ip );
char *cld_construct_url (cld_input_params *ip);
inline void cld_append_string (const char *from, char **to);
int cld_replace_input_param (cld_input_params *ip, const char *name, const char *new_value);
int cld_get_runtime_options(const char **version, const char **log_directory, const char **html_directory, long *max_upload_size, cld_store_data *uparams, const char **web, const char **email, const char **file_directory, const char **tmp_directory, const char **db, const char **sock, const char **ignore_mismatch);
inline const char * cld_major_version();
inline int cld_minor_version();
inline int cld_patch_version();
void cld_out_file (const char *fname, cld_header *header);
void cld_strncpy(char *dest, const char *src, int max_len);
int cld_subst (char **src, const char *search, const char *subst_with, int all);
inline int cld_getpid ();
char *cld_construct_input_params (cld_input_params *ip);
int cld_post_url_with_response(const char *url, char **result, char **error, const char *cert, const char *cookiejar);
int cld_copy_file (const char *src, const char *dst);
void cld_b64_decode (const char* in, size_t in_len, char** out, size_t* out_len);
void cld_b64_encode(const char* in, size_t in_len, char** out, size_t* out_len);
int cld_read_whole_file (const char *name, char **data);
int cld_is_number (const char *s, int *prec, int *scale, int *positive);
inline void cld_clear_config();
int cld_sendmail(const char *from, const char *to, const char *subject, const char *headers, const char *message);
void cld_init_header (cld_header *header);
int cld_write_file (const char *file_name, const char *content, size_t content_len, int append);
char *cld_home_dir ();
char *cld_web_name(const char *url);
void cld_print_web_show_newline (const char *text);
int cld_check_transaction(int check_mode);
void cld_break_down (char *value, const char *delim, cld_broken *broken);
const char * cld_get_tz ();
int cld_execute_SQL (const char *s,  int *rows, unsigned int *er, const char **err_message);
char *cld_time (const char *timezone, int year, int month, int day, int hour, int min, int sec);
void cld_exec_program (const char *program, int num_args, const char **program_args, int *status, char **program_output, int program_output_length);
int cld_encode_base (int enc_type, const char *v, int vLen, char **res, int allocate_new);
void cld_make_random (char *rnd, int rnd_len);
void cld_forbidden (const char *reason, const char *detail);
void cld_lint_text(const char *html);
void cld_checkmem ();
inline int cld_copy_data_from_int (char **data, int val);
void file_too_large(input_req *iu, int max_size);
void oops(input_req *iu, const char *err);
int cld_total_so(so_info **sos);


// Application name for apache purposes 
extern char *cld_handler_name;

#endif

