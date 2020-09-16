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
// Main CLD processor. Takes input and output parameters from the command line
// and generates C file ready for compilation.
//

#include "cld.h"


// 
//
// Defines (most of them)
//
//

// Maximum number of SQL queries in a single file, otherwise unlimited in application
// Queries's code is generated based on the name of the query, and not the index into an array.
// This limit is for internal code generation purposes.
#define CLD_MAX_QUERY 300
// Max number of query fragments in a single file, otherwise unlimited in application
#define CLD_MAX_QUERY_FRAGMENTS 200
// Max number of query shards in a single file, otherwise unlimited in application
#define CLD_MAX_QUERY_SHARDS 200
// level of queries to allow for nesting (i.e. query inside a query)
#define CLD_MAX_QUERY_NESTED 10
// maximum # of input parameters, meaning <?..?> or '%s' in a query, per query
#define CLD_MAX_QUERY_INPUTS 200
// maximum length of input parameter, be it C code, or a string. This is the length of what goes in the 
// SQL query input, per input.
#define CLD_MAX_QUERY_INPUT_LEN 250
// maximum length of input line in a  source code file.
#define CLD_FILE_LINE_LEN 8096
// maximum space to write out all output columns of a query
#define CLD_TOT_COLNAMES_LEN (CLD_MAX_COLNAME_LEN * CLD_MAX_QUERY_OUTPUTS)
// maximym length of query name, which is the name in define-query#...=
#define CLD_MAX_QUERYNAME_LEN 200
// maximum length of any error this utility may produce
#define CLD_MAX_ERR_LEN 12000
// various keywords used. What's recognized is often preceded or followed with a space or has space on both sides
// depending on whether something is expected to precede, follow or both
// No whitespace but space can be used in commands!
#define CLD_KEYWITH " with "
#define CLD_KEYIN " in "
#define CLD_KEYAS " as "
#define CLD_KEYTO " to "
#define CLD_KEYSHOWNEWLINE " show-newline"
#define CLD_KEYSTATUS " status "
#define CLD_KEYFROM " from "
#define CLD_KEYAPPEND " append"
#define CLD_KEYLENGTH " length"
#define CLD_KEYWITHRESPONSE "with-response "
#define CLD_KEYWITHCERT "with-cert "
#define CLD_KEYCOOKIEJAR "cookie-jar "
// with-no-cert has NO space afterwards because it doesn't need anything to follow it
#define CLD_KEYWITHNOCERT "with-no-cert"
#define CLD_KEYWITHERROR "with-error "
#define CLD_KEYDEFINED "define "
#define CLD_KEY_EXPIRES " expires "
#define CLD_KEY_PATH " path "
#define CLD_KEYPROGRAMOUTPUT "program-output "
#define CLD_KEYPROGRAMOUTPUTLEN "program-output-length "
#define CLD_KEYPROGRAMARGS "program-args "
#define CLD_KEYPROGRAMSTATUS "program-status "
#define CLD_KEYSUBJECT "subject "
#define CLD_KEYHEADERS "headers "
#define CLD_KEYBODY "body "
// maximum length of generated code line (in .c file, final line)
#define CLD_MAX_CODE_LINE 4096
// error messages
#define CLD_NAME_INVALID "Name [%s] is not valid, must be a valid C identifier, reading file [%s] at line [%d]"
#define CLD_PARAM_USAGE "Note: input parameters are not parsed for C syntax; all quoted strings are considered parameters, and comma is always a parameter delimiter; if quotes or commas are used inside a parameter, please escape them with a '\\'. Also check if every <? is matched with corresponding ?>."
#define CLD_MSG_NESTED_QRY "Qry ID [%d] is nested too deep, maximum nesting of [%d], reading file [%s] at line [%d]"
#define CLD_MSG_SHARD "Syntax error, must be define-shard#name or define-soft-shard#name, found [%s]"
// cloudgizer ID stuff
#define TOOL "Cloudgizer"
#define TOOL_CMD "cld"
// maximum length of a column name in db select query result 
#define CLD_MAX_COLNAME_LEN 64
// pre-processing status of a qry, describes the way it is used in the code
#define CLD_QRY_USED 2
#define CLD_QRY_UNUSED 0
#define CLD_QRY_ACTIVE 1
// maximum number of input parameters to a program in exec-program
#define CLD_MAX_EXEC_PARAMS 256


//
//
// Type definitions
//
//

// 
// Query structure, describes define-query# and all that it entails
//
typedef struct cld_qry_info_t
{
    const char *text; // sql text of query
    const char *name; // name of query (markup name after # sign)

    // number of query inputs for each query in qry
    int qry_total_inputs;
    int is_qry_compact; // 1 if it has <?...?>
    char *compact_params[CLD_MAX_QUERY_INPUTS + 1]; // strdup'd values of input params from <?..?>

    int is_dynamic; // 1 if query is dynamic, 0 if not
    int is_prepared; // 1 if query-prepare was used (so don't prepare again
                    // as part of run-query)
    int is_DML; // 1 if this is UPDATE INSERT or DELETE
    int is_insert; // 1 if insert

    // number of, and qry outputs 
    int qry_total_outputs; 
    char *qry_outputs[CLD_MAX_QUERY_OUTPUTS + 1];
    
    // input parameters from URL bound to each query
    char qry_inputs[CLD_MAX_QUERY_INPUTS + 1][CLD_MAX_QUERY_INPUT_LEN + 1];
    int qry_is_input_str[CLD_MAX_QUERY_INPUTS + 1]; // 1 if this is a string literal as an input parameter

    // number of query inputs actually found as opposed to the number of <?..?> or '%s'
    int qry_found_total_inputs; 
} qry_info;
// 
// Context data for CLD preprocessor. Used to hold information
// during the preprocessing.
//
typedef struct cld_gen_ctx_s
{
    // list of db queries - actual SQL with %s for input params
    // from URL
    qry_info qry[CLD_MAX_QUERY + 1]; // used for generation code phase only

    int qry_active[CLD_MAX_QUERY + 1]; // the status of query at current line in source code
    int total_queries; // total number of queries that we have. 
    int total_write_string; // used to detect unclosed write-strings

    // when nesting, query IDs are stored in global_qry_stack, with
    // curr_qry_ptr pointing to one just above the deepest. Query ID
    // is the index into qry array defined above. Same Query ID cannot be nested
    // within itself - if needed, use the same query under two
    // different Query IDs.
    int curr_qry_ptr;
    int global_qry_stack[CLD_MAX_QUERY_NESTED + 1];

    int cmd_mode; // 1 if this is command line program and not within a web server
    const char *db; // name of the file with db credentials

} cld_gen_ctx;
// Query fragments type, name and text for each
typedef struct qry_fragments_s
{
    char *name;
    char *text;
} qry_fragments_t;
// Query fragments data
qry_fragments_t qry_fragments[CLD_MAX_QUERY_FRAGMENTS+1];
int tot_qry_fragments=0;
// Query shards type, name, text and the comparison mode (soft=ignore input parameters when comparing)
typedef struct qry_shards_s
{
    char *name;
    char *text;
    int soft_compare;
} qry_shards_t;

typedef struct qry_shards_s1
{
    char *name;
    char *text;
    int soft_compare;
} qry_shards_t1;


// 
//
// Global variables (and a few related macros)
//
//

// Query shards data
qry_shards_t qry_shards[CLD_MAX_QUERY_SHARDS+1];
int tot_qry_shards=0;
int die_now = 0; // used in cld run-time only for now to gracefully exit program, not used here (not yet,
                // and maybe not ever).
FILE *outf = NULL; // cld output file (.c generated file)
int usedCLD = 0; // 1 if cld markup is used on the line
int last_line_if_closed = 0; // line number of the last IF that has closed
int last_line_for_closed = 0; // line number of the last FOR that has closed
// setup variables to be able to report the location of unclosed IF
#define check_next_if {if (open_ifs==0) last_line_if_closed = lnum; open_ifs++;}
// setup variables to be able to report the location of unclosed FOR
#define check_next_for {if (open_for==0) last_line_for_closed = lnum; open_for++;}
int last_line_query_closed = 0; // line number of where the last query closed
// setup variables to report the location of unclosed query
#define check_next_query {if (open_queries==0) last_line_query_closed = lnum; open_queries++;}
int verbose = 0; // 1 if verbose output of preprocessing steps is displayed
int total_exec_programs=0; // enumerates instance of exec-program so generated argument array variables are unique
// Application name - we must know it in order to figure out .db config file
char *cld_handler_name="";
int is_verbatim = 0; // if 1, everything is printed out verbatim (for start-verbatim/end-verbatim)

//
//
// Function/macro declarations
//
//
void init_cld_gen_ctx (cld_gen_ctx *gen_ctx);
void cld_gen_c_code (cld_gen_ctx *gen_ctx, const char *file_name);
int initialize_query (cld_gen_ctx *gen_ctx, const char *name, const char *text, int tot_inputs);
void _cld_report_error (const char *format, ...)  __attribute__ ((format (printf, 1, 2)));
int recog_markup (char *cinp, int pos, char *opt, char **mtext, int *msize, int isLast, const char *fname, int lnum);
void get_col_info (cld_gen_ctx *gen_ctx, const char *tab, const char *col, char **out_max_len, char **out_numeric_precision, char **out_numeric_scale, char **out_data_type, const char *fname, int lnum);
int find_query (cld_gen_ctx *gen_ctx, const char *query_name);
int is_query_DML (cld_gen_ctx *gen_ctx, int qry_name, int *is_insert);
int find_before_quote (char *mtext, int msize, char *what);
void new_query (cld_gen_ctx *gen_ctx, const char *qry, char *qry_name, int lnum, const char *cname);
int get_num_of_cols (cld_gen_ctx *gen_ctx, int query_name, const char *fname, int lnum);
void describe_query (cld_gen_ctx *gen_ctx, int qry_name, const char *fname, int lnum);
int get_col_ID (cld_gen_ctx *gen_ctx, int qry_name, const char *column_out, const char *fname, int lnum);
void oprintf (const char *format, ...)  __attribute__ ((format (printf, 1, 2)));
char *find_unescaped_chars (char *start, char *chars);
void get_passed_whitespace (char **s);
void get_until_comma (char **s);
void get_until_whitespace (char **s);
void cld_allocate_query (cld_gen_ctx *gen_ctx, int query_id);
void end_query (cld_gen_ctx *gen_ctx, int *query_id, int *open_queries, int close_block, const char *file_name, int lnum);
void get_next_input_param (cld_gen_ctx *gen_ctx, int query_id, char **end_of_query, const char *file_name, int lnum);
void tfprintf (FILE *f, const char *format, ...)  __attribute__ ((format (printf, 2, 3)));
int terminal_width();
int try_DML (cld_gen_ctx *gen_ctx, int query_name, const char *fname, int lnum, char **err);
void add_query_shard(char *name, char *text, int soft, const char *fname, int lnum);
char *find_query_shard(char *name, int *soft, const char *fname, int lnum);
void remove_sql_params(char *sql, const char *fname, int lnum);
void add_query_fragment(char *name, char *text, const char *fname, int lnum);
char *find_query_fragment(char *name, const char *fname, int lnum);
void out_verbose(int cld_line, const char *format, ...);
void add_input_param (cld_gen_ctx *gen_ctx, int query_id, int is_inp_str, char *inp_par, const char *file_name, int lnum);
void carve_markup (char **markup, const char *markup_name, const char *keyword, int is_mandatory, int no_data, int can_be_defined, const char *fname, int lnum);
#define  CLD_VERBOSE(lnum,...) out_verbose(lnum,  __VA_ARGS__)
// 
// Close the line in code generation. At the end of each markup we generate a code to start "printing"
// (either to web output or stdout or string). At the beginning of each markup we generate the code to 
// STOP "printing", and this is it. It is encapsulated here in case we need to do something more with this
// in the future.
//
#define END_TEXT_LINE  oprintf("\");\n");
#define BEGIN_TEXT_LINE oprintf("cld_puts (CLD_NOENC, \""); // open the text line for free-text unencoded output
void parse_param_list (char **parse_list, cld_store_data *params, const char *file_name, int lnum);
void handle_quotes_in_input_param (char **inp_par, int *is_inp_str);
void is_opt_defined (char **option, int *is_defined,  const char *file_name, int lnum);
int get_query_id (cld_gen_ctx *gen_ctx, char *mtext, int msize, const char *file_name, int lnum, int *is_defined, char **asvar);

//
//
// Implementation of functions used in CLD alone
//
//

//
// Get value of option in markup. For example, if markup is 
// send-mail from "x@y.com" to "z@w.com" ...
// then you can get "x@y.com" by doing this:
// 
// char *from = NULL;
// carve_markup (&from, "send-mail", CLD_KEYFROM, 1, 1, 0, file_name, lnum);
//
// where variable 'from' will be "x@y.com", '1' as 'is_mandatory' parameter means this parameter MUST be present, and fname and lnum
// indicate where we're at in the parsing process (file name and line number).
// 'send-mail' is the name of top markup, and CLD_KEYFROM is "from", and we're parsing out the data after it.
// NOTE that 'from' MUST point to actual "x@y.com" within original send-mail string. This means ALL options must be first
// found with strstr() before calling this function for any of them.
// This function MUST be called for ALL markup options - if not then some markup values will contain other markup and will be 
// INCORRECT. 
// 'has_data' is 0 if the option is alone without data, for example 'no-cert'. Typically it's 1.
// 'can_be_defined' is 1 if the option can be defined as string. This is typically for a resulting option that's a string. Mostly it's 0.
//
void carve_markup (char **markup, const char *markup_name, const char *keyword, int is_mandatory, int has_data, int can_be_defined, const char *fname, int lnum)
{
    //
    // *markup is the result of strstr(mtext, keyword) - and these MUST
    // be done for ALL options prior to calling this function
    //
    if (*markup != NULL) 
    {
        char *end_of_url = *markup;
        // advance past the keyword
        *(*markup = (*markup + strlen (keyword)-1)) = 0;
        (*markup)++;
        *end_of_url = 0;

        if (has_data==0)
        {
            (*markup)[0] = 0; // no data for this option, we only care if present or not present.
        }

        if (can_be_defined==1)
        {
            //
            // This is if option can be of form 'define some_var'
            //
            int is_def_result = 0;
            is_opt_defined (markup, &is_def_result, fname, lnum);
            if (is_def_result == 1)
            {
                oprintf ("char *%s = cld_init_string (\"\");\n", *markup);
            }
        }
    }
    else if (is_mandatory==1)
    {
        _cld_report_error( "%s markup is missing in %s, reading file [%s] at line [%d]", keyword, markup_name, fname, lnum);
    }
}

//
// Get query id for a markup that follows something#queryname as define .... mtext is anything 
// after #, msize is the length of it. file_name/lnum is current file name and line number being processed.
// is_defined is output: 1 if 'define' is present, 0 if not. 'asvar' is output: name of 'as' (variable) ,
// NULL if variable not present. is_defined can be 1 only if asvar is not NULL.
// gen_ctx is execution context.
// Returns the query id of query specified right after # sign. For example:
// column-count#myquery as define col_count
// Here, return value is some number which is the ID of myquery in our qry[] array in context, is_defined is 1
// and asvar is 'col_count', and in:
// row-count#myquery
// Here, return value is ID of query, is_defined is 0, and asvar is NULL
//
int get_query_id (cld_gen_ctx *gen_ctx, char *mtext, int msize, const char *file_name, int lnum, int *is_defined, char **asvar)
{
    // get length of query ID
    int qry_markup_len = msize;

    //
    // this is static, since asvar points to somewhere in it. Since we don't use threads, it is fine.
    // We need this memory to stay for the duration of the program for the caller to have the correct asvar
    // value
    //
    static char qry_markup[CLD_MAX_QUERYNAME_LEN + 1]; 

    // make sure we have space for it
    if (qry_markup_len > (int)sizeof (qry_markup) - 1)
    {
        _cld_report_error( "Qry ID too long, reading file [%s] at line [%d]", file_name, lnum);
    }

    // obtain query ID
    memcpy (qry_markup, mtext, qry_markup_len);
    qry_markup[qry_markup_len] = 0;

    cld_trim (qry_markup, &qry_markup_len);

    //
    // Get AS [define] variable
    //
    char *as = strstr (qry_markup, CLD_KEYAS);
    *asvar = NULL;
    *is_defined = 0;
    if (as != NULL)
    {
        *as = 0;
        qry_markup_len = strlen (qry_markup);
        cld_trim (qry_markup, &qry_markup_len);
        *asvar = as + strlen (CLD_KEYAS);
        is_opt_defined (asvar, is_defined, file_name, lnum);
    }
    
    //
    // Find query
    //
    int k = find_query (gen_ctx, qry_markup);
    if (k == -1)
    {
        _cld_report_error( "Query [%s] is not found, reading file [%s] at line [%d]", qry_markup, file_name, lnum);
    }
    return k;
}


//
// For a given option text (without the option keyword), find out if it has 'define' keyword
// 'option' is the option text (such as 'define xyz' that was part of for example 'program-output define xyz')
// and on output it would be just 'xyz' and is_defined would be 1. If it were just 'xyz', it would be still 
// 'xyz' and is_defined would be 0.
//
// We do not make a copy via cld_strdup of the option because option is capped to be a string on its own, 
// i.e when we manipulate the option string here, we won't be moving the memory for the entire line, passed
// the option itself.
//
void is_opt_defined (char **option, int *is_defined,  const char *file_name, int lnum)
{
    int def_len = strlen (CLD_KEYDEFINED);
    int l;
    *is_defined=0;

    // trim it so we can reliably check if it starts with 'define'
    l = strlen (*option);
    cld_trim (*option, &l);

    if (!strncmp (*option, CLD_KEYDEFINED, def_len))
    {
        // if it starts with 'defined', get passed it
        *option = *option + strlen (CLD_KEYDEFINED);
        *is_defined = 1; // set output flag to indicate it has 'define'
        // trim the rest of it
        l = strlen (*option);
        cld_trim (*option, &l);
    }

    if (option[0]==0)
    {
        _cld_report_error ("Markup option is empty, reading file [%s] at line [%d]", file_name, lnum);
    }
}

// 
// Display verbosely steps taken during the preprocessing
// cld_line is the line number is source, plus the printf-like output
//
void out_verbose(int cld_line, const char *format, ...)
{
    if (verbose==0) return;
    // THIS FUNCTON MUST NEVER USE ANY FORM OF MALLOC OR CLD_MALLOC
    // or it may fail when memory is out or not available (such as in cld_malloc)

    char trc[CLD_TRACE_LEN + 1];

    va_list args;
    va_start (args, format);
    vsnprintf (trc, sizeof(trc) - 1, format, args);
    va_end (args);

    fprintf (stdout, "Line %d: %s\n", 
        cld_line, trc);
    fflush (stdout);
}


//
// Find query fragment if name of it is 'name'. fname/lnum is the file/line of source where
// this query takes place. Search is case sensitive.
// Returns text of fragment or empty string if not found.
// Errors out if fragment name not found.
//
char *find_query_fragment(char *name, const char *fname, int lnum)
{
    int i;
    for (i = 0; i <tot_qry_fragments;i++)
    {
        if (!strcmp(name, qry_fragments[i].name))
        {
            return qry_fragments[i].text;
        }
    }
    _cld_report_error ("Query fragment [%s] not found, reading file [%s] at line [%d]", name, fname, lnum);
    return "";
}

//
// Add query fragment with name 'name' of text 'text'. fname/lnum is the file/line of source 
// where this query takes place. 
// Errors out if fragment exists or too many fragments.
//
void add_query_fragment(char *name, char *text, const char *fname, int lnum)
{
    int i;
    for (i = 0; i <tot_qry_fragments;i++)
    {
        if (!strcmp(name, qry_fragments[i].name))
        {
            _cld_report_error ("Query fragment [%s] redefinied, reading file [%s] at line [%d]", name, fname, lnum);
        }
    }
    if (tot_qry_fragments == CLD_MAX_QUERY_FRAGMENTS)
    {
        _cld_report_error ("Too many query fragments, limit [%d] redefinied, exiting, reading file [%s] at line [%d]", CLD_MAX_QUERY_FRAGMENTS, fname, lnum);
    }
    qry_fragments[tot_qry_fragments].name = cld_strdup(name);
    qry_fragments[tot_qry_fragments].text = cld_strdup(text);
    tot_qry_fragments++;
}


//
// Remove <?..?> parameters from SQL and replace with '%s'. Used in soft comparing shards and dynamic qrys.
// sql is text of SQL, fname/lnum is the file/line of source where this query takes place.
// If found open <? without closing ?>, it errors out.
void remove_sql_params(char *sql, const char *fname, int lnum)
{
    char *curr=sql;
    while (1)
    {
        char *opening=strstr(curr,"<?");
        char *ending=NULL;
        if (opening!=NULL)
        {
            ending=strstr(opening+2,"?>");
            if (ending==NULL)
            {
                _cld_report_error ("Query parameter closing (?>) not found in [%s], reading file [%s] at line [%d]", opening, fname, lnum);
            }
            // remove <?...?> and put in %s
            sprintf(opening,"'%%s'%s", ending+2); // always works because %s is shorter than <??> in worst case
            curr=opening+2;// get passed %s
        }
        else break;
    }
}


//
// Find query shard from name 'name'. fname/lnum is the file/line of source where this query takes place.
// 'soft' is the output which is 1 if this is soft-compare shard.
// Returns text of shard or NULL if not found.
//
char *find_query_shard(char *name, int *soft, const char *fname, int lnum)
{
    // unused for now, but if there are future errors, this will be useful
    CLD_UNUSED (fname);
    CLD_UNUSED (lnum);

    int i;
    for (i = 0; i <tot_qry_shards;i++)
    {
        if (!strcmp(name, qry_shards[i].name))
        {
            if (soft!=NULL) *soft=qry_shards[i].soft_compare;
            return qry_shards[i].text;
        }
    }
    return NULL;
}

//
// Add query shard of name 'name' and text 'text' and 1 for soft if soft-compare. fname/lnum is the file/line of source where this query takes place.
// Errors out if existing shard redefined or too many shards. Search is case sensitive.
//
void add_query_shard(char *name, char *text, int soft, const char *fname, int lnum)
{
    int i;
    int shard_index = tot_qry_shards;;
    int is_found = 0;
    for (i = 0; i <tot_qry_shards;i++)
    {
        if (!strcmp(name, qry_shards[i].name))
        {
            if (qry_shards[i].text[0]!=0)
            {
                _cld_report_error ("Query shard [%s] redefinied, reading file [%s] at line [%d]", name, fname, lnum);
            }
            else
            {
                // this is defined but uninitialized shard, i.e. just define-shard#...
                is_found = 1;
                shard_index = i;
                break;
            }
        }
    }
    if (shard_index == CLD_MAX_QUERY_SHARDS)
    {
        _cld_report_error ("Too many query shards, limit [%d] redefinied, exiting, reading file [%s] at line [%d]", CLD_MAX_QUERY_SHARDS, fname, lnum);
    }
    if (is_found == 0)
    {
        qry_shards[shard_index].name = cld_strdup(name);
        qry_shards[shard_index].soft_compare=soft; // 1 for soft, 0 for hard
    }
    
    // don't bother freeing [].text if is_found==1, it is an empty string, minor leak taken care on the exit
    qry_shards[shard_index].text = cld_strdup(text);

    if (is_found==0)
    {
        tot_qry_shards++;
    }
}

// 
// Find terminal width for help display.
// Returns width in characters.
//
int terminal_width()
{
    static int twidth = 0;
    if (twidth == 0)
    {
        struct winsize w;
        ioctl(STDOUT_FILENO, TIOCGWINSZ, &w);

        twidth = (int)(w.ws_col);
    }
    return twidth;
}

//
// Print out help for CLD. Perform splitting/indenting of lines where lines are too long and indented.
// tabs must always be in front of everything else here, i.e. no tabs in the middle of text!
// Errors out if line too long.
//
void tfprintf (FILE *f, const char *format, ...)
{
    va_list args;
    va_start (args, format);

    char *oline = NULL;
    int oline_size = 0;
    int tw = terminal_width();

    if (oline == NULL) oline = cld_malloc (oline_size = 2*CLD_MAX_CODE_LINE + 1);

    int tot_written = vsnprintf (oline, oline_size - 1, format, args);
    if (tot_written >= oline_size - 1)
    {
        _cld_report_error ("Line being output is too long, exiting");
        exit (1);
    }

    // substitute leading tabs
    const char *tab = "    ";
    int tab_len = strlen (tab);
    int tot_tab = cld_count_substring (oline, "\t"); 
    int oline_len = strlen (oline);
    cld_trim (oline, &oline_len);

    // Print out and try to split line so wraparound is indented exactly as the beginning of the line is.
    char *curr_line = oline;
    while (1)
    {
        // length of the line
        int out_size = strlen (curr_line);
        // print out tabs
        int c = tot_tab;
        for (c = 0; c < tot_tab; c++) fprintf (f, "%s", tab);
        // what's remaining width on the display to print on
        int w = tw - tot_tab*tab_len - 2; // 2 less for sanity
        // check remaining width against the length of line
        if (out_size > w )
        {
            // need to split it
            // find the last space character that can fit on the line
            while (w >=0 && !isspace(curr_line [w])) w--;
            if (w <= 0) 
            {
                // cannot find space, just print everything and return
                fprintf (f, "%s\n", curr_line);
                break;
            }
            // cap the line where the last space character that fits on the line is, and don't print any leading spaces
            curr_line[w] = 0;
            int beg_line = 0;
            while (isspace(curr_line[beg_line])) beg_line++;
            fprintf (f, "%s\n", curr_line+beg_line);
            // start the process of checking the remainder of the line, if it's too big
            curr_line = curr_line + w + 1;
        }
        else
        {
            fprintf (f, "%s\n", curr_line);
            break;
        }
    }
    cld_free (oline);
}

// 
// Handle quotes in input parameter. inp_par is the input parameter. deescape is 1
// if we will deescape an escaped double quote. Output is is_inp_str which is 1
// if this is a quoted string, 0 otherwise.
// Removes double quotes if the string was quoted, and deescape if specified.
// This is used to handle input parameters for 1) a list of input parameters for 
// run-query# 2) <?..?> parameters (compact parameter) 3) exec-program parameters.
//
void handle_quotes_in_input_param (char **inp_par, int *is_inp_str)
{
    int par_len = strlen (*inp_par); 
    // we do not de-escape \, (i.e. a comma) because that's just for separating parameters, and it's not formatting feature of C
    // strings - this is for us only to separate comma (that separates input parameters) from a comma that's part of C
    // expression within an input parameter. Commas are de-escaped in parse_param_list() which handles the comma-separated lists.
    //
    // The reason why \" would be in a parameter is because for query inputs, or exec-program inputs, they are under 
    // quotes (either in <??> in query, or in run-query#...:".\"..\"", or in exec-program ... program-args "..\"..\"..") 
    // In all of these cases, they are used to generated code, as in 
    // char *x = "...input param..."
    // therefore, escaped quotes MUST remain. This is the reason we do NOT de-escape double quotes.

    // set the flag whether this is a quoted string or not, and if it is quoted, remove
    // leading and trailing quotes.
    *is_inp_str = 0;
    if ((*inp_par)[0] == '"')
    {
        *is_inp_str = 1;
        (*inp_par)[par_len - 1] = 0;
        (*inp_par) ++;
    }
}

//
// Parse a list of parameters. Parameters are a list of strings or non-string parameters separated
// by commas. If a string parameter has quotes in it, they must be escaped with \ . If a non-string
// parameter has a comma in it, it must be escaped with \ .
//
// String representing list of parameters is parse_list. CLD list structure is 'params', which will
// hold the parsed parameters. file_name/lnum is the file/line of source where this query takes place.
//
// This cannot be used for compact_params because it looks for commas - commas are okay in compact
// params and should NOT be treated any differently - this is for a LIST of parameters separated by 
// comma ONLY (i.e. run-query# and exec-program).
// On return, parse_list is one byte passed the end of last character in the list, which is null character.
//
void parse_param_list (char **parse_list, cld_store_data *params, const char *file_name, int lnum)
{
    cld_store_init(params);

    while (1)
    {
        // input param can be a quoted string, or a variable, or a number 
        // so find unescaped quote of comma, or if none found returns pointer
        // to the ending null char of *parse_list string.
        char *milestone = find_unescaped_chars (*parse_list, "\","); 
        char *end_of_parse_list; // this is where parse_list should be after looking for parameter
        char _inp_par[CLD_MAX_QUERY_INPUT_LEN + 1];
        char *inp_par=_inp_par; // because we will ++ this pointer, can't work otherwise

        if (*milestone == '"')
        {
            // this is actually beginning of the quoted value, find the end
            // it cannot have double quotes in it. Find unescaped quote after we
            // found the first unescaped quote above.
            milestone = find_unescaped_chars (milestone + 1, "\"");
            if (*milestone == 0)
            {
                _cld_report_error( "Unterminated string (missing double quote), reading file [%s] at line [%d]", file_name, lnum);
            }
            milestone++; // get passed final quote and include it
            end_of_parse_list = milestone; // one beyond double quote

            // skip white space after second quote (the end of quoted string)
            while (isspace(*end_of_parse_list)) end_of_parse_list++;

            // after we got passed the quoted parameter (and skipped over any whitespaces), what follows
            // MUST be either comma or the end of string. If not, error out.
            if (*end_of_parse_list == ',')
            {
                end_of_parse_list++;
            }
            else if (*end_of_parse_list == 0)
            {
                // end of parsing, but we must first process this quoted string before breaking out of loop!
            }
            else
            {
                _cld_report_error( "Expected comma or end of list, reading file [%s] at line [%d]", file_name, lnum);
            }
        }
        else
        {
            if (*milestone == 0)
            {
                // we reached the end of string, this is the last parameter
                end_of_parse_list = milestone;// get one beyond the last parameter
                    // in this case *end_of_parse_list is null char, a signal below to
                    // exit the loop, after we processed the last param.
            }
            else
            {
                end_of_parse_list = milestone+1;// get one beyond comma
                // skip white space after comma
                while (isspace(*end_of_parse_list)) end_of_parse_list++;
            }
        }

        // milestone now points to either the next parameter or the end of string (meaning end of param list)

        // length of input parameter: if quoted string, include quotes in length. If not quoted, do not include comma.
        int par_len = milestone - (*parse_list);

        if (par_len > CLD_MAX_QUERY_INPUT_LEN - 1)
        {
            _cld_report_error( "Parameter too long [%d], parameter [%.100s], reading file [%s] at line [%d]", par_len, *parse_list, file_name, lnum);
        }

        memcpy (inp_par, *parse_list, par_len);
        inp_par[par_len] = 0;

        cld_trim(inp_par, &par_len);


        // de-escaping will work because the resulting string is always smaller
        // the \" must be  de-escaped later by the caller, when parameters are used
        cld_replace_string (inp_par, par_len+1, "\\,", ",", 1, NULL); // de-escape


        int is_inp_str;
        // if quoted, remove leading and ending quote, inform us if this was quoted in the first place
        handle_quotes_in_input_param (&inp_par, &is_inp_str);

        // store parameter to a list, 1/0 indicates if this is a string or not
        if (is_inp_str==1)
        {
            cld_store(params,"1", inp_par);
        }
        else
        {
            cld_store(params,"0", inp_par);
        }

        // break if at the end
        if (*end_of_parse_list==0) break;

        *parse_list = end_of_parse_list; // positioned to next parameter
    } 
    cld_rewind(params);
}

//
// Find next input parameter in SQL text (which is end_of_query). gen_ctx is the context of parsing, and 
// file_name/lnum is the file/line of source where this query takes place.
// This parses part of markup such as run-query#xyz: a,b,c... i.e. it finds a, b, c etc. In this case query text uses '%s' instead of <?..?>.
// The parameter is a C expression or a quoted string (technically also a C expression, but treated differently).
// A parameter must have a double quote and a comma escaped with backslash.
// So for example a parameter like func_call("x",2) would be func_call(\"x\"\,2)
// This function is used only for when query uses '%s', and NOT if query uses <?..?> within it, in which case there is need only
// for de-escaping a double quote, since the entire SQL text is enclosed in double quotes.
//
// gen_ctx is the context. query_id is index into gen_ctx->qry[] array. end_of_query is the list of parameters to be parsed
// This function expects open text line, and will leave it in that state (i.e. BEGIN_TEXT_LINE is done prior to calling this).
//
void get_next_input_param (cld_gen_ctx *gen_ctx, int query_id, char **end_of_query, const char *file_name, int lnum)
{
    if (gen_ctx->qry[query_id].is_qry_compact==1)
    {
        _cld_report_error( "This query cannot have any additional parameters because all parameters must be within the query itself, reading file [%s] at line [%d]", file_name, lnum);
    }

    // parse params. 
    cld_store_data params;
    parse_param_list (end_of_query, &params, file_name, lnum);


    char *is_inp_str;
    char *value;
    while (1)
    {
        cld_retrieve (&params, &is_inp_str, &value);
        if (is_inp_str==NULL) break;

        // this function expects open text line, and will leave it in that state.
        END_TEXT_LINE
        add_input_param (gen_ctx, query_id, atoi(is_inp_str), value, file_name, lnum);
        BEGIN_TEXT_LINE
    } 
}

// 
// Add input parameter to a query.
// gen_ctx is the context. query_id is the index into gen_ctx->qry[] (i.e. queries). inp_par is the string
// representing input parameter (it is either a quoted string such as "abx" or any string - if quoted, quotes
// are still present). is_inp_str is 1 if inp_par is a quoted string, 0 otherwise.
// fname/lnum is the file/line of source where this query takes place.
//
void add_input_param (cld_gen_ctx *gen_ctx, int query_id, int is_inp_str, char *inp_par, const char *file_name, int lnum)
{
    CLD_TRACE("");



    gen_ctx->qry[query_id].qry_is_input_str[gen_ctx->qry[query_id].qry_found_total_inputs] = is_inp_str;
    strncpy (gen_ctx->qry[query_id].qry_inputs[gen_ctx->qry[query_id].qry_found_total_inputs], inp_par, 
       CLD_MAX_QUERY_INPUT_LEN - 1);

    oprintf("__is_input_used_%s[%d]=1;\n",  gen_ctx->qry[query_id].name, gen_ctx->qry[query_id].qry_found_total_inputs);
    gen_ctx->qry[query_id].qry_found_total_inputs++;

    // if query is dynamic, do not check inputs count, because we don't know it until run time and can be 
    // different each time query executes. At run-time, makeSQL will complain if not correct
    if (gen_ctx->qry[query_id].is_dynamic == 0)
    {
        if (gen_ctx->qry[query_id].qry_found_total_inputs > gen_ctx->qry[query_id].qry_total_inputs)
        {
            _cld_report_error( "Too many query input parameters [%d] for query [%s], expected [%d] parameters, reading file [%s] at line [%d]. %s", gen_ctx->qry[query_id].qry_found_total_inputs, gen_ctx->qry[query_id].name, gen_ctx->qry[query_id].qry_total_inputs, file_name, lnum, CLD_PARAM_USAGE);
        }
    }
}

//
// Process end-query markup. Update relevant data structures such as query stack.
// gen_ctx is preprocessor context, close_block is 1 if this is run-query (where we generate for loop)
// and 0 for start-query (where we don't open a loop, developer must use loop-query later).  
// fname/lnum is the file/line of source where this query takes place.
// Output: query_id will be set to either -1 if there is no active query once we end this query, or to 
// that query's id (for example in case of nested queries).
// Error out if there is no active query at the point in source code.
//
void end_query (cld_gen_ctx *gen_ctx, int *query_id, int *open_queries, int close_block, const char *file_name, int lnum)
{
    // end of action block found , but there was not beginning
    if (*query_id == -1)
    {
        _cld_report_error( "query ending found, but no matching beginning, reading file [%s] at line [%d]", file_name, lnum);
    }
    // Ending of query with ID of leaving ID
    int leaving_id = gen_ctx->global_qry_stack[gen_ctx->curr_qry_ptr - 1];
    assert (leaving_id != -1);

    // no longer active, and its variables are defined, in case we
    // need to use them again - don't define them again in C code
    gen_ctx->qry_active[leaving_id] = CLD_QRY_USED;

    // reset number of inputs found in <?qry XX, in case we have another 
    // same query later on, for which we need to calculate this 'actual'
    // number of found input URL params
    gen_ctx->qry[leaving_id].qry_found_total_inputs = 0;

    // put invalid query ID on stack so we know there is nothing there
    gen_ctx->global_qry_stack[gen_ctx->curr_qry_ptr - 1] = -1;

    // go down one level
    gen_ctx->curr_qry_ptr --;
    assert (gen_ctx->curr_qry_ptr >= 0);

    if (gen_ctx->curr_qry_ptr == 0)
    {
        // if back at zero (i.e. no action block active, then no queries active,
        // so query ID is -1
        *query_id = -1;
    }
    else
    {
        // otherwise, go back to surrounding query, i.e. we were in a nested
        // query up until now
        *query_id = gen_ctx->global_qry_stack[gen_ctx->curr_qry_ptr - 1];
    }

    END_TEXT_LINE

    if (close_block == 1)
    {
        oprintf("}\n"); // end of FOR loop we started with the QRY markup beginning
    }
    BEGIN_TEXT_LINE

    (*open_queries)--;
}


//
// Generate the C code to allocate a query. gen_ctx is the contect, and query_id is the query id.
// Depending on what kind of code we generate later, some of these may not be used, and we mark them
// as unused to avoid compiler warning.
//
void cld_allocate_query (cld_gen_ctx *gen_ctx, int query_id)
{
    // number of rows and columns retrieved
    oprintf("int __nrow_%s;\n", gen_ctx->qry[query_id].name);
    oprintf("int __ncol_%s;\n", gen_ctx->qry[query_id].name);
    oprintf("CLD_UNUSED(__nrow_%s);\n", gen_ctx->qry[query_id].name);
    oprintf("CLD_UNUSED(__ncol_%s);\n", gen_ctx->qry[query_id].name);

    // when we do 'define-query' with empty text followed by start-query or run-query, we don't
    // know if query is DML or not, so we have to specify all variables
    oprintf("unsigned int __err_%s;\n", gen_ctx->qry[query_id].name);
    oprintf("CLD_UNUSED (__err_%s);\n", gen_ctx->qry[query_id].name);
    oprintf("char __insert_id_%s[20];\n", gen_ctx->qry[query_id].name);
    oprintf("CLD_UNUSED (__insert_id_%s);\n", gen_ctx->qry[query_id].name);
    oprintf("char **__data_%s = NULL;\n", gen_ctx->qry[query_id].name);
    oprintf("CLD_UNUSED (__data_%s);\n", gen_ctx->qry[query_id].name);
    oprintf("char **__col_names_%s;\n", gen_ctx->qry[query_id].name);
    oprintf("CLD_UNUSED (__col_names_%s);\n", gen_ctx->qry[query_id].name);

    // allocate SQL buffer
    oprintf("char *__sql_buf_%s = (char*)cld_malloc (%d + 1);\n", gen_ctx->qry[query_id].name, CLD_MAX_SQL_SIZE);

    // __is_input_used specifies if there's an input parameter. For dynamic SQL, when add-query-input is used,
    // there may be some zeroes in it, but normally it is a block of 1's corresponding to input parameters
    // see comments on cld_make_SQL() function for more on this.
    oprintf ("int __is_input_used_%s[%d];\n", gen_ctx->qry[query_id].name, CLD_MAX_QUERY_INPUTS + 1);
    oprintf ("memset (__is_input_used_%s, 0, sizeof (int)*%d);\n", gen_ctx->qry[query_id].name, CLD_MAX_QUERY_INPUTS + 1);
    // __arr is the array of output data from the query.
    oprintf ("char ***__arr_%s = NULL;\n", gen_ctx->qry[query_id].name);
    // __iter is the iterator through output data loop (incrementing by 1 for each row)
    oprintf("int __iter_%s;\n", gen_ctx->qry[query_id].name);
    // __column_count is the total number of columns
    oprintf("char __column_count_%s[30];\n", gen_ctx->qry[query_id].name);
    // __row_count is the total number of rows
    oprintf("char __row_count_%s[30];\n", gen_ctx->qry[query_id].name);
    // __current_row is the current row number
    oprintf("char __current_row_%s[30];\n", gen_ctx->qry[query_id].name);
    oprintf("CLD_UNUSED (__current_row_%s);\n", gen_ctx->qry[query_id].name);
    oprintf("CLD_UNUSED (__row_count_%s);\n", gen_ctx->qry[query_id].name);
    oprintf("CLD_UNUSED (__column_count_%s);\n", gen_ctx->qry[query_id].name);
}



// 
// find first from the set of chars that isn't escaped. So for example
// in string ab\"cd\"xy" find_unescaped_chars(<this string>,"\"") will find the 
// very last " and ignore the ones before, since they are ESCAPED
// Look in 'start' string for each char from 'chars' string. Look for each of those
// that aren't escaped, returned the first one. This is useful when parsing code to account
// for escaped characters.
// Return the pointer to first unescaped char as described aboved, or the pointer to the
// end of string if none found.
//
char *find_unescaped_chars (char *start, char *chars)
{
    assert (start);
    assert (chars);
    
    // e is always assigned value in while loop below
    char *e;

    while (1)
    {
        e = start + strcspn (start, chars);
        if (*e == 0) return e;

        if (*(e-1) == '\\')
        {
            start = e + 1;
            continue;
        }
        else break;
    }
    return e;
}

// 
// Output generated C code. We remove empty lines (of which is generally plenty in order not to clutter
// HTML output, and to consistently output only non-empty lines. Works like printf.
// Error out if generated source code line too long.
//
void oprintf (const char *format, ...)
{
    va_list args;
    va_start (args, format);

    static char *oline = NULL;
    static int oline_len = 0;
    static int oline_size = 0;

    if (format == NULL)
    {
        if (oline != NULL)
        {
            // remove empty printouts
            int len = strlen (oline);
            cld_replace_string (oline, len+1, "cld_puts (CLD_NOENC, \"\");\n", "", 1, NULL); // remove idempotent printouts
                                                    // will always succeed because output is shorter 
            cld_replace_string (oline, len+1, "cld_puts (CLD_NOENC, \"\");", "", 1, NULL); // replace with or without new line

            // The output goes to outf, i.e. the global file description tied to generated C code,
            // OR it goes to stdout, which would be if this is a command line program
            if (outf != NULL)
            {
                fprintf (outf, "%s", oline);
                va_end (args);
            }
            else
            {
                printf ("%s", oline);
                va_end (args);
            }
            return;
        }
    }

    if (oline == NULL) oline = cld_malloc (oline_size += 2*CLD_MAX_CODE_LINE + 1);

    int tot_written = vsnprintf (oline + oline_len, oline_size - 1, format, args);
    if (tot_written >= oline_size - 1)
    {
        _cld_report_error ("Source code line too long, exiting");
        exit (1);
    }
    oline_len += tot_written;
    if (oline_len >= CLD_MAX_CODE_LINE)
    {
        oline = cld_realloc (oline, oline_size += 2*CLD_MAX_CODE_LINE + 1);
    }
}


// 
// Output error to stderr. The error means error during the preprocessing with CLD.
// There's a maximum length for it, and if it's more than that, ignore the rest.
// If there is no dot at the end, put it there.
// After that, exit program.
//
void _cld_report_error (const char *format, ...)
{
    char errtext[CLD_MAX_ERR_LEN + 1];

    oprintf (NULL); //  flush output

    va_list args;
    va_start (args, format);
    vsnprintf (errtext, sizeof(errtext) - 1, format, args);
    va_end (args);
    fprintf (stderr, "%s", errtext);
    if (errtext[0] != 0 && errtext[strlen (errtext) - 1] != '.')  fprintf(stderr, ".");
    fprintf (stderr, "\n");
    exit (1);
}

// 
// Get column ID for a query and output column name. gen_ctx is preprocessor context, 
// fname/lnum is the file/line of source where this query takes place, qry_name is the ID of a query
// (in the array of queries), column_out is the name of the column for which we seek the ID. The ID of a column
// is the array index in the list of output columns that matches the data array (the result set).
// Returns the ID of a column.
// Errors out if column name is not a part of the result set, OR if there are two columns with the SAME name, which can
// happen in mariadb - we do NOT allow this - all columns MUST have unique names.
// To do this, we execute SQL a query and discard the data. DMLs are not executed since we have a fixed set
// of columns returned.
//
int get_col_ID (cld_gen_ctx *gen_ctx, int qry_name, const char *column_out, const char *fname, int lnum)
{
    assert (gen_ctx);
    assert (column_out);
    assert (fname);

    int is_insert = gen_ctx->qry[qry_name].is_insert;
    int is_DML = gen_ctx->qry[qry_name].is_DML;
    if (is_DML == 1)
    {
        // for DML queries there are only affected_rows, error and insert_id columns
        if (!strcmp (column_out, "affected_rows"))
        {
            return  0;
        }
        else if (!strcmp (column_out, "error"))
        {
            return  1;
        }
        else if (!strcmp (column_out, "insert_id"))
        {
            if (is_insert != 1)
            {
                _cld_report_error( "'insert_id' is allowed only for insert queries, reading file [%s] at line [%d]", fname, lnum);
            }
            return  2;
        }
        else
        {
            _cld_report_error( "Unknown column [%s], only 'affected_rows', 'insert_id', and 'error' allowed for DML queries, reading file [%s] at line [%d]", column_out, fname, lnum);
        }
    }


    char all_columns[CLD_TOT_COLNAMES_LEN + 1];
    // get column ID from column name
    int i;
    int j = 0;
    int found_col = -1;
    for (i = 0; i < gen_ctx->qry[qry_name].qry_total_outputs; i++)
    {
        j+=snprintf (all_columns + j, sizeof (all_columns)-j-1, "%s,", gen_ctx->qry[qry_name].qry_outputs[i]);
        if (!strcmp (column_out, gen_ctx->qry[qry_name].qry_outputs[i]))
        {
            //
            // Make sure column is not duplicated in the query output, leading to hard-to-find programming errors
            //
            if (found_col != -1) 
            {
                _cld_report_error( "Column [%s] is present more than once in the set of columns for query[%s], please make sure every column in query output has a unique name. List of columns separated by comma is [%s], reading file [%s] at line [%d]", column_out, gen_ctx->qry[qry_name].text, all_columns, fname, lnum);
            }
            found_col = i;
        }
    }
    if (found_col != -1) return found_col;
    if (j>0) all_columns[j-1] =0;
    _cld_report_error( "Column [%s] is not a part of the set of columns for query[%s], list of columns separated by comma is [%s], reading file [%s] at line [%d]", column_out, gen_ctx->qry[qry_name].text, all_columns, fname, lnum);

    return 0; // will never be reached, column not found
}

//
// Describe query, i.e. determine the set of columns it possesses.
// gen_ctx is the context, qry_name is query ID of the query,
// fname/lnum is the file/line of source where this query takes place.
// This is called ONCE per query definition (be it start-query or run-query) and not
// once per column resolution (as it used to be), so we can cache this call always
// except when start-query is used, because each start-query has a different query text.
// We do not call this function for define-query because there is no query text yet there, and we
// will call it when run-query/start-query happens, since qry_total_outputs has remained 0.
//
void describe_query (cld_gen_ctx *gen_ctx, int qry_name, const char *fname, int lnum)
{
    // if this is dynamic query, cannot describe it as we don't have it's text (known only at run-time)
    if (gen_ctx->qry[qry_name].is_dynamic==1) return;

    // no need to describe DML, we already know what's returned for any DML
    if (gen_ctx->qry[qry_name].is_DML == 1) return;

    char **col_names;

    // this is when we must re-check columns: if we already have outputs but we check again - 
    // this happens only for start-query, in which case we must make sure the output columns are
    // identical. start-query can redefined SQL text of a query any number of times. The code is 
    // properly generated for each such redefinition, but we must make sure the output columns are the same
    // or some nasty bugs could occur.
    int check_again = (gen_ctx->qry[qry_name].qry_total_outputs != 0);

    // Find out a list of names of columns in the output of a sample SQL query
    // and tie a name of output column to a positional number of a column in
    // the result set (in a row)
    int snrow;
    int sncol;
    CLD_TRACE ("sample query [%s]", gen_ctx->qry[qry_name].text);
    char *fname_loc = cld_strdup(fname); // to overcome constness
    cld_location (&fname_loc, &lnum, 1);
    cld_select_table (gen_ctx->qry[qry_name].text, &snrow, &sncol, &col_names, NULL);

    int column_id = 0; // current column ID, from 0 to sncol
    while (column_id < sncol)
    {
        if (check_again==1)
        {
            // check that each start-query query text has the SAME output columns
            if (strcmp (col_names[column_id], gen_ctx->qry[qry_name].qry_outputs[column_id]))
            {
                _cld_report_error( "In multiple queries, output column [%s] does not match output column [%s] in query [%s], reading file [%s] at line [%d]", col_names[column_id],
                    gen_ctx->qry[qry_name].qry_outputs[column_id], gen_ctx->qry[qry_name].name, fname, lnum);
            }
        }
        else
        {
            //
            // First time around (when check_again is 0, i.e. for the first start-query or for run-query), we get the column names
            // Later, if we do check_again=1 for start-query only, the column names are ALREADY in place, so no need to copy again
            //
            gen_ctx->qry[qry_name].qry_outputs[column_id] = col_names[column_id]; // col_names is cld_malloc-ed inside cld_select_table
        }
        // move to the next column
        column_id++;

    }
    gen_ctx->qry[qry_name].qry_total_outputs = column_id;
}


//
// Prepare SQL (a static text of SQL) and stop there. The intent is to catch various syntax and other errors
// This will NOT catch all errors, but will catch a sizeable chunk of query problems.
// gen_ctx is the context, query_name is the ID of a query, fname/lnum is the file/line of source where this query takes place.
// Output parameter err holds the error in query, if any, otherwise it's NULL.
// Returns 1 if no error, 0 if error.
//
int try_DML (cld_gen_ctx *gen_ctx, int query_name, const char *fname, int lnum, char **err)
{
    // unused for now, but in the future there may be error message
    CLD_UNUSED (fname);
    CLD_UNUSED (lnum);

    assert (gen_ctx);
    assert (fname);
    if (gen_ctx->qry[query_name].is_dynamic==1) return 1; // cannot check dynamic ones

    if (gen_ctx->qry[query_name].qry_total_outputs != 0) return gen_ctx->qry[query_name].qry_total_outputs;

    MYSQL_STMT *mh = mysql_stmt_init(cld_get_db_connection(gen_ctx->db));
    // prepare will NOT catch all errors. For example update A,B set A.X=B.Y where... will NOT catch if B.Y column does not exist!
    // AVOID such queries - break them into SELECT/UPDATE
    int res=mysql_stmt_prepare(mh, gen_ctx->qry[query_name].text, strlen(gen_ctx->qry[query_name].text));
    if (res!=0) *err=cld_strdup(mysql_stmt_error(mh)); else *err=NULL;
    mysql_stmt_close(mh);

   if (res==0) return 1; else return 0;

}

// 
// Get number of columns in a SQL query. gen_ctx is the context, query_name is the ID of a query,
// fname/lnum is the file/line of source where this query takes place.
// Returns the number of columns in a result set row.
//
int get_num_of_cols (cld_gen_ctx *gen_ctx, int query_name, const char *fname, int lnum)
{
    assert (gen_ctx);
    assert (fname);

    // use cached values if available, do not keep executing the query for each request!
    if (gen_ctx->qry[query_name].qry_total_outputs != 0) return gen_ctx->qry[query_name].qry_total_outputs;

    int snrow;
    int sncol;
    char **col_names;

    // execute SQL as it is in the query definition, 
    CLD_TRACE ("sample query [%s]", gen_ctx->qry[query_name].text);
    char *fname_loc = cld_strdup(fname); // to overcome constness
    cld_location (&fname_loc, &lnum, 1);
    cld_select_table (gen_ctx->qry[query_name].text, &snrow, &sncol, &col_names, NULL);

    return sncol;
}


// 
// Add new query. gen_ctx is the context, orig_qry is the SQL text of query, qry_name is the name of the 
// query as given in (for example) run-query# or start-query#.
// fname/lnum is the file/line of source where this query takes place.
// We convert SQL query into a format-ted query, meaning we replace <?..expression..?> with '%s' and use
// the 'expression' as an input parameter, which will be proofed against SQL injection at run-time. The query
// with <?..?> is called 'compact' query. We also process shards and fragments here. Error messages are emitted
// if there are syntax errors, if shards don't match, if shards/fragments not found.
// Each time a fragment is expanded, processing starts from the beginning of the fragment which may have other 
// fragments or shards.
//
void new_query (cld_gen_ctx *gen_ctx, const char *orig_qry, char *qry_name, int lnum, const char *cname)
{
    assert (gen_ctx);
    assert (orig_qry);
    assert (qry_name);
    assert (cname);

    char *qry=cld_strdup(orig_qry);

    char *params[CLD_MAX_QUERY_INPUTS + 1];
    int param_count=0;

    // Check if this is a compact query such as 'select x from y where z=<?some C expression that doesn't have ?> in it ?>'
    char *is_compact=strstr(qry,"<?");
    if (is_compact!=NULL)
    {
        // this is a compact query
        char *curr=qry;
        char *begin=NULL;
        char *end=NULL;
        char *param = NULL;
        while (1)
        {
            int curr_qry_len = strlen(qry);
            int len = strlen (curr);
            // find anything that starts with <? and ends with ?> first
            // this includes input parameters, shards and fragments
            begin=strstr(curr,"<?");

            // <? MUST be followed by ?>: if this is a parameter, that's what a parameter is. If this is a shard or fragment, it's <?shard..?> or
            // <?fragment..?> so it must have ?> right away either way.
            if (begin==NULL)
            {
                end=strstr(curr,"?>");
                if (end!=NULL)
                {
                    _cld_report_error( "\n\nFound parameter closing ('?>') without matching parameter opening in a query, line [%d], reading file [%s]\n\n", lnum, cname);
                }
                break;
            }
            else
            {
                end=strstr(begin+2,"?>");
                if (end==NULL)
                {
                    _cld_report_error( "\n\nFound parameter opening ('<?') without matching parameter closing in a query, line [%d], reading file [%s]\n\n", lnum, cname);
                }
            }
            // extract parameter
            *end=0;

            //
            // The processing below checks if <?..?> is a shard, fragment or input parameter
            // When shard or fragment are found, and after they are processed, the processing starts again from the 
            // begining of the shard/fragment because they can have other <?..?> in them.
            // Eventually we exhaust all <?..?> markup inside a query text
            //


            // SHARD Processing
            // check if this is a shard, enforce if it is
            const char *shard_begin_tag = "<?shard#";
            int shard_begin_tag_len = strlen(shard_begin_tag);
            if (!strncmp(begin,shard_begin_tag, shard_begin_tag_len))
            {
                // get shard name
                char *shard_name=begin+shard_begin_tag_len;
                int shard_name_len=strlen(shard_name);
                cld_trim(shard_name,&shard_name_len);
                // find the end of shard
                const char *shard_end_tag = "<?end-shard?>";
                int shard_end_tag_len = strlen(shard_end_tag);

                char *shard_start = end+2; // end+2 is just passed <?shard#..?>
                // search for <?end-shard?> starting from the start of shard text
                char *shard_end = strstr(shard_start, shard_end_tag);

                // stack for shard begin/end pairs
                int shard_stack_ptr = 0;
#define CLD_MAX_SHARD_NEST_LEVEL 6 
                char *shard_stack_begin[CLD_MAX_SHARD_NEST_LEVEL+1];
                char *shard_stack_end[CLD_MAX_SHARD_NEST_LEVEL+1];

                // start with what we have for sure and that is shard's start
                shard_stack_begin[shard_stack_ptr] = shard_start;
                shard_stack_ptr++;

                char *inside_shard_start= NULL;
                // loop until we find all inside shard-starts exhausted with corresponding end-shards
                while (1)
                {
                    // find next shard-start. There may be none, or there may be one in between current start and end, or 
                    // there may be one after current end
                    inside_shard_start=strstr(shard_start,shard_begin_tag);
                    if (inside_shard_start!=NULL && (inside_shard_start-shard_start)<(shard_end-shard_start))
                    {
                        // this is there is shard-start inbetween current start and end
                        if (shard_stack_ptr>=CLD_MAX_SHARD_NEST_LEVEL)
                        {
                            _cld_report_error( "Too many open shard tags (<?shard#...?>) found, line [%d], reading file [%s]\n\n", lnum, cname);
                        }
                        // add stack element with this new start
                        shard_stack_begin[shard_stack_ptr]=inside_shard_start;
                        shard_stack_ptr++;
                        // search for a new start ALWAYS continues from the inside most shard (i.e. right-most), if we found the inside shard
                        // inbetween current start and end
                        shard_start=inside_shard_start+shard_begin_tag_len;
                        // continue - if we have more shard-starts inbetween NEW shard start and CURRENT end, pile them onto the stack
                        continue;
                    }
                    else
                    {
                        // this is there is no new shard start at all, OR there is one, but it is past the current end
                        assert(shard_stack_ptr!=0);
                        shard_stack_ptr--;
                        // remember the current end. It is tied to the shard-start that is corresponding to it because
                        // we just decreased shard_stack_ptr, and this is the top of the stack. 
                        // we always match the shard-start with the current shard-end if there is NO shard-starts
                        // INBETWEEN the shard-start and shard-end, i.e. they are inner most to each other.
                        shard_stack_end[shard_stack_ptr]=shard_end;
                        // because there is NO shard-start between current start and current end, set search point for shard-start
                        // after current shard-end. If this isn't the end, we will advance shard_end below, and then look for new start-starts
                        // inbetween shard_start we set here and this new end. shard-start serves the purpose to "flush out" if there are inside
                        // shard starts. If not, we use stack to match start and end tags.
                        shard_start=shard_end+shard_end_tag_len;
                        if (shard_stack_ptr==0)
                        {
                            // found the matching shard-end to the first shard#, the results are in shard_stack_...[0]
                            break;
                        }
                        else
                        {
                            // shard_end is ALWAYS pushed forward from the RIGHT-MOST shard-end
                            shard_end = strstr(shard_end+shard_end_tag_len,shard_end_tag);
                            if (shard_end==NULL)
                            {
                                _cld_report_error( "Too few shard closing tags (<?end-shard?>) found, line [%d], reading file [%s]\n\n", lnum, cname);
                            }
                            continue;
                        }
                    }
                } 
                shard_end=shard_stack_end[0];
                shard_start=shard_stack_begin[0];

                if (shard_end==NULL)
                {
                    _cld_report_error( "<?end-shard?> not found in query, found [%s], line [%d], reading file [%s]\n\n", shard_start, lnum, cname);
                }

                // now the shard text starts with shard_start and ends in shard_end
                *shard_end=0;
                // find shard or error out if not found
                int soft_compare=0;
                char *existing_shard_text=find_query_shard (shard_name, &soft_compare, cname, lnum);
                if (existing_shard_text==NULL)
                {
                    _cld_report_error( "Shard [%s] is not defined, line [%d], reading file [%s]\n\n", shard_name, lnum, cname);
                }
                else
                {
                    // check if shard has only been declared or actually defined with the text
                    // shard must be declared first before being used
                    if (existing_shard_text[0] == 0)
                    {
                        // 0 for soft_shard doesn't mean anything,if it is existing shard text, it won't be set
                        add_query_shard(shard_name, shard_start, 0, cname,lnum);
                    }
                    else 
                    {
                        // this is shard being used after being defined, and now it must match its definition
                        int match=0;
                        if (soft_compare==0)
                        {
                            match=!strcmp(existing_shard_text,shard_start);
                        }
                        else
                        {
                            // for soft-compare, remove parameters before comparing
                            char *existing_copy=cld_strdup(existing_shard_text);
                            char *new_copy=cld_strdup(shard_start);
                            remove_sql_params(existing_copy, cname, lnum);
                            remove_sql_params(new_copy, cname, lnum);
                            match=!strcmp(existing_copy,new_copy);
                        }
                        if (match==0)
                        {
                            _cld_report_error( "Shard [%s] does not match previous definition, found [%s], previous definition is [%s], line [%d], reading file [%s]\n\n", shard_name, shard_start, existing_shard_text, lnum, cname);
                        }
                    }
                }
                
                // remove the end tag
                memmove(shard_end, shard_end+shard_end_tag_len, strlen(shard_end+shard_end_tag_len)+1);
                // remove the begin tag
                memmove(begin, shard_start, strlen(shard_start)+1);

                // continue scanning from the beginning of the shard, since it can have other <?..?> in it
                curr = begin;
                // now continue normally
                continue;
            }

            
            // +2 to get passed <?
            param = cld_strdup(begin+2); 

            // check if param empty. 
            if (param[0]==0)
            {
               _cld_report_error( "Parameter cannot be empty, line [%d], reading file [%s]\n\n", lnum, cname);
            }
            // check if this is query-fragment
            const char *qf = "query-fragment#";
            int qf_len = strlen(qf);
            if (!strncmp(param,qf, qf_len))
            {
                // get fragment name
                char *frag_name=param+qf_len;
                int frag_name_len=strlen(frag_name);
                cld_trim(frag_name,&frag_name_len);
                // find fragment or error out if not found
                char *frag=find_query_fragment (frag_name, cname, lnum);
                // length of new query is current + frag, with cushion for zero due to <??>
                int new_text_len=curr_qry_len+strlen(frag); // we have <??> as cushions for zero at the end
                char *new_text=cld_malloc(new_text_len);
                // delineate first part
                *begin=0;
                // where to start searching for <? in new query
                int distance_to_fragment = (begin-qry);
                // construct new query - up to <?, then fragment, then after ?>
                // qry is capped at *begin being 0, so we can do this
                snprintf(new_text,new_text_len-1,"%s%s%s", qry,frag,end+2);
                // set new query
                qry=new_text;
                // set new beginning where to continue searching - this is in the fragment as it can have <??> in it!
                curr=qry+distance_to_fragment; // start from query, as it can have <??> in it!
                continue;
            }

            //
            // This is input parameter processing. We just put '%s' instead of <?..?> and the contents of <?..?> becomes the input 
            // parameter, so that run-time SQL construction works properly, in a printf-like manner, but with all kinds of checks and
            // enhancements, such as preventing SQL injection.
            //
            // put in %s where <?...?> was
            strcpy(begin,"'%s'");
            // compact query. Since no <??> is not allowed (no empty param, we checked this above), it means it has to be
            // at least <?X?>. In this case, begin is '%s'> and begin+4 is the last >, and end+2 is the first char after >, which is the continuation
            // of SQL. So the SQL is formed correctly. 
            // len is length of curr. end+2 is the source of copy. end+2-curr is 0-based position of source of copy in curr. len-(end+2-curr)
            // is the number of characters to the end. And +1 to copy zero char at the end.
            // Query gets shorter each time, so no memory violation here.
            int to_copy=(len - (end+2-curr))+1;
            // MUST use memmove since there is an overlap
            memmove(begin+4,end+2,to_copy);
            //strcpy(begin+4,cld_strdup(end+2));
            // start looking for next
            curr=begin+4;
            // add parameter
            params[param_count++]=param;
            if (param_count>=CLD_MAX_QUERY_INPUTS)
            {
                _cld_report_error( "\n\nToo many input variables (<?...?>) in a query, limit [%d], line [%d], reading file [%s]\n\n", CLD_MAX_QUERY_INPUTS,  lnum, cname);
            }

        }
    }

    int tot_inp = cld_count_substring (qry, "%s"); // compute total # of inputs
                        // to the query
    // check there are no %s anywhere, if we use <?..?> we can NOT used %s as well.
    if (is_compact!=NULL && tot_inp!=param_count)
    {
        _cld_report_error( "\n\nQuery cannot have both %%s and <?...?>, line [%d], reading file [%s]\n\n",  lnum, cname);
    }
    if (tot_inp >= CLD_MAX_QUERY_INPUTS)
    {
        _cld_report_error( "\n\nToo many string variables (%%s's or <?...?>'s) in a query, limit [%d], line [%d], reading file [%s]\n\n", CLD_MAX_QUERY_INPUTS,  lnum, cname);
        
    }
    int ql = strlen(qry);

    // 
    // We do not touch % signs, because we do NOT use snprintf like function at run time in cld_make_SQL!
    // '%s' will be replaced with input parameters, others won't be touched
    //

    // Sanity check and error
    if (qry[0] != 0 && qry[ql - 1] == ';')
    {
        _cld_report_error( "\n\nQuery cannot end with a semicolon, line [%d], reading file [%s]\n\n", lnum, cname);
    }

    // actually add final query into the array of queries, curr_query is query ID
    int curr_query = initialize_query (gen_ctx, cld_strdup (qry_name), qry, tot_inp);

    
    if (is_compact!=NULL)
    {
        gen_ctx->qry[curr_query].is_qry_compact=1;
        // save params so we can set them later, when get_next_input_params in called
        int i;
        //qry_total_inputs is exact param_count
        assert(param_count==gen_ctx->qry[curr_query].qry_total_inputs);
        for (i=0;i<gen_ctx->qry[curr_query].qry_total_inputs;i++)
        {
            // gen_ctx->qry[].compact_params[] will be used in add_input_param and for that,
            // all input must be de-escaped (escaping is only for source code parsing!, we cannot
            // have backslashes in the actual data)
            int par_len = strlen (params[i]); 
            cld_replace_string (params[i], par_len+1, "\\\"", "\"", 1, NULL); // de-escape
            gen_ctx->qry[curr_query].compact_params[i]=params[i];
        }
    }

}

// 
// Find something before the first quote. mtext is the text to search. msize is its length.
// 'what' is a string containing characters to search for BEFORE the first quote.
// Returns number of characters from mtext to one of 'what' characters, or strlen(mtext) if none from 'what' 
// characters found, such that mtext+<result> is one character after 'what' character.
// It allows to position to one character after one of 'what' characters OR to the null character after the
// string if none found.
//
int find_before_quote (char *mtext, int msize, char *what)
{
    assert (mtext);
    assert (what);

    int l = strlen(mtext);
    char *q = strchr (mtext, '"');
    int p = strcspn (mtext, what);
    if (q == NULL) 
    { 
        if (p == l)
        {
            return msize;
        }
        return p;
    }
    else
    {
        if (mtext+p>=q) 
        {
            return msize;
        }
        else return p;
    }
}

// 
// Returns 1 if this is DML query. gen_ctx is context, qry_name is ID of query,
// and output parameter is_insert is set to 1 if this is INSERT.
// Otherwise return 0.
//
int is_query_DML (cld_gen_ctx *gen_ctx, int qry_name, int *is_insert)
{
    assert (gen_ctx);
    if (!strncasecmp (gen_ctx->qry[qry_name].text, "insert", strlen ("insert")) ||
            !strncasecmp (gen_ctx->qry[qry_name].text, "update", strlen ("update")) ||
            !strncasecmp (gen_ctx->qry[qry_name].text, "set", strlen ("set")) ||
            !strncasecmp (gen_ctx->qry[qry_name].text, "delete", strlen ("delete")))
    {
        if (is_insert != NULL)
        {
            if (!strncasecmp (gen_ctx->qry[qry_name].text, "insert", strlen ("insert"))) *is_insert = 1; 
            else *is_insert = 0;
        }
        return 1;
    }
    return 0;
}
 

// 
// Find query ID if given name 'query_name'. gen_ctx is the context.
// Returns query ID or -1 if none found..
//
int find_query (cld_gen_ctx *gen_ctx, const char *query_name)                        
{
    assert (gen_ctx);
    assert (query_name);

    int k;
    for (k = 0; k < gen_ctx->total_queries; k++)
    {
        if (!strcmp (gen_ctx->qry[k].name, query_name))
        {
            return k;
        }
    }
    return -1;
}

// 
// Get column information. Column is col from table tab. We get maximum length (out_max_len), precision (out_numeric_precision), scale
// (out_numeric_scale), type (out_data_type). fname/lnum is the file/line of source where this query takes place. gen_ctx is the context.
// This isn't cached as we do this only once.
// Errors out if column doesn't exist or cannot get column information.
//
void get_col_info (cld_gen_ctx *gen_ctx, const char *tab, const char *col, char **out_max_len, char **out_numeric_precision, char **out_numeric_scale, char **out_data_type, const char *fname, int lnum)
{
    // unused for now, may be used in the future
    CLD_UNUSED (gen_ctx);

    assert (tab);
    assert (col);
    assert (fname);

    int snrow;
    int sncol;
    char check_col_query[500];
    snprintf (check_col_query, sizeof (check_col_query)-1, "select numeric_precision,numeric_scale,character_maximum_length, data_type from information_schema.columns where table_schema=database() "
        " and table_name='%s' and column_name='%s'", tab, col);
    char **sdata = NULL;
    CLD_TRACE ("sample query [%s]", check_col_query);
    
    char **col_names;

    cld_select_table (check_col_query, &snrow, &sncol, &col_names, &sdata);
    if (snrow == 0)
    {
        _cld_report_error( "Column name [%s] does not exist in table [%s], reading file [%s] at line [%d]", col, tab, fname, lnum);
    }

    cld_iter it;
    char *d = NULL;
    int br = -1;

    cld_data_iterator_init (&it, sdata, snrow, sncol);

    char *maxLen = NULL;
    char *data_type = NULL;
    char *numeric_precision = NULL;
    char *numeric_scale = NULL;

    d = cld_data_iterator_next(&it, &br);
    if (d) numeric_precision = d; else
    {
        _cld_report_error( "Cannot get metadata for table [%s], reading file [%s] at line [%d]", tab, fname, lnum);
    }
    d = cld_data_iterator_next(&it, &br);
    if (d) numeric_scale = d; else
    {
        _cld_report_error( "Cannot get metadata for table [%s], reading file [%s] at line [%d]", tab, fname, lnum);
    }
    d = cld_data_iterator_next(&it, &br);
    if (d) maxLen = d; else
    {
        _cld_report_error( "Cannot get metadata for table [%s], reading file [%s] at line [%d]", tab, fname, lnum);
    }
    d = cld_data_iterator_next(&it, &br);
    if (d) data_type = d; else
    {
        _cld_report_error( "Cannot get metadata for table [%s], reading file [%s] at line [%d]", tab, fname, lnum);
    }

    if (out_max_len != NULL)
    {
        CLD_STRDUP (*out_max_len, maxLen);
    }
    if (out_data_type != NULL)
    {
        CLD_STRDUP (*out_data_type, data_type);
    }
    if (out_numeric_precision != NULL)
    {
        CLD_STRDUP (*out_numeric_precision, numeric_precision);
    }
    if (out_numeric_scale != NULL)
    {
        CLD_STRDUP (*out_numeric_scale, numeric_scale);
    }

    cld_free (sdata); // we don't need actual data, just a list of columns

}

// 
// Change pointer to string pointer s, so it advances until either 1) a comma is encountered or 2) the end of the string
// The pointer is now at either one
//
void get_until_comma (char **s)
{
    assert (*s != NULL);
    int i = 0;
    while (((*s)[i])!=',' && (*s)[i]!=0) i++;
    *s = *s + i;
}

// 
// Change pointer to string pointer s, so it advances until either 1) a whitespace is encountered or 2) the end of the string
// The pointer is now at whitespace
//
void get_until_whitespace (char **s)
{
    assert (*s != NULL);
    int i = 0;
    while (!isspace ((*s)[i])) i++;
    *s = *s + i;
}

// 
// Change pointer to string pointer s, so it advances until either 1) a non-whitespace is encountered or 2) the end of the string
// The pointer is now non-whitespace (presumably after skipping bunch (or zero) of whitespaces)
//
void get_passed_whitespace (char **s)
{
    assert (*s != NULL);
    int i = 0;
    while (isspace ((*s)[i])) i++;
    *s = *s + i;
}



//
// Recognition of a markup. cinp is the line read from source file, pos is the current byte position in it (from where to 
// search for a markup), opt is the markup itself (a string to search for). Outputs mtext (which is the pointer to just after
// the markup), msize is the length of it. isLast is 1 if this markup should NOT have any data after it, 0 if it should.
// fname/lnum is the file/line of source where this query takes place.
// Returns byte position in cinp right after where markup is found.
// Errors out if there is an unterminated string after markup, or if markup closing isn't there (either end of string or ?>)
//
int recog_markup (char *cinp, int pos, char *opt, char **mtext, int *msize, int isLast, const char *fname, int lnum)
{
    assert (cinp);
    assert (opt);
    assert(msize);
    assert(fname);


    // do not assign mtext in case of not-found, because it could override the found one when
    // we check for multiple recog_markup() in the same if(), same for msize
    int opt_len = strlen (opt);
    int orig_position = pos;
    if (!memcmp (cinp+pos, opt, opt_len))
    {
        // for comment, any markup can be inside comment, so the whole line is passed
        // // would be faster to have a flag passed to the function if it is comment or not
        if (!strcmp(opt,"//")) 
        {
            *mtext=cinp+pos+opt_len;
            *msize=0;// no size for comment
            CLD_VERBOSE(lnum, "Beginning of comment");
            return pos+strlen(cinp)+1;
        }

        //
        // In 'verbatim' mode, we just copy everything out (to web output or to string) regardless of markup
        // EXCEPT for comment (//) - comments are NOT output.
        // The only other exception obviously has to be 'end-verbatim' which we cannot ignore structuraly in order
        // to end the verbatim block.
        // We ignore all markup: if what's found is NOT end-verbatim and start-verbatim is in effect.
        // If what's found IS end-verbatim, then we HAVE to recognize it in order to end the verbatim block, regardless
        // of whether start-verbatim is in effect or not.
        //
        if (strcmp (opt, "end-verbatim") && is_verbatim == 1) return 0;

        *mtext = cinp+pos+opt_len;
        pos += opt_len;
        if (isLast == 1)
        {
            //
            // Nothing is expected after this markup, i.e. no text
            //
            while (isspace(cinp[pos])) pos++;
            // we consider end of line to be the same as ?> 
            // we already handle \ (line continuation) before we encounter the first recog_markup
            // (this is done during reading from the C file)
            if (cinp[pos] == 0 || !memcmp (cinp+pos, "?>", 2))
            {
                usedCLD = 0;
                *msize = (cinp+pos)-*mtext;
                (*mtext)[*msize] = 0; // zero the ?>
                CLD_VERBOSE(lnum,"Markup [%s] found", opt);
                return pos + 1; // cinp[pos+1] must be '>' in the last '?>', i.e. the last char
            }
            else
            {
                return 0; // there can be else versus else-if : do not say 'extra characters...'
            }
        }
        else
        {
            //
            // After this markup, we expect more text
            // We expect either space or # after markup keyword
            // We also don't look for markup ending (i.e. ?>) if it is quoted, such as 
            // for example with query text.
            // We will look for \" inside a string (an escaped quote), because C code might have this, so an
            // escaped quote is NOT end of the string. This is useful primarily in C code (c markup).
            // We do NOT interpret \", it is specified
            // so in the string and it remains so (i.e. we don't switch \" for " or anything at all). 
            // This does not affect SQL queries (think injection) because we this affects only the code
            // programmer writes, and we also use ANSI single quotes for all sessions.
            //
            int inStr = 0;
            do
            {
                // escaped quote is not end or beginning of a string
                if (cinp[pos] == '"' && (pos==0 || cinp[pos-1]!='\\'))
                {
                    if (inStr == 1) inStr = 0;
                    else inStr = 1;
                }
                else if (inStr == 0)
                {
                    if (cinp[pos] == 0 || !memcmp (cinp+pos, "?>", 2))
                    {
                        *msize = (cinp+pos)-*mtext;
                        // if opt is empty, the whole cld marker is c code, just pass it back
                        if (opt[opt_len-1] != '#')
                        {
                            // one exception for expecting space after keyword
                            // is qry...#, i.e. when keyword ends with #
                            //
                            // in case some constructs are subsets of other (such as else versus else-if)
                            // we expect a space after it (since it's not the last!)
                            if (cinp[orig_position+opt_len] != ' ')
                            {
                                return 0; // must have space afterward
                            }
                        }
                        usedCLD = 0;
                        (*mtext)[*msize] = 0; // zero the ?>
                        CLD_VERBOSE(lnum,"Markup [%s] found", opt);
                        return pos + 1;
                    }
                }
            } while (cinp[pos++] != 0);

            if (inStr == 1)
            {
                _cld_report_error( "Unterminated string in markup '%s', reading file [%s] at line [%d]", opt, fname, lnum);
            }
            else
            {
                // this branch should never happen since we consider end of string to be the end of markup. Here it is anyway just in case.
                _cld_report_error( "Terminating '?>' not found in markup '%s', reading file [%s] at line [%d]", opt, fname, lnum);
            }
        }
    }
    return 0;
}


// 
// Initialize context gen_ctx. Initialize command mode, all queries, query stack, string writing balancer
// and database location.
//
void init_cld_gen_ctx (cld_gen_ctx *gen_ctx)
{
    CLD_TRACE("");

    gen_ctx->cmd_mode = 0;
    int i;
    int j;
    for (j = 0; j < CLD_MAX_QUERY; j++) 
    {
        gen_ctx->qry[j].is_qry_compact=0;
        gen_ctx->qry[j].qry_total_inputs = 0;
        gen_ctx->qry[j].is_dynamic = 0;
        gen_ctx->qry[j].is_prepared = 0;
        gen_ctx->qry[j].is_DML = 0;
        gen_ctx->qry[j].is_insert = 0;
        for (i = 0; i < CLD_MAX_QUERY_INPUTS; i++)  
        {
            gen_ctx->qry[j].qry_inputs[i][0] = 0;
            gen_ctx->qry[j].qry_is_input_str[i] = 0;
            gen_ctx->qry[j].compact_params[i]=NULL;
        }
    }

    int k;
    for (k = 0; k < CLD_MAX_QUERY_NESTED; k++)
    {
        gen_ctx->global_qry_stack[k] = -1;
    }


    gen_ctx->total_queries = 0;

    gen_ctx->curr_qry_ptr = 0;

    gen_ctx->total_write_string = 0;
    gen_ctx->db = "";


}


// 
// Initialize query once the text of query has been processed (for all <?..?> markup inside the query text)
// gen_ctx is the context, 'name' is the name of query, 'text' is query text (i.e. SQL), and to_inputs,
// tot_inputs is the number of input parameters to this query.
// Note that query already exists - and we will find here what is the ID of the query. What we do here is set
// status of query (UNUSED), initialize number of input parameter, set is_DML flag, increase total number of
// queries (if needed) and such. Other things about the query, such as query input parameters, are already set.
//
int initialize_query (cld_gen_ctx *gen_ctx, const char *name, const char *text, int tot_inputs)
{
    CLD_TRACE("");
    assert (gen_ctx);
    assert (name);
    assert (text);

    int i;
    // qry is 'which query is this in the array of queries', it is either total_queries (if we add a new one)
    // or one of the existing queries (if redefined)
    int qry=gen_ctx->total_queries;
    int was_added = 1;
    for (i = 0; i < gen_ctx->total_queries; i++)
    {
        if (!strcmp (name, gen_ctx->qry[i].name))
        {
           qry=i;
           // we used to error out when the same query was redefined. No more, because now 
           // we can change the text of the query multiple times for quasi-dynamic queries (but still static)
           // (i.e. change text definition depending on the run-time code conditionals such as with start-query#)
           was_added = 0; // in this case, query was NOT added, we redefined existing query
           break;
        }
    }
    gen_ctx->qry_active[qry] = CLD_QRY_UNUSED;
    gen_ctx->qry[qry].text = cld_strdup (text);
    if (was_added == 1) gen_ctx->qry[qry].name = cld_strdup(name); // name always stays the same if we redefine, obviously
    gen_ctx->qry[qry].qry_total_inputs =  tot_inputs;
    gen_ctx->qry[qry].is_DML = is_query_DML (gen_ctx, qry, &(gen_ctx->qry[qry].is_insert));
    // only up the number of queries if this is a new query
    if (was_added==1)
    {
        gen_ctx->total_queries++;
        if (gen_ctx->total_queries >= CLD_MAX_QUERY)
        {
            _cld_report_error("Too many queries specified");
        }
    }
    return qry;
}



// 
// Main code generation function. gen_ctx is the context, file_name is the file
// that is being processed.
// Errors out if cannot open file and for many other reasons (including syntax and 
// other errors in markup). 
// Generates the code which is output according to the options used (see the documentation).
//
void cld_gen_c_code (cld_gen_ctx *gen_ctx, const char *file_name)
{
    FILE *f;

    f = fopen (file_name, "r");
    CLD_VERBOSE(0,"Starting");
    if (f == NULL)
    {
        _cld_report_error( "Error opening file [%s]", file_name);
    }

    // 
    // Open shard so to be able to use shard definitions from a single file in any source file
    //
    CLD_VERBOSE(0,"Opening shard file");
    FILE *f_shard=fopen("shard","r");
    if (f_shard!=NULL)
    {
        char shard_text[8000];
        int shard_lnum=0;
        while (1)
        {
            if (fgets (shard_text, sizeof (shard_text) - 1, f_shard) != NULL)
            {
                shard_lnum++;
                int len = strlen (shard_text);
                cld_trim (shard_text, &len);
                if (shard_text[0]==0) continue;
                if (shard_text[0]=='/' && shard_text[1]=='/') continue;

                char *eq=strchr(shard_text,'=');
                if (eq==NULL)
                {
                    _cld_report_error( "Shard missing equal sign, must be in the form of define-[soft]-shard#name=<SQL-shard>, found [%s], line [%d]",shard_text, shard_lnum);
                }
                *eq=0;

                char *sql=eq+1;
                len = strlen (sql);
                cld_trim (sql, &len);
                if (sql[0]!='"' || sql[len-1]!='"')
                {
                    _cld_report_error( "Shard must be double-quoted, found [%s], line [%d]",eq+1, shard_lnum);
                }
                sql[len-1]=0;
                sql++;

                len = strlen (shard_text);
                cld_trim (shard_text, &len);

                int is_soft=0;
                char *pound=strchr(shard_text,'#');
                if (pound==NULL)
                {
                    _cld_report_error(CLD_MSG_SHARD,shard_text);
                }
                else
                {
                    *pound = 0;
                    if (!strcmp(shard_text,"define-shard"))
                    {
                        is_soft=0;
                    }
                    else if (!strcmp(shard_text,"define-soft-shard"))
                    {
                        is_soft=1;
                    }
                    else
                    {
                        _cld_report_error(CLD_MSG_SHARD,shard_text);
                    }

                }
                CLD_VERBOSE(0,"Adding shard [%s], sql text [%s] is soft [%d]", pound+1, sql, is_soft);
                add_query_shard(pound+1, sql, is_soft, "shard", shard_lnum);
            }
            else break;
        }
        fclose(f_shard);
    }

    //
    // Start reading line by line from the source file
    //
    char line[CLD_FILE_LINE_LEN + 1]; // buffer for each line from HTML file
    char *res;
    int query_id = -1; // query ID currently worked on
    int lnum = 0; // current line worked on in HTML file

    // 
    // setup macros so that error reporting shows source file names and the
    // corresponding line numbers and NOT the generated C code
    //
#undef CLD_FILE
#undef CLD_LINE
#define CLD_FILE file_name
#define CLD_LINE lnum
    CLD_VERBOSE(0,"Opened your file [%s]",file_name);

    int non_cld = 0; // is current line all vm
    int cld_mode = 0; // treat as vm code

    int open_ifs = 0;
    int open_for = 0;
    int open_queries = 0;
    int line_len = 0; // unlike 'len' below, used only for concatenation of lines
    int is_c_block = 0;
    int is_comment_block = 0;

    char last_char_printed = 0;


    // 
    // Main loop in which lines are read from the source file
    //
    while (1)
    {
        non_cld = 0; // non-vm chars, for purpose of getting extra-new-lines out
        res = fgets (line + line_len, sizeof (line) - line_len- 1, f);
        if (res == NULL) // either end or error
        {
            int err = ferror (f);
            if (err) // if error, quit
            {
                _cld_report_error( "Error [%s] reading file [%s]", strerror (errno), file_name);
            }
            break; // nothing read in, exit normally
        }
        lnum++;

        // 
        // Setup line number for error reporting. Print new line in front
        // so if we forget to finish a line with new line, this would ruin
        // C syntax.
        //
        oprintf("\n#line %d \"%s\"\n", lnum, file_name);

        // 
        // if this is continuation, trim the continuation to avoid big gaps in between pieces
        // Lines can be concatenated
        //
        if (line_len > 0)
        {
            int cont_len = strlen (line+line_len);
            // check if continuation starts with space. If it doesn't, we cannot trim.
            if (cont_len > 1 && isspace(line[line_len])) 
            {
                // We used to leave a space when lines are concatenated with / but NO MORE. The space
                // should be made in the code, if needed. The reason is that space-sensitive parsing will NOT 
                // work if we arbitrarily leave a space in between lines. Concatenation means just that - concatenation.
                // form incorrect syntax because there would be no spaces at all in between them
                cld_trim (line+line_len, &cont_len);
            }
        }

        int i;
        int len = strlen (line);

        if (len >= (int)sizeof (line) - 2) // don't allow too big of a line
        {
            _cld_report_error( "Line too long, reading file [%s] at line [%d]", file_name, lnum);
        }

        cld_trim (line, &len);

        if (cld_mode == 1)
        {
            if (len > 0 && line[len - 1] == '\\')
            {
                // continuation of the line
                line[len - 1] = 0;
                line_len = len - 1;
                continue; // read more
            }
        }
        line_len = 0;

        CLD_VERBOSE(lnum,"Got [%s]", line);

        // 
        // Initially, the boundary for markup code was CLD_BEGIN and that still stays
        // It may be useful if there is actual comments in C code that start with '<'
        //
        char *cld_begin = "/*" "CLD_BEGIN";
        int cld_begin_len = strlen (cld_begin);
        char *cld_begin1 = "/*" "<";
        int cld_begin_len1 = strlen (cld_begin1);
        char *cld_end = "CLD_END" "*/";
        int cld_end_len = strlen (cld_end);
        char *cld_end1 = ">" "*/";
        int cld_end_len1 = strlen (cld_end1);
        int is_long_cld = 0;
       
        // If CLD_BEGIN alone on the line, we will set cld_mode to 1 and continue to next line
        // If not alone, we will set cld_mode to 1 and continue to parse this line.
        // If CLD_END at the end of line, we will continue to parse this line, but set cld_mode to 0
        // so the next line isn't parsed.
        // If CLD_END is the whole line, we will set cld_mode to 0 and continue to next line
        // CLD_BEGIN must be the beginning of line, CLD_END must be the end of line
        //
        // since we examine the beginning of the line, it cannot happen that short and long begin would happen
        // the same, only short or long can happen, and they don't have to match for
        // the beginning and ending
        if ((is_long_cld=!strncasecmp (line, cld_begin, cld_begin_len)) || !strncasecmp (line, cld_begin1, cld_begin_len1))
        {
            if (cld_mode == 1)
            {
                _cld_report_error( "Already in %s code, cannot begin again, reading file [%s] at line [%d]", TOOL, file_name, lnum);
            }
            cld_mode = 1; // set for the next line
            memmove (line, (is_long_cld==1 ? line+cld_begin_len : line+cld_begin_len1), (is_long_cld==1? len-cld_begin_len:len-cld_begin_len1) +1); // continue parsing vm after the start
            len = strlen (line); // update length, no need for trim, since already done
            if (line[0] == 0) continue; // if this if CLD_BEGIN alone, just continue
        }

        // It is possible to have CLD_BEGIN and CLD_END on the same line. In this case we want to parse this line as CLD
        // but after continue non-CLD (i.e. copy to output only). This is why this IF is not under 'else' of the previous IF.
        if ((is_long_cld=!strcasecmp (line + len - cld_end_len, cld_end)) || !strcasecmp (line + len - cld_end_len1, cld_end1))
        {
            if (cld_mode == 0)
            {
                _cld_report_error( "%s code ending, but never found '<' to begin code, reading file [%s] at line [%d]", TOOL, file_name, lnum);
            }
            cld_mode = 0; // set for the next line
            *(line + len - (is_long_cld == 1 ? cld_end_len : cld_end_len1)) = 0;
            len = strlen (line);
            cld_trim (line, &len); // possible it's just an empty CLD_BEGIN ... CLD_END
            // at this point cld_mode is 0, but we will continue and parse this line, since it is CLD.
            // after this line, it's no longer CLD, that is when cld_mode = 0 takes effect.
            if (line[0] == 0) continue; // if this if CLD_END alone, just continue
        }
        else if (cld_mode == 0) // This check is under 'else' because if CLD_BEGIN ... CLD_END on the same line,
                    // we need to parse this line as CLD, and then continue with next line after it's parsed. 
        {
            CLD_VERBOSE(lnum,"Just copy out [%s]", line);
            oprintf("%s\n", line);
            continue;
        }

        char query_id_str[CLD_MAX_QUERYNAME_LEN + 1]; // query ID being worked on

        // if C code, do not start any printing (and see code below, after 'for (i=0;i<len;i++)')
        // and we won't print out any line closures either (meaning finish the string in line_track()), while C block is in effect
        // The same for comments.
        if (is_c_block == 0 && is_comment_block == 0)
        {
            BEGIN_TEXT_LINE
            // start printing out any HTML that comes along
                            // unless this is a C block
        }

        char *mtext = NULL;
        int msize = 0;
        int first_on_line = 1;
        usedCLD = 0;


        //
        // In this loop, a line is examined char by char. However, once certain patterns are recognized, parsing
        // progresses more rapidly. Basically, recog_markup() is called to recognized the beginning of the CLD markup,
        // and from there the rest is parsed in a more rapid fashion. Each markup is checked, and once recognized and
        // processed, the next line is read in the outer loop.
        //
        for (i = 0; i < len; i++)
        {
            int newI = 0;
            int newI1 = 0;
            int newI2 = 0;
            int newI3 = 0;
            int newI5 = 0;
            int newI6 = 0;
            int newI7 = 0;
            int newI8 = 0;
            int newI9 = 0;
            int newI10 = 0;

            if (!memcmp (line + i, "<?", 2)) // short directive
            {
                i = i + 2; // past "<?"
                usedCLD = 1;
            }
            else
            {
                ; // else? nothing. We consider new line start within CLD_BEGIN/CLD_END block to be the start
                // of the <?cld markup?>, for simplicity of coding.
            }


            // checking for CLD construct without <?..?> can only work in those constructs where they are the first on the 
            // line, OR if this is actual markup <? ...?>
            if (first_on_line == 1 || usedCLD == 1)
            {
                while (isspace (line[i])) i++;
                first_on_line = 0;
                    
                if (is_comment_block == 1)
                {
                    // 
                    // while comments, check for end-comment.
                    //
                    if ((newI=recog_markup (line, i, "end-comment", &mtext, &msize, 1, file_name, lnum)) != 0) 
                    {
                        i = newI;

                        is_comment_block = 0;
                        BEGIN_TEXT_LINE
                        // since we finished the line above, start a new
                                    // this is in case there is something on the same line after <?end-comment?>
                                    // and if there isn't, it will be just an empty line which is removed later
                        continue; 
                    }

                    // this is if not end-comment

                    // check for start-comment on one line and then some Comment and end-comment on the same line
                    // Note: if start-comment starts on one line, then all comment and then end-comment (and maybe some more after that)
                    // then we will NOT be here, instead we will ignore char by char right after this i=0;i<len;i++ 'for' loop
                    // and all comments will be ignored that way, and then we will encounter end-comment above (right after checking
                    // is_comment_block ==1) and then continue to process afterwards, so for example <?start-comment?>THis is comment<?end-comment?>XY<?print-web z?>
                    // would work fine. This right below is ONLY if there start-comment on one line and some comment then <?end-comment?> on that line.
                    // This means there can't be a string or anything in comment that has '<?end-c?> in it.
                    char *is_end_on_line=strstr(line+i, "<?end-comment?>");
                    if (is_end_on_line != NULL)
                    {
                        *is_end_on_line=0;
                        // comment the comments
                        oprintf("// %s\n",line+i); 
                        // just like in recog_markup, 'i' must be the very last '>', and NOT the character just after it
                        i+=(is_end_on_line-(line+i))+strlen("<?end-comment?>")-1;
                        is_comment_block = 0;
                        BEGIN_TEXT_LINE
                        continue; 
                    }
                    // this is just comment on the line, print it
                    oprintf("// %s\n", line+i);
                    i = len;
                    break;
                }
                else if (is_c_block == 1)
                {
                    // 
                    // while print out C code, check for end-c.
                    //
                    if ((newI=recog_markup (line, i, "end-c", &mtext, &msize, 1, file_name, lnum)) != 0) 
                    {
                        i = newI;

                        is_c_block = 0;
                        BEGIN_TEXT_LINE
                        // since we finished the line above, start a new
                                    // this is in case there is something on the same line after <?end-c?>
                                    // and if there isn't, it will be just an empty line which is removed later
                        continue; 
                    }

                    // this is if not end-c

                    // check for start-c on one line and then some C-code and end-c on the same line
                    // Note: if start-c starts on one line, then all C code and then end-c (and maybe some more after that)
                    // then we will NOT be here, instead we will print char by char right after this i=0;i<len;i++ 'for' loop
                    // and all C code will be printed out that way, and then we will encounter end-c above (right after checking
                    // is_c_block ==1) and then continue to process afterwards, so for example <?start-c?>a=b;<?end-c?>XY<?print-web z?>
                    // would work fine. This right below is ONLY if there is start-c on one line and some C code then <?end-c?> on that line.
                    // This means there can't be a string or anything in C code that has '<?end-c?> in it.
                    char *is_end_on_line=strstr(line+i, "<?end-c?>");
                    if (is_end_on_line != NULL)
                    {
                        *is_end_on_line=0;
                        oprintf("%s\n",line+i); // print C code print to <?end-c?>
                        // just like in recog_markup, 'i' must be the very last '>', and NOT the character just after it
                        i+=(is_end_on_line-(line+i))+strlen("<?end-c?>")-1;
                        is_c_block = 0;
                        BEGIN_TEXT_LINE
                        continue; 
                    }
                    // this is just plain C code on the line, print it
                    oprintf("%s\n", line+i);
                    i = len;
                    break;
                }
                else if ((newI=recog_markup (line, i, "end-verbatim", &mtext, &msize, 1, file_name, lnum)) != 0) 
                {
                    i = newI;
                    END_TEXT_LINE

                    if (is_verbatim != 1)
                    {
                        _cld_report_error( "Encountered end-verbatim without start-verbatim, reading file [%s] at line [%d]", file_name, lnum);
                    }

                    is_verbatim = 0;
                    BEGIN_TEXT_LINE
                    continue; 
                }
                else if ((newI=recog_markup (line, i, "start-verbatim", &mtext, &msize, 1, file_name, lnum)) != 0) 
                {
                    i = newI;
                    END_TEXT_LINE

                    if (is_verbatim != 0)
                    {
                        _cld_report_error( "Encountered start-verbatim, but prior start-verbatim has already started, reading file [%s] at line [%d]", file_name, lnum);
                    }

                    is_verbatim = 1;
                    BEGIN_TEXT_LINE
                    continue; 
                }
                else if ((newI=recog_markup (line, i, "start-comment", &mtext, &msize, 1, file_name, lnum)) != 0) 
                {
                    i = newI;
                    END_TEXT_LINE

                    is_comment_block = 1;
                    continue; 
                }
                else if ((newI=recog_markup (line, i, "start-c", &mtext, &msize, 1, file_name, lnum)) != 0) 
                {
                    i = newI;
                    END_TEXT_LINE

                    is_c_block = 1;
                    continue; 
                }
                //
                // Up until now comment and C blocks were handled, because they are different from all other markups
                // which follow from this point on. All the following markups use the same recipe, calling recog_markup()
                // and advancing 'i' which points to the next segment to be parsed, while mtext/msize are the text of the 
                // markup (after the initial markup keyword). The 1 or 0 after msize tells if this markup is alone or if it
                // needs more data after the markup keyword. For example, end-query has nothing after the markup keyword 'end-query'
                // and so it is alone on the line and argument following msize is 1.
                //
                // Parsing done typically involves finding keywords and then dividing the string into smaller pieces (based on
                // the location of these keywords and the information contained between them or between such keywords and the end of string), typically
                // a single piece of information is to be found in each piece. It's a
                // parsing mechanism that's more suited for markup-like constructs, where such constructs are given through parameters
                // which control what is being done, on what data, and how. Examples of markups given in the documentation present the 
                // use scenarios. Markups are generally quite simple and more importantly are designed to be simple
                // and to streamline commonly used functionality in an 80/20 fashion. The 20% functionality is typically done in pure C, and
                // 80% functionality in simple markups. This tends to reduce not only the number of bugs, but also to force clearer and simpler
                // programming (while the code generate is still C and thus typically faster than most anything else).
                // 
                //
                else if ((newI=recog_markup (line, i, "end-query", &mtext, &msize, 1, file_name, lnum)) != 0) // is it end of query action block
                {
                    i = newI;

                    end_query (gen_ctx,  &query_id, &open_queries, 1, file_name, lnum);

                    continue; // skip the markup and continue analyzing 
                }
                else if ((newI=recog_markup (line, i, "//", &mtext, &msize, 0, file_name, lnum)) != 0)
                {
                    // Comment does NOT produce msize, it is 0, so msize NOT used in oprintf
                    i = newI;
                    END_TEXT_LINE
                    oprintf("//%s\n", mtext); 

                    BEGIN_TEXT_LINE
                    continue;
                }
                else if (((newI=recog_markup (line, i, "rollback-transaction", &mtext, &msize, 1, file_name, lnum)) != 0)) // rollback transaction
                {
                    i = newI;
                    END_TEXT_LINE
                    oprintf("cld_rollback ();\n");
                    BEGIN_TEXT_LINE
                    continue;
                }
                else if (((newI=recog_markup (line, i, "commit-transaction", &mtext, &msize, 1, file_name, lnum)) != 0)) // commit transaction
                {
                    i = newI;
                    END_TEXT_LINE
                    oprintf("cld_commit ();\n");
                    BEGIN_TEXT_LINE
                    continue;
                }
                else if (((newI=recog_markup (line, i, "begin-transaction", &mtext, &msize, 1, file_name, lnum)) != 0)) // begin transaction
                {
                    i = newI;
                    END_TEXT_LINE
                    oprintf("cld_begin_transaction ();\n");
                    BEGIN_TEXT_LINE
                    continue;
                }
                else if ((newI=recog_markup (line, i, "column-length", &mtext, &msize, 0, file_name, lnum)) != 0)   // this is column length
                {
                    i = newI;    

                    char arg_name[2*CLD_MAX_QUERY_INPUT_LEN+1];

                    int arg_len = msize;
                    if (arg_len >= (int)sizeof (arg_name) - 1)
                    {
                        _cld_report_error( "column-length name in arg markup is too long, reading file [%s] at line [%d]", file_name, lnum);
                    }
                    memcpy (arg_name, mtext, arg_len);
                    arg_name[arg_len] = 0;

                    cld_trim (arg_name, &arg_len);

                    char *as = strstr (arg_name, CLD_KEYAS);
                    char *asvar = NULL;
                    int is_defined = 0;
                    if (as != NULL)
                    {
                        *as = 0;
                        arg_len = strlen (arg_name);
                        cld_trim (arg_name, &arg_len);
                        asvar = as + strlen (CLD_KEYAS);
                        is_opt_defined (&asvar, &is_defined, file_name, lnum);
                    }



                    char *und = strchr (arg_name, '.');
                    if (und == NULL)
                    {
                        _cld_report_error( "Argument name in column-length markup must be in the form of table.column, reading file [%s] at line [%d]", file_name, lnum);
                    }
                    char *tab = arg_name;
                    *und = 0;
                    char *col = und + 1;
                    char *max_len;
                    char *data_type;
                    char *numeric_precision;
                    char *numeric_scale;
                    get_col_info (gen_ctx, tab, col, &max_len, &numeric_precision, &numeric_scale, &data_type, file_name, lnum);
                    char *mlen = "30"; // default max len
                    if (!strcmp (data_type, "varchar") || !strcmp(data_type,"char"))
                    {
                        mlen = max_len;
                    }
                    else if (!strcmp (data_type, "int") || !strcmp (data_type, "smallint") || !strcmp (data_type, "tinyint") || !strcmp (data_type, "bigint"))
                    {
                        mlen = numeric_precision;
                    }
                    else if (!strcmp (data_type, "decimal") || !strcmp(data_type,"double") || !strcmp(data_type,"float"))
                    {
                        char temp[30];
                        snprintf (temp, sizeof(temp)-1, "%d",atoi(numeric_precision)+1);
                        mlen = cld_strdup(temp);
                    }
                    else
                    {
                        _cld_report_error( "Unsupported data type for argument [%s.%s], data type [%s], reading file [%s] at line [%d]", tab, col, data_type, file_name, lnum);
                    }

                    if (asvar != NULL)
                    {
                        // this happens right in the run-time, as cld_printf is going on
                        END_TEXT_LINE
                        oprintf("%s%s = \"%s\";\n", is_defined == 1 ? "char *" : "" , asvar, mlen);
                        BEGIN_TEXT_LINE
                    }
                    else
                    {
                        // no new line here because column-length doesn't print but rather just outputs number
                        oprintf("%s", mlen); // print out column length (maximum)
                    }

                    continue;
                }
                else if ((newI=recog_markup (line, i, "use-no-result#", &mtext, &msize, 0, file_name, lnum)) != 0
                        || (newI1=recog_markup (line, i, "create-empty-row#", &mtext, &msize, 0, file_name, lnum)) != 0)
                {
                    //
                    // when we check for multiple markups at the same time, the return values of recog_markup() are either 
                    // all zero (no recognition) or one is non-zero and all others are zero (because only one can get recognized). 
                    // The return value is the offset to advance in the current text line. So in any case, the amount to 
                    // advance (if there was a match) is always the sum of all return values (all but one of them being zero).
                    //
                    i = newI + newI1;
                    int use_empty = (newI != 0 ? 1 :0);

                    // get length of query ID
                    int qry_dis_len = msize;
                    char qry_dis_id[CLD_MAX_QUERYNAME_LEN + 1]; 

                    // make sure we have space for it
                    if (qry_dis_len > (int)sizeof (qry_dis_id) - 1)
                    {
                        _cld_report_error( "Qry ID too long, reading file [%s] at line [%d]", file_name, lnum);
                    }

                    // obtain query ID
                    memcpy (qry_dis_id, mtext, qry_dis_len);
                    qry_dis_id[qry_dis_len] = 0;

                    cld_trim (qry_dis_id, &qry_dis_len);

                    int k = find_query (gen_ctx, qry_dis_id);
                    if (k == -1)
                    {
                        _cld_report_error( "Query [%s] is not found, reading file [%s] at line [%d]", qry_dis_id, file_name, lnum);
                    }

                    if (gen_ctx->qry[k].is_DML == 1)
                    {
                        _cld_report_error( "Query [%s] cannot create empty row or use no result for DML queries, which always have a result row, reading file [%s] at line [%d]", qry_dis_id, file_name, lnum);
                    }

                    END_TEXT_LINE
                    if (use_empty == 1)
                    {
                        oprintf("__qry_massage_%s = CLD_QRY_USE_EMPTY;\n", gen_ctx->qry[k].name);
                    }
                    else
                    {
                        oprintf("__qry_massage_%s = CLD_QRY_CREATE_EMPTY;\n", gen_ctx->qry[k].name);
                    }
                    BEGIN_TEXT_LINE
                    continue;
                }
                else if (((newI=recog_markup (line, i, "current-row#", &mtext, &msize, 0, file_name, lnum)) != 0))  // this is query row (number)
                {
                    i = newI;

                    // get length of query ID
                    int qry_row_len = msize;
                    char qry_row_id[CLD_MAX_QUERYNAME_LEN + 1]; 

                    // make sure we have space for it
                    if (qry_row_len > (int)sizeof (qry_row_id) - 1)
                    {
                        _cld_report_error( "Qry ID too long, reading file [%s] at line [%d]", file_name, lnum);
                    }

                    // obtain query ID
                    memcpy (qry_row_id, mtext, qry_row_len);
                    qry_row_id[qry_row_len] = 0;

                    cld_trim (qry_row_id, &qry_row_len);

                    char *as = strstr (qry_row_id, CLD_KEYAS);
                    char *asvar = NULL;
                    int is_defined = 0;
                    if (as != NULL)
                    {
                        *as = 0;
                        qry_row_len = strlen (qry_row_id);
                        cld_trim (qry_row_id, &qry_row_len);
                        asvar = as + strlen (CLD_KEYAS);
                        is_opt_defined (&asvar, &is_defined, file_name, lnum);
                    }

                    int k = find_query (gen_ctx, qry_row_id);
                    if (k == -1)
                    {
                        _cld_report_error( "Query [%s] is not found, reading file [%s] at line [%d]", qry_row_id, file_name, lnum);
                    }
                    if (gen_ctx->qry_active[k] != CLD_QRY_ACTIVE)
                    {
                        _cld_report_error( "Qry [%s] is used but not active, reading file [%s] at line [%d]", gen_ctx->qry[k].name, file_name, lnum);
                    }

                    END_TEXT_LINE

                    if (asvar != NULL)
                    {
                        // this happens right in the run-time, as cld_printf_noenc is going on
                        oprintf("snprintf (__current_row_%s, sizeof (__current_row_%s)-1, \"%%d\" , __iter_%s+1);\n", 
                            gen_ctx->qry[k].name, gen_ctx->qry[k].name, gen_ctx->qry[k].name);
                        oprintf("%s%s = __current_row_%s;\n", is_defined == 1 ? "char *" : "" , asvar, gen_ctx->qry[k].name);
                    }
                    else
                    {
                        oprintf("cld_printf (CLD_NOENC, \"%%d\", __iter_%s+1);\n", gen_ctx->qry[k].name); 
                    }
                    
                    BEGIN_TEXT_LINE
                    continue;
                }
                else if (((newI=recog_markup (line, i, "trim-query-input", &mtext, &msize, 1, file_name, lnum)) != 0))  
                {
                    i = newI;
                    END_TEXT_LINE
                    oprintf("cld_get_config()->ctx.trim_query_input = 1;\n");
                    BEGIN_TEXT_LINE

                    continue;
                }
                else if (((newI=recog_markup (line, i, "no-trim-query-input", &mtext, &msize, 1, file_name, lnum)) != 0))  
                {
                    i = newI;
                    END_TEXT_LINE
                    oprintf("cld_get_config()->ctx.trim_query_input = 0;\n");
                    BEGIN_TEXT_LINE

                    continue;
                }
                else if (((newI=recog_markup (line, i, "column-count#", &mtext, &msize, 0, file_name, lnum)) != 0))  // this is how many columns in result set
                                            // not obvious if doing select * from dynamic table!
                {
                    i = newI;
                    char *asvar;
                    int is_defined;
                    int k = get_query_id (gen_ctx, mtext, msize, file_name, lnum, &is_defined, &asvar);


                    // query must be either used or active, i.e. either inside the query or outside after
                    // this can be used outside the query loop, and it refers to the preceding query.
                    // if used, then this is not-active, so we're outside the query after it.
                    // Since we allow only one instance of a query, it is always correct
                    // (here the only other option is CLD_QRY_ACTIVE)
                    if (gen_ctx->qry_active[k] == CLD_QRY_UNUSED)
                    {
                        _cld_report_error( "Qry [%s] has never been used, reading file [%s] at line [%d]", gen_ctx->qry[k].name, file_name, lnum);
                    }
                    if (gen_ctx->qry[k].is_DML == 1)
                    {
                        _cld_report_error( "column-count cannot be used on query [%s] because it is a DML statement, reading file [%s] at line [%d]", gen_ctx->qry[k].name, file_name, lnum);
                    }

                    END_TEXT_LINE

                    if (asvar != NULL)
                    {
                        // we have variable in which to put row-count
                        // this happens right in the run-time, as cld_printf_noenc is going on
                        oprintf("snprintf (__column_count_%s, sizeof (__column_count_%s)-1, \"%%d\" , __ncol_%s);\n", 
                        gen_ctx->qry[k].name, gen_ctx->qry[k].name, gen_ctx->qry[k].name);
                        oprintf("%s%s = __column_count_%s;\n", is_defined == 1 ? "char *" : "",  asvar, gen_ctx->qry[k].name);
                    }
                    else
                    {
                        oprintf("cld_printf (CLD_NOENC, \"%%d\", __ncol_%s);\n", gen_ctx->qry[k].name); 
                    }
                    
                    BEGIN_TEXT_LINE
                    continue;
                }
                else if (((newI=recog_markup (line, i, "column-data#", &mtext, &msize, 0, file_name, lnum)) != 0))  // this is the array of column data
                                            // not obvious if doing select * from dynamic table!
                {
                    i = newI;
                    char *asvar;
                    int is_defined;
                    int k = get_query_id (gen_ctx, mtext, msize, file_name, lnum, &is_defined, &asvar);


                    // query must be either used or active, i.e. either inside the query or outside after
                    // this can be used outside the query loop, and it refers to the preceding query.
                    // if used, then this is not-active, so we're outside the query after it.
                    // Since we allow only one instance of a query, it is always correct
                    // (here the only other option is CLD_QRY_ACTIVE)
                    if (gen_ctx->qry_active[k] == CLD_QRY_UNUSED)
                    {
                        _cld_report_error( "Qry [%s] has never been used, reading file [%s] at line [%d]", gen_ctx->qry[k].name, file_name, lnum);
                    }
                    if (gen_ctx->qry[k].is_DML == 1)
                    {
                        _cld_report_error( "column-data cannot be used on query [%s] because it is a DML statement, reading file [%s] at line [%d]", gen_ctx->qry[k].name, file_name, lnum);
                    }

                    END_TEXT_LINE

                    if (asvar != NULL)
                    {
                        // we have variable in which to put column-names
                        oprintf("%s%s = __data_%s;\n", is_defined == 1 ? "char **" : "",  asvar, gen_ctx->qry[k].name);
                    }
                    else
                    {
                        _cld_report_error( "column-data in query [%s] cannot be used without 'as [define]' variable, i.e. the result must be assigned to a variable, reading file [%s] at line [%d]", gen_ctx->qry[k].name, file_name, lnum);
                    }
                    
                    BEGIN_TEXT_LINE
                    continue;
                }
                else if (((newI=recog_markup (line, i, "column-names#", &mtext, &msize, 0, file_name, lnum)) != 0))  // this is the array of column names
                                            // not obvious if doing select * from dynamic table!
                {
                    i = newI;
                    char *asvar;
                    int is_defined;
                    int k = get_query_id (gen_ctx, mtext, msize, file_name, lnum, &is_defined, &asvar);


                    // query must be either used or active, i.e. either inside the query or outside after
                    // this can be used outside the query loop, and it refers to the preceding query.
                    // if used, then this is not-active, so we're outside the query after it.
                    // Since we allow only one instance of a query, it is always correct
                    // (here the only other option is CLD_QRY_ACTIVE)
                    if (gen_ctx->qry_active[k] == CLD_QRY_UNUSED)
                    {
                        _cld_report_error( "Qry [%s] has never been used, reading file [%s] at line [%d]", gen_ctx->qry[k].name, file_name, lnum);
                    }
                    if (gen_ctx->qry[k].is_DML == 1)
                    {
                        _cld_report_error( "column-names cannot be used on query [%s] because it is a DML statement, reading file [%s] at line [%d]", gen_ctx->qry[k].name, file_name, lnum);
                    }

                    END_TEXT_LINE

                    if (asvar != NULL)
                    {
                        // we have variable in which to put column-names
                        oprintf("%s%s = __col_names_%s;\n", is_defined == 1 ? "char **" : "",  asvar, gen_ctx->qry[k].name);
                    }
                    else
                    {
                        _cld_report_error( "column-names in query [%s] cannot be used without 'as [define]' variable, i.e. the result must be assigned to a variable, reading file [%s] at line [%d]", gen_ctx->qry[k].name, file_name, lnum);
                    }
                    
                    BEGIN_TEXT_LINE
                    continue;
                }
                else if (((newI=recog_markup (line, i, "row-count#", &mtext, &msize, 0, file_name, lnum)) != 0))  // this is query count
                {
                    i = newI;
                    char *asvar;
                    int is_defined;
                    int k = get_query_id (gen_ctx, mtext, msize, file_name, lnum, &is_defined, &asvar);

                    // query must be either used or active, i.e. either inside the query or outside after
                    // this can be used outside the query loop, and it refers to the preceding query.
                    // if used, then this is not-active, so we're outside the query after it.
                    // Since we allow only one instance of a query, it is always correct
                    // (here the only other option is CLD_QRY_ACTIVE)
                    if (gen_ctx->qry_active[k] == CLD_QRY_UNUSED)
                    {
                        _cld_report_error( "Qry [%s] has never been used, reading file [%s] at line [%d]", gen_ctx->qry[k].name, file_name, lnum);
                    }
                    if (gen_ctx->qry[k].is_DML == 1)
                    {
                        _cld_report_error( "row-count cannot be used on query [%s] because it is a DML statement, use query-result#...,affected_rows, reading file [%s] at line [%d]", gen_ctx->qry[k].name, file_name, lnum);
                    }

                    END_TEXT_LINE

                    if (asvar != NULL)
                    {
                        // we have variable in which to put row-count
                        // this happens right in the run-time, as cld_printf_noenc is going on
                        oprintf("snprintf (__row_count_%s, sizeof (__row_count_%s)-1, \"%%d\" , __nrow_%s);\n", 
                        gen_ctx->qry[k].name, gen_ctx->qry[k].name, gen_ctx->qry[k].name);
                        oprintf("%s%s = __row_count_%s;\n", is_defined == 1 ? "char *" : "",  asvar, gen_ctx->qry[k].name);
                    }
                    else
                    {
                        oprintf("cld_printf (CLD_NOENC, \"%%d\", __nrow_%s);\n", gen_ctx->qry[k].name); 
                    }
                    
                    BEGIN_TEXT_LINE
                    continue;
                }
                // 
                // The following covers most of the query markups
                // and lots of them are connected and share some of the code
                //
                else if (((newI=recog_markup (line, i, "run-query#", &mtext, &msize, 0, file_name, lnum)) != 0)  
                    || ((newI1=recog_markup (line, i, "query-result#", &mtext, &msize, 0, file_name, lnum)) != 0) 
                    || ((newI2=recog_markup (line, i, "define-query#", &mtext, &msize, 0, file_name, lnum)) != 0)
                    || ((newI3=recog_markup (line, i, "loop-query#", &mtext, &msize, 0, file_name, lnum)) != 0)
                    || ((newI5=recog_markup (line, i, "define-dynamic-query#", &mtext, &msize, 0, file_name, lnum)) != 0)
                    || ((newI6=recog_markup (line, i, "start-query#", &mtext, &msize, 0, file_name, lnum)) != 0)
                    || ((newI7=recog_markup (line, i, "add-query-input#", &mtext, &msize, 0, file_name, lnum)) != 0)
                    || ((newI8=recog_markup (line, i, "define-shard#", &mtext, &msize, 0, file_name, lnum)) != 0)
                    || ((newI9=recog_markup (line, i, "define-soft-shard#", &mtext, &msize, 0, file_name, lnum)) != 0)
                    || ((newI10=recog_markup (line, i, "query-fragment#", &mtext, &msize, 0, file_name, lnum)) != 0))
                {
                    i = newI+newI1+newI2+newI3+newI5+newI6+newI7+newI8+newI9+newI10;

                    int run_query = (newI != 0 ? 1:0);
                    int query_result = (newI1 != 0 ? 1:0);
                    int define_query = (newI2 != 0 ? 1:0);
                    int loop_query = (newI3 != 0 ? 1:0);
                    int dynamic_query = (newI5 != 0 ? 1:0);
                    int start_query = (newI6 != 0 ? 1:0);
                    int add_input = (newI7 != 0 ? 1:0);
                    int shard = (newI8 != 0 ? 1:0);
                    int soft_shard = (newI9 != 0 ? 1:0);
                    int fragment = (newI10 != 0 ? 1:0);


                    if (dynamic_query == 1) define_query=1; //define query either way for our processing
                    if (start_query == 1) run_query=1; //starting query is the same as running, minus the loop
                    if (soft_shard==1) shard=1;

                    // end_of_query variable isn't actualy end-of-query, but the end of currently processed query text
                    // the only characters following query markup are =,: (or end of string)
                    char *end_of_query = mtext+ find_before_quote (mtext, msize, "=,:");
                    char *end_of_markup = mtext + msize;

                    //
                    // end_of_query is now pointing to either '=', ',' or ':' or the null character at the end of the string
                    // if neither of these characters was found
                    //

                    // get length of query ID
                    int qry_len = end_of_query - mtext;

                    // make sure we have space for it
                    if (qry_len > (int)sizeof (query_id_str) - 1)
                    {
                        _cld_report_error( "Qry ID too long, reading file [%s] at line [%d]", file_name, lnum);
                    }

                    // obtain query ID
                    memcpy (query_id_str, mtext, qry_len);
                    query_id_str[qry_len] = 0;
                    cld_trim (query_id_str, &qry_len);

                    if (cld_is_valid_param_name(query_id_str) != 1)
                    {
                        _cld_report_error(CLD_NAME_INVALID, query_id_str, file_name, lnum); 
                    }


                    if (shard==1)
                    {
                        if (find_query_shard(query_id_str, NULL, file_name, lnum)!=NULL)
                        {
                            _cld_report_error( "Shard [%s] already defined, reading file [%s] at line [%d]", query_id_str, file_name, lnum);
                        }
                        add_query_shard(query_id_str, "",soft_shard, file_name, lnum);
                        continue;
                    }

                    int k = -1;
                    int is_query_empty = 0; // this is 1 when define-query#xyz without anything else 
                    int is_redefine_query = 0; // this is if query is existing and being redefined - only valid for run-query or start-query
                                            // start-query can redefine the text many times, while run-query redefines empty query text (from define-query)
                                            // or it doesn't at all (i.e. run-query is used without define-query)

                    if (fragment==1)
                    {
                        if (*end_of_query!='=')
                        {
                            _cld_report_error( "query-fragment missing '=' sign, found [%s], reading file [%s] at line [%d]", end_of_query, file_name, lnum);
                        }
                        else
                        {
                            char *frag=strdup(end_of_query+1);
                            int frag_len = strlen(frag);
                            cld_trim(frag,&frag_len);
                            if (frag_len<2)
                            {
                                _cld_report_error( "Syntax error in query-fragment, found [%s], reading file [%s] at line [%d]", end_of_query, file_name, lnum);
                            }
                            if (frag[0]!='"' || frag[frag_len-1]!='"')
                            {
                                _cld_report_error( "query-fragment must be a quoted string, found [%s], reading file [%s] at line [%d]", end_of_query, file_name, lnum);
                            }
                            // remove start and end quote
                            frag[frag_len-1]=0;
                            frag++;

                            char *unq = find_unescaped_chars (frag, "\"");
                            if (*unq!=0)
                            {
                                _cld_report_error( "Unescaped quote found in query-fragment, found [%s], reading file [%s] at line [%d]", frag, file_name, lnum); 
                            }
                            add_query_fragment(query_id_str, frag, file_name,lnum);
                            //
                            // End of processing of query-fragment
                            //
                            continue;
                        }
                    }

                    // if this is define-query and not dynamic, there must be nothing afterwards
                    if (*end_of_query!=0 && define_query==1 && dynamic_query==0)
                    {
                        _cld_report_error( "Extra characters found after define-query, found [%s], reading file [%s] at line [%d]", end_of_query, file_name, lnum);
                    }

                    // dynamic query must have =variable_name with-output ...

                    // This is define-query or define-dynamic-query or start-query or run-query. Only define-query must be itself (not = or : or anything after it)
                    // the rest of run-query/start-query executes later when we check *end_of_query==0)
                    // define_query is 1 for both static AND define-dynamic-query, run_query is 1 for both run-query AND start-query
                    // What is under this if is 1) it has = and is dynamic, start or run-query OR 2) has nothing afterwards and is define-query (but not dynamic-query)
                    if ((*end_of_query == '=' && (dynamic_query==1 || run_query==1)) || (is_query_empty=(*end_of_query==0 && define_query==1 && dynamic_query==0)))
                    {
                        // THE PURPOSE of this IF branch is for DEFINITION or re-definition of a query. 
                        // here it must be either dynamic_query or define_query or run-query or start_query based on the above if

                        k = find_query (gen_ctx, query_id_str);
                        // if query is empty it is okay to redefine it, we will NOT add new one
                        if (k != -1)
                        {
                            // this is if query exists
                            dynamic_query = gen_ctx->qry[k].is_dynamic; // dynamic is defined in define-dynamic-query ONLY, so when run-query or start-query
                                        // comes along, we need to know if it is dynamic or not
                            // dynamic query cannot be redefined, all of its definition is in define-dynamic-query
                            // i.e. if query name found, it cannot be redefined
                            if (dynamic_query==1)
                            {
                                _cld_report_error( "Dynamic query cannot be re-defined, query [%s], reading file [%s] at line [%d]", query_id_str, file_name, lnum);
                            }
                            // when query is used again, it must be start-query or run-query.  We can redefine query in run-query
                            // only start-queries - if it is non-dynamic, which is true at this point
                            if (run_query!=1 && start_query !=1) // check if NOT start-query or run-query, if not, error out
                            {
                                _cld_report_error( "Query text re-definition for query [%s] can be used with start-query only, reading file [%s] at line [%d]", query_id_str, file_name, lnum);
                            }
                            is_redefine_query=1;
                        }
                        else
                        {
                            // if query not found, it cannot be start-query. Query can be defined in define-query, define-dynamic-query or run-query
                            if (start_query==1)
                            {
                                _cld_report_error( "Query  [%s] in start-query is not defined, reading file [%s] at line [%d]", query_id_str, file_name, lnum);

                            }
                        }
                        // definition of a new query in v file
                        // it cannot have double quotes inside of it, only single quotes
                        if (run_query==1 || start_query==1 || dynamic_query ==1)
                        {
                            // we go here if we did run-query/start-query redefinition of define-query, or if we do run-query/start-query for the first time, or if this is define-dynamic-query
                            // this is NOT define-query, because there is no query text there
                            // this is for run-query
                            char *q = strchr (end_of_query, '"');
                            if (q == NULL)
                            {
                                // this is potentially for define-dynamic-query#qry_name var [with-output col1,col2,col3...]
                                // i.e. a fully dynamic query
                                char *from_var = NULL;
                                char *next_kw = end_of_query + 1;
                                get_passed_whitespace (&next_kw);
                                if (dynamic_query == 1)
                                {
                                    from_var = next_kw;
                                    char *end_var = from_var;
                                    get_until_whitespace (&end_var);
                                    *end_var = 0;
                                    end_var++;

                                    // new_query either adds a query or uses existing one
                                    new_query (gen_ctx, from_var, query_id_str, lnum, file_name);
                                    k = find_query (gen_ctx, query_id_str);
                                    assert (k!=-1); // must be there
                                    gen_ctx->qry[k].is_dynamic = 1;

                                    get_passed_whitespace (&end_var);
                                    const char *without = "with-output";
                                    const char *withunout = "with-unknown-output";
                                    if (!strncmp (end_var, without, strlen(without)))
                                    {
                                        // this is dynamic query
                                        //
                                        get_until_whitespace(&end_var);
                                        get_passed_whitespace(&end_var);
                                        while (1)
                                        {
                                            int end_of_list = 0;
                                            char *collist = end_var;
                                            // now finding column list after with-output
                                            char *colend = collist;
                                            get_until_comma (&colend);
                                            if (*colend == 0) end_of_list = 1; // no more in comma-separated list, last item
                                            *colend = 0;
                                            int colLen = strlen (collist);
                                            cld_trim (collist, &colLen);
                                            gen_ctx->qry[k].qry_outputs[gen_ctx->qry[k].qry_total_outputs] = cld_strdup (collist);
                                            gen_ctx->qry[k].qry_total_outputs++;
                                            if (gen_ctx->qry[k].qry_total_outputs >= CLD_MAX_QUERY_OUTPUTS)
                                            {
                                                _cld_report_error( "Too many query outputs [%d], reading file [%s] at line [%d]", gen_ctx->qry[k].qry_total_outputs, file_name, lnum);
                                            }
                                            end_var = colend + 1;
                                            if (end_of_list == 1) break;
                                        }
                                    }
                                    else if (!strncmp (end_var, withunout, strlen(withunout)))
                                    {
                                        // 
                                        // This is SELECT with unknown outputs, such as 'SELECT * from '... - the output isn't really
                                        // unknown but we don't want to get data using column names, but rather column-data#... construct
                                        // So qry_total_outputs remains ZERO in this case.
                                        // This is ONLY for dynamic queries, which is this section of code (see is_dynamic above).
                                        // Check there is nothing beyond with-unknown-output:
                                        //
                                        char *beyond = end_var +  strlen(withunout);
                                        get_passed_whitespace(&beyond);
                                        if (*beyond != 0)
                                        {
                                            _cld_report_error( "Extra text after 'with-unknown-output', reading file [%s] at line [%d]", file_name, lnum);
                                        }
                                        //
                                        // We set qry_total_output to 1 even though we don't know the number of columns at run-time. If it were to remain 0,
                                        // then in get_num_of_cols, we would attempt to describe the table, and at that point the query text is just the name of the 
                                        // string variable that will hold the query at run-time, leading to crash. Any query would have at least one column so we can
                                        // safely call this one "first_column", which is rather arbitrary. The purpose of "with-unknown-output" is to NEVER get columns by
                                        // name but rather to use column-names#, column-data#.. and other markups.
                                        //
                                        gen_ctx->qry[k].qry_outputs[0] = "first_column";
                                        gen_ctx->qry[k].qry_total_outputs = 1;
                                    }
                                    else if (end_var[0] == 0)
                                    {
                                        // no with-output, this is not a SELECT, such a dynamic query is DML
                                        gen_ctx->qry[k].is_DML = 1;
                                    }
                                    else
                                    {
                                        _cld_report_error( "Unknown keyword after with-output, found [%s], reading file [%s] at line [%d]", end_of_query, file_name, lnum);
                                    }
                                }
                                else _cld_report_error( "Query must be double quoted, or the query must be defined as define-dynamic-query, found [%s], reading file [%s] at line [%d]", end_of_query, file_name, lnum);
                            }
                            else
                            {
                                // this is still either run-query or start-query
                                if (dynamic_query == 1)
                                {
                                    _cld_report_error( "A dynamic query cannot have double quotes, it must use a C char * variable, found [%s], reading file [%s] at line [%d]", end_of_query, file_name, lnum);
                                }

                                q++;
                                // find the end double quote but ignore the escaped double quotes
                                char *qe = find_unescaped_chars (q, "\"");
                                if (*qe == 0)
                                {
                                    _cld_report_error( "End quote of query missing, found [%s], reading file [%s] at line [%d]", q, file_name, lnum);
                                }
                                *qe = 0; 
                                // check if there is anything after final double quote, there should be NOTHING
                                char *after_qe=cld_strdup(qe+1);
                                int len_after_qe = strlen(after_qe);
                                cld_trim(after_qe,&len_after_qe);
                                if (after_qe[0]!=0)
                                {
                                    _cld_report_error( "Extra text after query-text, reading file [%s] at line [%d]",  file_name, lnum);
                                }
                                // new_query either adds a new one or uses existing one
                                new_query (gen_ctx, q, query_id_str, lnum, file_name);
                                k = find_query (gen_ctx, query_id_str);
                                assert (k!=-1); // must be there
                            }
                            // describe query, so we can get column # from name 
                            // there is no need to describe a query if it is a define-query as there is no query text yet
                            describe_query (gen_ctx, k, file_name, lnum);
                        }
                        else
                        {
                            // this is define-query ONLY, there is no text, query MUST be empty (no text)
                            if (is_query_empty == 0)
                            {
                               _cld_report_error( "Query definition has extra characters on the line, reading file [%s] at line [%d]", file_name, lnum); 
                            }
                            new_query (gen_ctx, "", query_id_str, lnum, file_name);
                            k = find_query (gen_ctx, query_id_str);
                            assert (k!=-1); // must be there
                        }
                        END_TEXT_LINE


                        // make variables ONLY for the original define-query or define-dynamic-query without query text
                        if (is_redefine_query==0)
                        {
                            oprintf("int __qry_massage_%s = CLD_QRY_NORMAL;\n", gen_ctx->qry[k].name);
                            oprintf("CLD_UNUSED (__qry_massage_%s);\n", gen_ctx->qry[k].name);
                            oprintf("int __qry_executed_%s = 0;\n", gen_ctx->qry[k].name);
                            oprintf("CLD_UNUSED (__qry_executed_%s);\n", gen_ctx->qry[k].name);
                            oprintf("char *%s = NULL;\n", query_id_str); // define query string
                            cld_allocate_query (gen_ctx, k);
                        }
                        if (gen_ctx->qry[k].is_qry_compact==1)
                        {
                            // For compact query, add each parameter directly to the query
                            int i;
                            for (i=0;i<gen_ctx->qry[k].qry_total_inputs;i++)
                            {
                                int is_inp_str;
                                handle_quotes_in_input_param (&(gen_ctx->qry[k].compact_params[i]), &is_inp_str);
                                add_input_param (gen_ctx, k, is_inp_str,gen_ctx->qry[k].compact_params[i], file_name, lnum);
                            }
                        }


                        // we start a new line for run-query, this is why below get_next_input_param can work because
                        // this function EXPECTS open new line!
                        BEGIN_TEXT_LINE

                        // if this is run-query="..." then continue to run query after being defined
                        // otherwise nothing else to do here, and so continue
                        if (run_query==0)
                        {
                            continue;
                        }
                        else
                        {
                            // For run-query, we may want to still have define-query prior to run-query as a separate statement in order to be
                            // able to change query properties at run time, such as create-empty-row, or any future ones
                            end_of_query= end_of_markup; // we have processed all of define-query and now run-query
                                        // below needs to see *end_of_query as being empty string and just run the query
                        }
                    }


                    // Now check for run-query or query-result
                    k = find_query (gen_ctx, query_id_str);
                    if (k == -1)
                    {
                        _cld_report_error( "Query [%s] is not found, reading file [%s] at line [%d]", query_id_str, file_name, lnum);
                    }

                    query_id = k;

                    // we only allow query-result to be DIRECTLY under run-query/loop-query, and NOT under another query. For example we do not allow run-query#X... run-query#Y... query-result#X... - it has to be query-result#Y. The reason is that this causes hard-to-find errors when looking for error, insert_id and columns with the same name. ALWAYS get the results of the query PRIOR to issuing another query. Getting query result is CHEAP, it only assigns a char *.
                    if (query_result==1)
                    {
                        if (gen_ctx->global_qry_stack[gen_ctx->curr_qry_ptr - 1] != query_id)
                        {
                            _cld_report_error( "query-result can be only directly under the run-query or loop-query, check the name of query used, reading file [%s] at line [%d]", file_name, lnum);
                        }
                    }


                    //
                    // for run-query#..="..." , *end_of_query is 0 here, so all these branches are skipped.
                    //
                    if (*end_of_query == ':')
                    {
                        // this is for run-query, input params are after :
                        // it could be also add-input which is just for adding input params
                        // see comments on cld_make_SQL() function for more on this.
                        // We have open new line (see above cld_printf(... just opening of new string) - because
                        // get_next_input_param expects this, and it will leave it open after it's done and also if this 
                        // function is not executed below, then it's open, as it should be (and we have END_TEXT_LINE where
                        // necessary to close it).

                        if (add_input == 1)
                        {
                            end_of_query++; // now points to first after ':'
                            get_next_input_param (gen_ctx, query_id, &end_of_query, file_name, lnum);
                            continue;
                            // since this is add-input, we have added parameters, and done
                        }


                        if (run_query != 1)
                        {
                            _cld_report_error( "Invalid syntax, it appears this should be a run-query markup, found [%s], reading file [%s] at line [%d]", end_of_query, file_name, lnum);
                        }
                        if (gen_ctx->qry_active[query_id] == CLD_QRY_ACTIVE)
                        {
                            _cld_report_error( "Qry [%s] is already active, cannot use run-query here, reading file [%s] at line [%d]", gen_ctx->qry[query_id].name, file_name, lnum);
                        }

                        end_of_query++; // now points to first after ':'
                        get_next_input_param (gen_ctx, query_id, &end_of_query, file_name, lnum);
                    }


                    // after going through :x,y,z for run-query, we must encounter ? (end of markup)
                    // if it wasn't run-query (i.e. no : to begin with), then we must find , (a comma) which is used in query-result
                    if (*end_of_query == 0)
                    {
                        // this is now query execution, i.e. a query loop because we reached the end of query markup
                        if (loop_query == 1)
                        {
                            if (gen_ctx->qry_active[query_id] != CLD_QRY_USED)
                            {
                                _cld_report_error( "In order to use loop-query, it must have been used with run-query or start-query first, found [%s], reading file [%s] at line [%d]", end_of_query, file_name, lnum);
                            }

                            END_TEXT_LINE

                            oprintf("for (__iter_%s = 0; __iter_%s < __nrow_%s; __iter_%s++)\n",gen_ctx->qry[query_id].name, 
                                gen_ctx->qry[query_id].name,gen_ctx->qry[query_id].name,gen_ctx->qry[query_id].name);
                            oprintf("{\n");

                            BEGIN_TEXT_LINE
                            check_next_query
                            gen_ctx->curr_qry_ptr ++;
                            // check if we're nesting too much
                            if (gen_ctx->curr_qry_ptr >= CLD_MAX_QUERY_NESTED)
                            {
                                _cld_report_error( CLD_MSG_NESTED_QRY, query_id, CLD_MAX_QUERY_NESTED, file_name, lnum);
                            }
                            gen_ctx->global_qry_stack[gen_ctx->curr_qry_ptr - 1] = query_id;

                            // now this query ID is active. We use it to prohibit nesting queries with the same ID
                            gen_ctx->qry_active[query_id] = CLD_QRY_ACTIVE;

                            // done with loop query
                            continue;
                        }
            			else if (start_query ==1)
                        {
                            // for start-query, nothing to do, because start-query doesn't execute a query loop
                            // loop-query will be used later to do that
                            ;
                        }
                        else
                        {
                            // run-query can be called only once (this must be run-query here) and we check if it was called already
                            if (gen_ctx->qry_active[query_id] == CLD_QRY_USED)
                            {
                                _cld_report_error( "run-query markup can be called only once for a query, use multiple queries or loop-query, found [%s], reading file [%s] at line [%d]", end_of_query, file_name, lnum);
                            }
                        }


                        // end of run-query markup
                        // start-query, run-query all set variable run_query to 1
                        if (run_query != 1)
                        {
                            _cld_report_error( "Invalid syntax, it appears this should be a run-query or start-query markup, found [%s], reading file [%s] at line [%d]", end_of_query, file_name, lnum);
                        }
                        check_next_query

                        // check if number of input params matches the number of %s in a query
                        // but only if query is not dynamic (see previous comment for dynamic)
                        if (gen_ctx->qry[query_id].is_dynamic == 0)
                        {
                            if (gen_ctx->qry[query_id].qry_found_total_inputs != gen_ctx->qry[query_id].qry_total_inputs)
                            {
                                _cld_report_error( "Expecting [%d] input parameters for query [%s], found only [%d], reading file [%s] at line [%d]. %s", gen_ctx->qry[query_id].qry_total_inputs, gen_ctx->qry[query_id].name, gen_ctx->qry[query_id].qry_found_total_inputs, file_name, lnum, CLD_PARAM_USAGE);
                            }
                        }

                        // Cannot use query ID within itself
                        if (gen_ctx->qry_active[query_id] == CLD_QRY_ACTIVE)
                        {
                            _cld_report_error( "Qry ID [%d] is used within itself, use the same query with different ID if needed, reading file [%s] at line [%d]", query_id, file_name, lnum);
                        }

                        // move the query stack pointer one up. At this location in the stack, there is
                        // nothing, i.e. this pointer always points to the next (as of yet non-existent)
                        // query ID
                        gen_ctx->curr_qry_ptr ++;

                        // check if we're nesting too much
                        if (gen_ctx->curr_qry_ptr >= CLD_MAX_QUERY_NESTED)
                        {
                            _cld_report_error( CLD_MSG_NESTED_QRY, query_id, CLD_MAX_QUERY_NESTED, file_name, lnum);
                        }

                        // since current valid stack is one below stack pointer, put our query ID there
                        gen_ctx->global_qry_stack[gen_ctx->curr_qry_ptr - 1] = query_id;

                        // now this query ID is active. We use it to prohibit nesting queries with the same ID
                        gen_ctx->qry_active[query_id] = CLD_QRY_ACTIVE;

                        END_TEXT_LINE

                        // ready to generate query execution code in C


                        int z;

                        // DML query cannot be made empty or no-use
                        if (gen_ctx->qry[query_id].is_DML == 0)
                        {
                            // check if we are using empty result set
                            // this 'if' is if it is not CLD_QRY_CREATE_EMPTY, in this 
                            // case execute query
                            oprintf("if (__qry_massage_%s != CLD_QRY_CREATE_EMPTY)\n", gen_ctx->qry[query_id].name);
                            oprintf("{\n");
                        }

                        // we prohibit selects from having double quotes so we place the query text within double quotes
                        if (gen_ctx->qry[query_id].is_dynamic == 0)
                        {
                            oprintf("%s = \"%s\";\n", gen_ctx->qry[query_id].name, gen_ctx->qry[query_id].text);
                        }
                        else
                        {
                            oprintf("%s = %s;\n", gen_ctx->qry[query_id].name, gen_ctx->qry[query_id].text);
                        }
                        oprintf("char *fname_loc_%s = \"%s\";\n",gen_ctx->qry[query_id].name, file_name); // to overcome constness
                        oprintf("int lnum_%s = %d;\n",gen_ctx->qry[query_id].name,lnum); 
                        oprintf("cld_location (&fname_loc_%s, &lnum_%s, 1);\n",gen_ctx->qry[query_id].name,gen_ctx->qry[query_id].name);
                        // with dynamic queries, we cannot count how many '%s' in SQL text (i.e. inputs) there are. Only with static queries
                        // can we do that (this is qry_total_inputs). For dynamic, the number of inputs is known  only by
                        // the number of actual input parameters in run-query or start-query (this is qry_found_total_inputs). Because
                        // SQL statement (__sql_buf...) is only known at run-time, we need to pass this number of input params to it
                        // and verify we are not using bad memory or missing arguments.
                        int num_run_time_params = (gen_ctx->qry[query_id].is_dynamic == 1 ?
                                                    gen_ctx->qry[query_id].qry_found_total_inputs : gen_ctx->qry[query_id].qry_total_inputs);
                        oprintf("cld_make_SQL (__sql_buf_%s, %d, %d, %s ",
                                gen_ctx->qry[query_id].name,CLD_MAX_SQL_SIZE, num_run_time_params,  gen_ctx->qry[query_id].name); 

                        for (z = 0; z < num_run_time_params; z++)
                        {
                            if (gen_ctx->qry[query_id].qry_is_input_str[z] == 0)
                            {
                                oprintf(", __is_input_used_%s[%d]==1 ?  (%s) : NULL ", gen_ctx->qry[query_id].name, z, gen_ctx->qry[query_id].qry_inputs[z]);
                            }
                            else
                            {
                                oprintf(", __is_input_used_%s[%d]==1 ?  \"%s\" : NULL ", gen_ctx->qry[query_id].name, z, gen_ctx->qry[query_id].qry_inputs[z]);
                            }
                        }
                        oprintf(");\n");

                        // We execute the actual db query right at the beginning of the action block
                        // We use 'query ID' decorated variables, so all our results are separate

                        oprintf("if (__qry_executed_%s == 1) {cld_report_error(\"Query [%s] has executed the second time without calling define-query before it; if your query executes in a loop, make sure the define-query executes in that loop too prior to the query; if you want to execute the same query twice in a row without a loop, use different queries with the same query text if that is your intention. \");}\n", gen_ctx->qry[query_id].name, gen_ctx->qry[query_id].name);
                        oprintf("__qry_executed_%s = 1;\n", gen_ctx->qry[query_id].name);
                        if (gen_ctx->qry[query_id].is_DML == 0)
                        {
                            // generate select call for SELECTs
                            oprintf("cld_select_table (__sql_buf_%s, &__nrow_%s, &__ncol_%s, &__col_names_%s, &__data_%s);\n",
                            gen_ctx->qry[query_id].name, gen_ctx->qry[query_id].name,gen_ctx->qry[query_id].name, gen_ctx->qry[query_id].name, gen_ctx->qry[query_id].name);

                            oprintf("if (__nrow_%s > 0) cld_data_iterator_fill_array (__data_%s, __nrow_%s, __ncol_%s, &__arr_%s);\n",
                            gen_ctx->qry[query_id].name, gen_ctx->qry[query_id].name,gen_ctx->qry[query_id].name, gen_ctx->qry[query_id].name, 
                                gen_ctx->qry[query_id].name);

                            oprintf("else if (__qry_massage_%s == CLD_QRY_USE_EMPTY)\n", gen_ctx->qry[query_id].name);
                            oprintf("{\n");
                            oprintf("__nrow_%s=1;\n", gen_ctx->qry[query_id].name);
                            oprintf("__ncol_%s=%d;\n", gen_ctx->qry[query_id].name, 
                                get_num_of_cols (gen_ctx, query_id, file_name, lnum));
                            oprintf("cld_get_empty_row (&__arr_%s, __ncol_%s);\n",
                                gen_ctx->qry[query_id].name, gen_ctx->qry[query_id].name);
                            oprintf("}\n");
                        }
                        else
                        {
                            // this is DML call
                            char *dml_err=NULL;
                            if (try_DML (gen_ctx, query_id, file_name, lnum,&dml_err)!=1)
                            {
                                _cld_report_error( "DML statement could not be parsed, error [%s], reading file [%s] at line [%d]", dml_err==NULL?"":dml_err, file_name, lnum);
                            }
                            oprintf("cld_execute_SQL (__sql_buf_%s, &__nrow_%s, &__err_%s, NULL);\n",
                                gen_ctx->qry[query_id].name, gen_ctx->qry[query_id].name, gen_ctx->qry[query_id].name);


                            if (gen_ctx->qry[query_id].is_insert == 1)
                            {
                                oprintf("cld_get_insert_id (__insert_id_%s, sizeof (__insert_id_%s)) ;\n", gen_ctx->qry[query_id].name, 
                                    gen_ctx->qry[query_id].name);
                            }
                            else
                            {
                                oprintf("__insert_id_%s[0] = 0;\n", gen_ctx->qry[query_id].name);
                            }
                            
                            // __nrow_... is actual number of affected rows here
                            oprintf("cld_get_dml_row (&__arr_%s, __nrow_%s, __err_%s, __insert_id_%s);\n",  gen_ctx->qry[query_id].name,
                                gen_ctx->qry[query_id].name,  gen_ctx->qry[query_id].name, gen_ctx->qry[query_id].name);
                            // __nrow_... is now always one, since we return one row always, consisting of affected rows, error and insert id.
                            oprintf("__nrow_%s=1;\n", gen_ctx->qry[query_id].name);
                            oprintf("__ncol_%s=3;\n", gen_ctx->qry[query_id].name);
                            
                        }
                        if (gen_ctx->qry[query_id].is_DML == 0)
                        {
                            oprintf("}\n");
                            oprintf("else\n");
                            oprintf("{\n");
                            oprintf("__nrow_%s=1;\n", gen_ctx->qry[query_id].name);
                            oprintf("__ncol_%s=%d;\n", gen_ctx->qry[query_id].name, 
                                get_num_of_cols (gen_ctx, query_id, file_name, lnum));
                            oprintf("cld_get_empty_row (&__arr_%s, __ncol_%s);\n",
                                gen_ctx->qry[query_id].name, gen_ctx->qry[query_id].name);
                            oprintf("}\n");
                        }
                        oprintf("cld_free (__sql_buf_%s);\n", gen_ctx->qry[query_id].name);

                        if (start_query == 0)
                        {
                            // at this point, we opened a FOR loop to get all the rows. As we go along, we will
                            // print both HTML code and the resulting columns 
                            oprintf("for (__iter_%s = 0; __iter_%s < __nrow_%s; __iter_%s++)\n",gen_ctx->qry[query_id].name, gen_ctx->qry[query_id].name, 
                                gen_ctx->qry[query_id].name, gen_ctx->qry[query_id].name);
                            oprintf("{\n");
                            BEGIN_TEXT_LINE
                        }
                        else
                        {
                            // end_query will close current line, open new one - this is why we start opening
                            // line before end_query - note that such empty line will be removed from executable code.
                            // this one (0 for close_block) will not close for() block because for start-query
                            // we never opened it. See above - we only open for() block for run-query
                            // But we must close all other stuff (query stack) - this is why we call end_query here.
                            BEGIN_TEXT_LINE
                            end_query (gen_ctx,  &query_id, &open_queries, 0, file_name, lnum); 
                        }


                        continue;
                    }



                    else if (*end_of_query == ',')
                    {
                        // 
                        // this is query-result markup
                        //
                        if (query_result != 1)
                        {
                            _cld_report_error( "Invalid syntax, it appears this should be a query-result markup, found [%s], reading file [%s] at line [%d]", end_of_query, file_name, lnum);
                        }
                        // reserve variable space for db column name, C name, maybe define and encode as in
                        // query-result#..., (from here on) columnName as define col_name urlencode
                        char col_out[3*CLD_MAX_COLNAME_LEN + 1];
                        int col_len = end_of_markup - (end_of_query +1);

                        if (col_len > (int)sizeof (col_out) - 1)
                        {
                            _cld_report_error( "Column name too long, reading file [%s] at line [%d]", file_name, lnum);
                        }

                        // get col_out, which is col name in SELECT output of a query
                        memcpy (col_out, end_of_query + 1, col_len);
                        col_out[col_len] = 0;

                        cld_trim (col_out, &col_len);

                        // get column encoding when retrieving query column
                        int no_encode = 0;
                        int url_encode = 0;
                        int web_encode = 0;
                        char *noenc = strstr (col_out, " noencode");
                        if (noenc != NULL) no_encode = 1;
                        char *webenc = strstr (col_out, " webencode");
                        if (webenc != NULL) web_encode = 1;
                        char *urlenc = strstr (col_out, " urlencode");
                        if (urlenc != NULL) url_encode = 1;

                        // we can change the mtext all we want, but we can't memmove
                        if (urlenc != NULL) *urlenc = 0;
                        if (webenc != NULL) *webenc = 0;
                        if (noenc != NULL) *noenc = 0;

                        if (no_encode+web_encode+url_encode > 1)
                        {
                            _cld_report_error ("Query output can be either noencode, webencode (default) or urlencode, but not any combination of these, reading file [%s] at line [%d]", file_name, lnum);
                        }

                        int is_defined = 0;
                        char *asv = strstr (col_out, CLD_KEYAS);
                        char *newV = NULL;
                        if (asv != NULL)
                        {
                            if (no_encode+web_encode+url_encode == 1)
                            {
                                _cld_report_error ("Encoding cannot be used with AS keyword, the data is not encoded, but not any combination of these, reading file [%s] at line [%d]", file_name, lnum);
                            }
                            *asv = 0;
                            col_len = strlen (col_out);
                            cld_trim (col_out, &col_len);
                            if (col_len == 0)
                            {
                                _cld_report_error ("Column name cannot be empty, reading file [%s] at line [%d]", file_name, lnum);
                            }
                            newV = asv + strlen (CLD_KEYAS);
                            // newV cannot be empty because we cld_trimmed col_out and 
                            // searched for a string that ends in space!
                            
                            is_opt_defined (&newV, &is_defined, file_name, lnum);
                        }
                        if (no_encode+web_encode+url_encode == 0) web_encode = 1; // default webencode

                        // finish printing whatever was done so far
                        END_TEXT_LINE

                        // Check if query ID is active. If it is used, but not active, it means
                        // we're using a query column outside the action block for that query
                        if (gen_ctx->qry_active[query_id] != CLD_QRY_ACTIVE)
                        {
                            _cld_report_error( "Qry [%s] is used, but not active, reading file [%s] at line [%d]", gen_ctx->qry[query_id].name, file_name, lnum);
                        }

                        int column_id =  get_col_ID (gen_ctx, query_id, col_out, file_name, lnum);

                        // we now have numerical positional ID for a column in the result set
                        // which is column_id
                        // for start-query we have made sure all start-query# queries have exact same columns
                        // because the get_col_ID() call above will get the very last column definition in any case

                        // Print out actual column from db query, at runtime
                        if (newV == NULL)
                        {
                            oprintf("cld_printf (%s, \"%%s\", __arr_%s[__iter_%s][%d]);\n", web_encode == 1 ? "CLD_WEB" : (url_encode == 1 ? "CLD_URL":"CLD_NOENC"), gen_ctx->qry[query_id].name,gen_ctx->qry[query_id].name,column_id);
                        }

                        if (newV != NULL)
                        {
                            // this happens right in the run-time, as cld_printf is going on
                            oprintf("%s%s = (__arr_%s[__iter_%s][%d]);\n", is_defined == 1 ? "char *" : "" , newV, gen_ctx->qry[query_id].name, 
                                gen_ctx->qry[query_id].name, column_id);
                        }
                        

                        BEGIN_TEXT_LINE
                        continue;
                    }
                    else
                    {
                       _cld_report_error ("Syntax error in query, reading file [%s] at line [%d]", file_name, lnum);
                    }
                }
                else if (!memcmp(line+i, "w ",2) || !memcmp(line+i,"w?>",3))
                {
                    // W (or <?W?> is a synonym for a web-output line. Sometimes a web output would
                    // start with a keyword (such as 'if'), and we didn't mean to do that, we just want
                    // to ouput 'if ..' to the web. So 'w' really doesn't do anything else, but prevent
                    // other commands from kicking it and interpreting the code
                    if (line[i]=='w' && line[i+1]==' ')
                    {
                        i+=1; // position on last space
                    }
                    else
                    {
                        i+=2; // get passed <?w?> and position on last '>'
                    }
                    usedCLD = 0; // reset usedCLD because "w" isn't actual directive, it is only
                                // a dead disambigation directive
                    // 'w' is unique in that it always has to have something after it (i.e. isLast is 0),
                    // but the data after 'w' should not be processed here, in 'w' handler. Above 'i'
                    // setting makes sure processing continues normally after 'w'
                    END_TEXT_LINE
                    BEGIN_TEXT_LINE

                    continue;
                }
                else if ((newI=recog_markup (line, i, "c", &mtext, &msize, 0, file_name, lnum)) != 0)  
                {
                    i = newI;
                    END_TEXT_LINE
                    // we do not enforce that C code must end with semicolon (because it doesn't, for example a line like:
                    // while (1)
                    // if you added ; at the end, the following
                    // {
                    // ...
                    // }
                    // would NEVER execute.
                    // However, if a line ends with ; it MUST be C code (or use <?;?> to not do that!)
                    // For code like above (while loop for instance), use start-c markup
                    oprintf("%.*s\n", msize, mtext); 
                    BEGIN_TEXT_LINE

                    continue;
                }
                else if ((newI=recog_markup (line, i, "print-noenc", &mtext, &msize, 0, file_name, lnum)) != 0)  
                {
                    i = newI;
                    END_TEXT_LINE
                    oprintf("cld_puts (CLD_NOENC, %.*s);\n", msize, mtext); 
                    BEGIN_TEXT_LINE

                    continue;
                }
                else if ((newI=recog_markup (line, i, "print-url", &mtext, &msize, 0, file_name, lnum)) != 0)  
                {
                    i = newI;
                    END_TEXT_LINE
                    oprintf("cld_puts (CLD_URL, %.*s);\n", msize, mtext); 
                    BEGIN_TEXT_LINE

                    continue;
                }
                else if ((newI=recog_markup (line, i, "preprocessor-output#", &mtext, &msize, 0, file_name, lnum)) != 0)  
                {
                    // write something to a file - this is preprocessor, not run-time output!!!
                    i = newI;
                    // get file name first
                    char *txt = cld_strdup (mtext);
                    int l = strlen (txt);
                    cld_trim (txt, &l);
                    char *end_var = txt;
                    get_until_whitespace (&end_var);
                    *end_var = 0;
                    l = strlen (end_var+1);
                    cld_trim (end_var+1, &l);
                    // get the text to write to it
                    char *entry = end_var + 1;
                    char full_name[1024];
                    snprintf (full_name, sizeof(full_name)-1, "%s.clo", txt);
                    FILE *f = fopen (full_name, "a+");
                    if (f==NULL)
                    {
                        _cld_report_error( "Cannot open file [%s] from preprocessor-output, reading file [%s] at line [%d]", full_name, file_name, lnum);
                    }
                    fprintf (f, "%s | %s:%d\n", entry, file_name,lnum);
                    fclose(f);
                    continue;
                }
                else if ((newI=recog_markup (line, i, "print-web", &mtext, &msize, 0, file_name, lnum)) != 0)  
                {
                    i = newI;
                    END_TEXT_LINE
                    char *show_newline = strstr (mtext, CLD_KEYSHOWNEWLINE);
                    if (show_newline!=NULL)
                    {
                        *show_newline=0; // can do this since we don't change length of mtext, if we did replace, then we'd have to work on a copy!
                    }
                    if (show_newline!=NULL)
                    {
                        oprintf("cld_print_web_show_newline(%.*s);\n", msize, mtext); 
                    }
                    else
                    {
                        oprintf("cld_puts (CLD_WEB, %.*s);\n", msize, mtext); 
                    }
                    BEGIN_TEXT_LINE

                    continue;
                }
                else if ((newI=recog_markup (line, i, "output-http-header", &mtext, &msize, 1, file_name, lnum)) != 0)  
                {
                    i = newI;
                    END_TEXT_LINE
                    oprintf("cld_output_http_header(cld_get_config ()->ctx.req);\n");
                    BEGIN_TEXT_LINE

                    continue;
                }
                else if ((newI=recog_markup (line, i, "continue-query", &mtext, &msize, 1, file_name, lnum)) != 0)  
                {
                    i = newI;
                    END_TEXT_LINE
                    oprintf("continue;\n");
                    BEGIN_TEXT_LINE
                    if (query_id == -1)
                    {
                        _cld_report_error( "continue-query used outside of active query, reading file [%s] at line [%d]", file_name, lnum);
                    }

                    continue;
                }
                else if ((newI=recog_markup (line, i, "exit-query", &mtext, &msize, 1, file_name, lnum)) != 0)  
                {
                    i = newI;
                    END_TEXT_LINE
                    oprintf("break;\n");
                    BEGIN_TEXT_LINE
                    if (query_id == -1)
                    {
                        _cld_report_error( "exit-query used outside of active query, reading file [%s] at line [%d]", file_name, lnum);
                    }

                    continue;
                }
                else if ((newI=recog_markup (line, i, "exec-program", &mtext, &msize, 0, file_name, lnum)) != 0)  
                {
                    i = newI;
                    END_TEXT_LINE
                    
                    // exec-program <program> program-args <list separated by comma> program-status <int status> program-output <out-variable> program-output-length <length of out-variable>

                    // Get keywords (if used)
                    char *program_args = strstr (mtext, CLD_KEYPROGRAMARGS);
                    char *program_status = strstr (mtext, CLD_KEYPROGRAMSTATUS);
                    char *program_output = strstr (mtext, CLD_KEYPROGRAMOUTPUT);
                    char *program_output_length = strstr (mtext, CLD_KEYPROGRAMOUTPUTLEN);

                    // get string where program output goes
                    if (program_output!= NULL)
                    {
                        char *end_of_url = program_output;
                        *(program_output = (program_output + strlen (CLD_KEYPROGRAMOUTPUT)-1)) = 0;
                        program_output++;
                        *end_of_url = 0;
                    }
                    else
                    {
                        _cld_report_error( "program-status not found in exec-program, reading file [%s] at line [%d]", file_name, lnum);
                    }
                    int is_def_program_output;
                    is_opt_defined (&program_output, &is_def_program_output, file_name, lnum);



                    // get length of (to-be) string where program output goes
                    if (program_output_length!= NULL)
                    {
                        char *end_of_url = program_output_length;
                        *(program_output_length = (program_output_length + strlen (CLD_KEYPROGRAMOUTPUTLEN)-1)) = 0;
                        program_output_length++;
                        *end_of_url = 0;
                    }
                    else
                    {
                        program_output_length="256"; // default length for program output
                    }

                    // get int variable for program status
                    if (program_status!= NULL)
                    {
                        char *end_of_url = program_status;
                        *(program_status = (program_status + strlen (CLD_KEYPROGRAMSTATUS)-1)) = 0;
                        program_status++;
                        *end_of_url = 0;
                    }
                    else
                    {
                        _cld_report_error( "program-status not found in exec-program, reading file [%s] at line [%d]", file_name, lnum);
                    }
                    int is_def_program_status;
                    is_opt_defined (&program_status, &is_def_program_status, file_name, lnum);

                    // get program argument list, to be parsed below
                    if (program_args != NULL)
                    {
                        char *end_of_url = program_args;
                        *(program_args = (program_args + strlen (CLD_KEYPROGRAMARGS)-1)) = 0;
                        program_args++;
                        *end_of_url = 0;
                    }
                    else
                    {
                        _cld_report_error( "program-args not found in exec-program, reading file [%s] at line [%d]", file_name, lnum);
                    }

                    // define run-time list of program arguments
                    oprintf("const char *__prg_arr%d[%d];\n", total_exec_programs, CLD_MAX_EXEC_PARAMS);

                    // must duplicate string because we CHANGE curr_start in parse_param_list (because it removes
                    // escaped quotes) and thus we change the 'line' (the current parsing line). As a result, we will
                    // probably error out at the end of line because we will see null characters since the line has 
                    // shifted to the left during removal of escaped quotes).
                    char *curr_start = cld_strdup(program_args);

                    // parse program arguments
                    cld_store_data params;
                    parse_param_list (&curr_start, &params, file_name, lnum);
                    char *is_inp_str;
                    char *value;
                    int exec_inputs=1; // we start with index 1 in order to fill in program name as arg[0] at run-time
                    while (1)
                    {
                        cld_retrieve (&params, &is_inp_str, &value);
                        if (is_inp_str==NULL) break;

                        if (atol(is_inp_str)==1)
                        {
                            // generate code for program arguments array (string argument)
                            oprintf("__prg_arr%d[%d] = \"%s\";\n", total_exec_programs, exec_inputs, value);
                        }
                        else
                        {
                            // generate code for program arguments array (run time variable argument)
                            oprintf("__prg_arr%d[%d] = %s;\n", total_exec_programs, exec_inputs, value);
                        }
                        exec_inputs++;
                        if (exec_inputs>=CLD_MAX_EXEC_PARAMS - 1)
                        {
                            _cld_report_error( "Too many program arguments [%d], reading file [%s] at line [%d].", exec_inputs, file_name, lnum);

                        }
                    } 

                    // final arg MUST be NULL
                    oprintf("__prg_arr%d[%d] = NULL;\n", total_exec_programs, exec_inputs);

                    if (is_def_program_output ==1)
                    {
                        oprintf ("char *%s=NULL;\n", program_output);
                    }
                    if (is_def_program_status ==1)
                    {
                        oprintf ("int %s=0;\n", program_status);
                    }

                    // generate run-time call to execute program
                    oprintf ("cld_exec_program(%s, %d, __prg_arr%d, &(%s), &(%s), %s);\n", mtext, exec_inputs, total_exec_programs, program_status, program_output,
                        program_output_length);
                    total_exec_programs++; // advance exec-program counter so generating specific variables is unique
                    BEGIN_TEXT_LINE
                    continue;
                }
                else if ((newI=recog_markup (line, i, "web-call", &mtext, &msize, 0, file_name, lnum)) != 0)  
                {
                    i = newI;
                    END_TEXT_LINE
                    // Example:
                    // web-call "https://zigguro.com/zigguro.f?program=&session=cc0d6f00c7b72a0dc482fb1106aea7b02c20bvmd76cf2545b0a600540ae35367&user_id=1&back_page_id=140&page=home&action=default" with-response define webres with-error define weberr  with-cert "/home/zigguro/ca.crt" cookie-jar "/home/zigguro/cookies"
                    // or 'with-no-cert' instead of with-cert ....
                    // with-no-cert doesn't check anything
                    // with-cert checks file
                    // without either option, or with-cert "", is regular authority (preinstalled CAs)/host name checks


                    //
                    // Look for each option and collect relevant info
                    //
                    char *resp = strstr (mtext, CLD_KEYWITHRESPONSE);
                    char *err = strstr (mtext, CLD_KEYWITHERROR);
                    char *cert = strstr (mtext, CLD_KEYWITHCERT);
                    char *nocert = strstr (mtext, CLD_KEYWITHNOCERT);
                    char *cookiejar = strstr (mtext, CLD_KEYCOOKIEJAR);


                    if (cert != NULL && nocert != NULL)
                    {
                        _cld_report_error( "with-cert and with-no-cert cannot coexist in the same web-call markup, reading file [%s] at line [%d]", file_name, lnum);
                    }

                    if (cookiejar != NULL)
                    {
                        char *end_of_url = cookiejar;
                        *(cookiejar = (cookiejar + strlen (CLD_KEYCOOKIEJAR)-1)) = 0;
                        cookiejar++;
                        *end_of_url = 0;
                    }

                    if (resp != NULL) 
                    {
                        char *end_of_url = resp;
                        *(resp = (resp + strlen (CLD_KEYWITHRESPONSE)-1)) = 0;
                        resp++;
                        *end_of_url = 0;
                    }
                    else
                    {
                        _cld_report_error( "with-response markup is missing in web-call, reading file [%s] at line [%d]", file_name, lnum);
                    }
                    if (cert != NULL) 
                    {
                        char *end_of_url = cert;
                        *(cert = (cert + strlen (CLD_KEYWITHCERT)-1)) = 0;
                        cert++;
                        *end_of_url = 0;
                    } // cert can be unspecified in which case no checks are done on SSL connection
                    if (nocert != NULL) 
                    {
                        *nocert = 0;
                    } // cert has no options

                    if (err != NULL) 
                    {
                        char *end_of_url = err;
                        *(err = (err + strlen (CLD_KEYWITHERROR)-1)) = 0;
                        err++;
                        *end_of_url = 0;
                    }
                    else
                    {
                        _cld_report_error( "with-error markup is missing in web-call, reading file [%s] at line [%d]", file_name, lnum);
                    }

                    int is_def_result = 0;
                    int is_def_err = 0;

                    is_opt_defined (&resp, &is_def_result, file_name, lnum);

                    is_opt_defined (&err, &is_def_err, file_name, lnum);
                    
                    if (is_def_result == 1)
                    {
                        oprintf ("char *%s = cld_init_string (\"\");\n", resp);
                    }
                    if (is_def_err == 1)
                    {
                        oprintf ("char *%s = cld_init_string (\"\");\n", err);
                    }
                    // cert cannot be defined, must exist and be filled with the location of .ca file!

                    oprintf ("cld_post_url_with_response(%s, &(%s), &(%s), %s, %s);\n", mtext, resp, err, nocert != NULL ? "NULL" : (cert != NULL ? cert : "\"\""), 
                        cookiejar == NULL ? "NULL":cookiejar);

                    BEGIN_TEXT_LINE

                    continue;
                }
                else if ((newI=recog_markup (line, i, "send-mail", &mtext, &msize, 0, file_name, lnum)) != 0)  
                {
                    i = newI;
                    END_TEXT_LINE
                    // Example:
                    // send-mail from "x@y.com" to "z@x.com" subject "subject of message" headers "x:z\r\nw:y" body "This is the body of email message" status int_var
                    // headers is optional, others are mandatory

                    //
                    // Look for each option and collect relevant info
                    // First we MUST get each options position
                    //
                    char *from = strstr (mtext, CLD_KEYFROM);
                    char *to = strstr (mtext, CLD_KEYTO);
                    char *subject = strstr (mtext, CLD_KEYSUBJECT);
                    char *headers = strstr (mtext, CLD_KEYHEADERS);
                    char *body = strstr (mtext, CLD_KEYBODY);
                    char *status = strstr (mtext, CLD_KEYSTATUS);

                    //
                    // After all options positions have been found, we must get the options 
                    // for ALL of them
                    //
                    carve_markup (&from, "send-mail", CLD_KEYFROM, 1, 1, 0, file_name, lnum);
                    carve_markup (&to, "send-mail", CLD_KEYTO, 1, 1, 0, file_name, lnum);
                    carve_markup (&subject, "send-mail", CLD_KEYSUBJECT, 1, 1, 0, file_name, lnum);
                    carve_markup (&headers, "send-mail", CLD_KEYHEADERS, 0, 1, 0, file_name, lnum);
                    carve_markup (&body, "send-mail", CLD_KEYBODY, 1, 1, 0, file_name, lnum);
                    carve_markup (&status, "send-mail", CLD_KEYSTATUS, 0, 1, 0, file_name, lnum);

                    //
                    // If there is data right after markup (i.e. 'send-mail') and it has no option (such as web-call https://...)
                    // then mtext is this option (in this case https://...). In this particular case, we don't have such an option -
                    // every option has a keyword preceding it, including the first one.
                    //

                    oprintf ("%s%scld_sendmail(%s, %s, %s, %s, %s);\n", status == NULL ? "":status, status == NULL ? "":"=", 
                        from, to, subject, headers == NULL ? "NULL" : headers, body);

                    BEGIN_TEXT_LINE

                    continue;
                }
                else if ((newI=recog_markup (line, i, "print-int", &mtext, &msize, 0, file_name, lnum)) != 0)  
                {
                    i = newI;
                    END_TEXT_LINE
                    oprintf("cld_printf (CLD_NOENC, \"%%d\", %.*s);\n", msize, mtext); 
                    BEGIN_TEXT_LINE

                    continue;
                }
                else if (((newI=recog_markup (line, i, "set-int", &mtext, &msize, 0, file_name, lnum)) != 0)  )
                {
                    i = newI;

                    END_TEXT_LINE
                    char *eq = strchr (mtext, '=');
                    if (eq == NULL)
                    {
                        oprintf("%.*s=0;\n", msize,mtext);
                    }
                    else
                    {
                        *eq = 0;
                        char *val = NULL;
                        CLD_STRDUP (val, eq+1);
                        int init_len = strlen (val);
                        cld_trim (val, &init_len);
                        oprintf ("%s=%s;\n", mtext, val); 
                    }
                    BEGIN_TEXT_LINE

                    continue;
                }
                else if ((newI=recog_markup (line, i, "copy-string", &mtext, &msize, 0, file_name, lnum)) != 0)
                {
                    i = newI;

                    END_TEXT_LINE
                    char *eq = strchr (mtext, '=');
                    if (eq == NULL)
                    {
                        oprintf("cld_copy_data (&(%.*s), \"\");\n",  msize, mtext); 
                    }
                    else
                    {
                        *eq = 0;
                        char *val = NULL;
                        CLD_STRDUP (val, eq+1);
                        int init_len = strlen (val);
                        cld_trim (val, &init_len);
                        oprintf ("cld_copy_data (&(%s), %s);\n", mtext, val); 
                    }
                    BEGIN_TEXT_LINE

                    continue;
                }
                else if (((newI=recog_markup (line, i, "if-string", &mtext, &msize, 0, file_name, lnum)) != 0)  
                    || ((newI1=recog_markup (line, i, "else-if-string", &mtext, &msize, 0, file_name, lnum)) != 0)  
                    || ((newI2=recog_markup (line, i, "if-string-case", &mtext, &msize, 0, file_name, lnum)) != 0)  
                    || ((newI3=recog_markup (line, i, "else-if-string-case", &mtext, &msize, 0, file_name, lnum)) != 0))  
                {
                    i = newI+newI1+newI2+newI3;
                    int else_if = 0;
                    int ignore_case = 0;
                    if (newI2 != 0 || newI3 != 0) ignore_case = 1;
                    if (newI1 != 0 || newI3 != 0) else_if = 1;
                    if (else_if == 1)
                    {
                        if (open_ifs == 0)
                        {
                            _cld_report_error( "Else-if-string found without an open if markup, reading file [%s] at line [%d]", file_name, lnum);
                        }
                        open_ifs--;
                        open_ifs++;
                    }
                    else
                    {
                        check_next_if
                    }
                    END_TEXT_LINE

                    char *if_str = cld_strdup (mtext);

                    // One can use both && and || and "and" and "or"
                    // "and" and "or" are converted to && and ||
                    //
                    // replace " or " with " || ". This will always work as they are of the same size
                    int len_if_str = strlen(if_str);
                    cld_replace_string (if_str, len_if_str+1, " or ", " || ", 1, NULL); 
                    // replace " and " with " && ". This will always work as "and" is longer than &&
                    cld_replace_string (if_str, len_if_str+1, " and ", " && ", 1, NULL); 

                    // parenthesis not allowed - this is a simple construct for simple string comparisions.
                    // Use multiple ifs to make statement clearer.
                    if (*find_unescaped_chars (if_str, "()") != 0)
                    {
                       _cld_report_error( "if-string cannot contain parenthesis, reading file [%s] at line [%d]", file_name, lnum);
                    }
                    // replace escaped () with unescaped
                    // this can still lead to incorrect statement if the parenthesis isn't in the string
                    // but that's up to programmer to make that mistake
                    cld_replace_string (if_str, len_if_str+1, "\\(", "(", 1, NULL); 
                    cld_replace_string (if_str, len_if_str+1, "\\)", ")", 1, NULL); 

                    int first_pass = 1;
                    while (1==1)
                    {
                        // find first logical operator
                        char *and = strstr(if_str, " && ");
                        char *or = strstr(if_str, " || ");
                        char *first_logical = NULL;
                        if (and!=NULL && or!=NULL)
                        {
                            if ((and-if_str)<(or-if_str))
                            {
                                first_logical = and;
                            }
                            else
                            {
                                first_logical = or;
                            }
                        }
                        else if (and!=NULL)
                        {
                            first_logical = and;
                        }
                        else if (or!=NULL)
                        {
                            first_logical = or;
                        }
                        else
                        {
                            first_logical = NULL;
                        }

                        if (first_logical != NULL) *first_logical = 0;

                        char *neq = strstr (if_str, "!=");
                        char *eq = NULL;
                        if (neq == NULL) eq = strchr (if_str, '=');
                        if (eq == NULL && neq == NULL)
                        {
                            _cld_report_error( "if-string must have '=' or '!=', reading file [%s] at line [%d]", file_name, lnum);
                        }
                        else
                        {
                            // extract left and right side of equal or not-equal
                            if (eq != NULL) *eq = 0;
                            if (neq != NULL) *neq = 0;
                            char *val = NULL;
                            CLD_STRDUP (val, eq != NULL ? eq+1 : neq+2);
                            int init_len = strlen (val);
                            cld_trim (val, &init_len);
                            if (eq != NULL)
                            {
                                if (first_pass == 1)
                                {
                                    oprintf("%sif (!str%scmp ((%s), (%s))", else_if == 1 ? "} else ":"", ignore_case==1 ? "case":"", if_str, val); 
                                }
                                else
                                {
                                    oprintf(" (!str%scmp ((%s), (%s))) ", ignore_case==1 ? "case":"", if_str, val); 
                                }
                            }
                            else
                            {
                                if (first_pass == 1)
                                {
                                    oprintf("%sif (str%scmp ((%s), (%s))\n", else_if == 1 ? "} else ":"", ignore_case==1 ? "case":"", if_str, val); 
                                }
                                else
                                {
                                    oprintf(" (str%scmp ((%s), (%s))) ", ignore_case==1 ? "case":"", if_str, val); 
                                }
                            }
                        }


                        first_pass = 0;
                        if (first_logical != NULL)
                        {
                            // string is now bla [space][0 which was | or &] [then either  | or &] bla
                            // we know 0 is equal to the next one
                            *(first_logical+1) = *(first_logical+2);
                            // print && or ||
                            if (*(first_logical+1) == '&')
                            {
                                oprintf(" && ");
                            }
                            else
                            {
                                oprintf(" || ");
                            }
                            // skip || or &&
                            if_str = first_logical + 4;
                        }
                        else break;
                    }
                    oprintf(") {\n");

                    BEGIN_TEXT_LINE

                    continue;
                }
                else if ((newI=recog_markup (line, i, "copy-string-from-int", &mtext, &msize, 0, file_name, lnum)) != 0)  
                {
                    i = newI;
                    END_TEXT_LINE
                    char *eq = strchr (mtext, '=');
                    if (eq == NULL)
                    {
                        oprintf("cld_copy_data_from_int (&%.*s, 0);\n", msize, mtext); 
                    }
                    else
                    {
                        *eq = 0;
                        char *val = NULL;
                        CLD_STRDUP (val, eq+1);
                        int init_len = strlen (val);
                        cld_trim (val, &init_len);
                        oprintf("cld_copy_data_from_int (&%s, %s);\n", mtext, val); 
                    }
                    BEGIN_TEXT_LINE

                    continue;
                }

                else if ((newI=recog_markup (line, i, "define-int", &mtext, &msize, 0, file_name, lnum)) != 0)  
                {
                    i = newI;
                    END_TEXT_LINE
                    char *eq = strchr (mtext, '=');
                    if (eq == NULL)
                    {
                        oprintf("int %.*s = 0;\n", msize, mtext); 
                    }
                    else
                    {
                        *eq = 0;
                        char *val = NULL;
                        CLD_STRDUP (val, eq+1);
                        int init_len = strlen (val);
                        cld_trim (val, &init_len);
                        oprintf("int %s = %s;\n", mtext, val); 
                    }
                    BEGIN_TEXT_LINE

                    continue;
                }
                else if ((newI=recog_markup (line, i, "print-error", &mtext, &msize, 0, file_name, lnum)) != 0)  
                {
                    i = newI;
                    END_TEXT_LINE
                    oprintf("fprintf(stderr, %.*s);\n", msize, mtext); 
                    BEGIN_TEXT_LINE

                    continue;
                }
                else if ((newI=recog_markup (line, i, "print-out", &mtext, &msize, 0, file_name, lnum)) != 0)  
                {
                    i = newI;
                    END_TEXT_LINE
                    oprintf("fprintf(stdout, %.*s);\n", msize, mtext); 
                    BEGIN_TEXT_LINE

                    continue;
                }
                else if ((newI=recog_markup (line, i, "report-error", &mtext, &msize, 0, file_name, lnum)) != 0)  
                {
                    i = newI;
                    END_TEXT_LINE
                    oprintf("cld_report_error(%.*s);\n", msize, mtext); 
                    BEGIN_TEXT_LINE

                    continue;
                }
                else if ((newI=recog_markup (line, i, "define-string", &mtext, &msize, 0, file_name, lnum)) != 0)  
                {
                    i = newI;
                    END_TEXT_LINE
                    char *eq = strchr (mtext, '=');
                    if (eq == NULL)
                    {
                        //
                        // default is empty string, because NULL would fail if such variable is to be fprintf-ed
                        // It's not cld_malloc("") for performance reasons. 'CLD_EMPTY_STRING' is an indicator that this
                        // string has not yet been initialized (and it may remain so while not being NULL).
                        //
                        oprintf("char *%.*s = CLD_EMPTY_STRING;\n", msize, mtext);
                    }
                    else
                    {
                        *eq = 0;
                        char *val = NULL;
                        CLD_STRDUP (val, eq+1);
                        int init_len = strlen (val);
                        cld_trim (val, &init_len);
                        //
                        // If string is initialized, then we can't rely on CLD_EMPTY_STRING to know its value hasn't
                        // been cld_malloc-ed, so we must must essentially strdup it.
                        //
                        oprintf("char *%s = cld_init_string (%s);\n", mtext, val); 
                    }
                    BEGIN_TEXT_LINE

                    continue;
                }
                else if ((newI=recog_markup (line, i, "set-cookie", &mtext, &msize, 0, file_name, lnum)) != 0)  
                {
                    i = newI;
                    END_TEXT_LINE
                    char *var = NULL;
                    CLD_STRDUP (var, mtext); // must have a copy because cld_trim could ruin further parsing, 
                                    // since we have 'i' up above already set to point in line
                    int var_len = strlen (var);
                    cld_trim (var, &var_len);
                    char *exp = strstr (var, CLD_KEY_EXPIRES);
                    char *path = strstr (var, CLD_KEY_PATH);
                    char *exp_date = NULL;
                    char *cookie_path = NULL;
                    // separate expiration and path for cookie so that beginning of each ends the previous
                    // string. Since they both must come after cookie name=value, they are each correct.
                    if (path != NULL)
                    {
                        *path = 0;
                    }
                    if (exp != NULL)
                    {
                        *exp = 0;
                    }
                    if (exp != NULL)
                    {
                        exp_date = cld_strdup (exp + strlen (CLD_KEY_EXPIRES));
                    }
                    if (path != NULL)
                    {
                        cookie_path = cld_strdup (path + strlen (CLD_KEY_PATH));
                    }
                    char *eq = strchr (var, '=');
                    if (eq == NULL)
                    {
                        _cld_report_error( "Equal sign missing, reading file [%s] at line [%d]", file_name, lnum);
                    }
                    *eq = 0;
                    char *value = eq+1;
                    // trim once more, since this name must not have whitespaces
                    var_len = strlen (var);
                    cld_trim (var, &var_len);

                    oprintf("cld_set_cookie (cld_get_config()->ctx.req, %.*s, %s, %s, %s);\n", var_len, var, value,
                        cookie_path == NULL ? "NULL" : cookie_path, exp_date == NULL ? "NULL" : exp_date);
                    BEGIN_TEXT_LINE

                    continue;
                }
                else if ((newI=recog_markup (line, i, "get-cookie", &mtext, &msize, 0, file_name, lnum)) != 0)  
                {
                    i = newI;
                    END_TEXT_LINE
                    char *var = NULL;
                    CLD_STRDUP (var, mtext); // must have a copy because cld_trim could ruin further parsing, 
                                    // since we have 'i' up above already set to point in line

                    // find out if cookie string is defined here or not
                    int is_def_cookie = 0;
                    is_opt_defined (&var, &is_def_cookie, file_name, lnum);

                    // find equal sign separating cookie variable and cookie name
                    char *eq = strchr (var, '=');
                    if (eq == NULL)
                    {
                        _cld_report_error( "Equal sign missing, reading file [%s] at line [%d]", file_name, lnum);
                    }
                    *eq = 0;
                    int var_len = strlen (var);
                    cld_trim (var, &var_len);
                    char *cname = eq + 1;
                    int c_len = strlen (cname);
                    cld_trim (cname, &c_len);
                    oprintf("%s%.*s = cld_find_cookie (cld_get_config()->ctx.req, %.*s, NULL, NULL, NULL);\n", is_def_cookie==1?"char *":"", var_len, var, c_len, cname); 
                    BEGIN_TEXT_LINE

                    continue;
                }
                else if ((newI=recog_markup (line, i, "delete-cookie", &mtext, &msize, 0, file_name, lnum)) != 0)  
                {
                    i = newI;
                    END_TEXT_LINE
                    char *var = NULL;
                    CLD_STRDUP (var, mtext); // must have a copy because cld_trim could ruin further parsing, 
                                    // since we have 'i' up above already set to point in line
                    int var_len = strlen (var);
                    cld_trim (var, &var_len);
                    oprintf("cld_delete_cookie (cld_get_config()->ctx.req, %.*s);\n", var_len, var); 
                    BEGIN_TEXT_LINE

                    continue;
                }
                else if ((newI=recog_markup (line, i, "input-param", &mtext, &msize, 0, file_name, lnum)) != 0)  
                {
                    i = newI;
                    END_TEXT_LINE
                    char *var = NULL;
                    CLD_STRDUP (var, mtext); // must have a copy because cld_trim could ruin further parsing, 
                                    // since we have 'i' up above already set to point in line
                    int var_len = strlen (var);
                    cld_trim (var, &var_len);
                    if (cld_is_valid_param_name(var) != 1)
                    {
                        _cld_report_error(CLD_NAME_INVALID, var, file_name, lnum); 
                    }
                    oprintf("char *%.*s = cld_get_input_param (cld_get_config()->ctx.req, \"%.*s\");\n", var_len, var, var_len, var); 
                    BEGIN_TEXT_LINE

                    continue;
                }
                else if (((newI=recog_markup (line, i, "subst-string", &mtext, &msize, 0, file_name, lnum)) != 0)  
                    || ((newI1=recog_markup (line, i, "subst-string-all", &mtext, &msize, 0, file_name, lnum)) != 0))
                {
                    i = newI+newI1;

                    int subst_all = 0;
                    if (newI1 != 0) subst_all = 1;


                    // subst-string[-all] x with y in z
                    char *with = strstr (mtext, CLD_KEYWITH);
                    if (with == NULL)
                    {
                        _cld_report_error( "'with' keyword is missing in %s reading file [%s] at line [%d]", subst_all == 1 ? "subst-string-all":"subst-string", file_name, lnum);
                    }

                    *with = 0;
                    char *subst_with = with + strlen(CLD_KEYWITH);
                    char *in = strstr (subst_with, CLD_KEYIN);
                    if (in == NULL)
                    {
                        _cld_report_error( "'in' keyword is missing in %s reading file [%s] at line [%d]", subst_all == 1 ? "subst-string-all":"subst-string", file_name, lnum);
                    }
                    *in = 0;
                    char *in_str = in + strlen(CLD_KEYIN);

                    END_TEXT_LINE

                    oprintf("cld_subst (&(%s), (%s), (%s), %d);\n", in_str, mtext, subst_with, subst_all);
                    BEGIN_TEXT_LINE
                    continue;

                }
                else if ((newI=recog_markup (line, i, "read-file", &mtext, &msize, 0, file_name, lnum)) != 0)  
                {
                    i = newI;
                    char *read_to = strstr (mtext, CLD_KEYTO);
                    if (read_to == NULL)
                    {
                        _cld_report_error( "'to' keyword is missing in read-file reading file [%s] at line [%d]", file_name, lnum);
                    }
                    *read_to = 0;
                    read_to = read_to + strlen(CLD_KEYTO);
                    char *status = strstr (read_to, CLD_KEYSTATUS);
                    int is_status_defined = 0;
                    if (status!=NULL)
                    {
                        *status = 0;
                        status += strlen(CLD_KEYSTATUS);
                        // check for 'define' for status only AFTER we place null char after BOTH STATUS and TO.
                        is_opt_defined (&status, &is_status_defined, file_name, lnum);
                    }
                    else
                    {
                        status="";
                    }
                    // We check for define on read "TO" here because there is a define in STATUS as well. So two defines. If only status had it, and we check for define
                    // on "TO" we would skip the whole status thing. When we find status, we put zero character there, so only THEN we are safe to look for DEFINE after TO:
                    // In other words, we have to place null chars after at STATUS and TO both, before attempting to discern 'define' on either one, or otherwise
                    // we might pickup define of the other.

                    int is_read_to_defined;
                    is_opt_defined (&read_to, &is_read_to_defined, file_name, lnum);

                    char *read_from = mtext;
                    END_TEXT_LINE
                    if (is_read_to_defined ==1)
                    {
                        oprintf("char *%s = NULL;\n", read_to);
                    }
                    oprintf("%s %s%scld_read_whole_file (%s, &(%s));\n", (status[0]!=0 && is_status_defined==1) ? "int":"", status[0]!=0 ? status:"",status[0]!=0 ?"=":"",read_from, read_to);
                    BEGIN_TEXT_LINE
                    continue;

                }
                else if ((newI=recog_markup (line, i, "copy-file", &mtext, &msize, 0, file_name, lnum)) != 0)  
                {
                    i = newI;
                    char *copy_to = strstr (mtext, CLD_KEYTO);
                    if (copy_to == NULL)
                    {
                        _cld_report_error( "'to' keyword is missing in copy-file reading file [%s] at line [%d]", file_name, lnum);
                    }
                    *copy_to = 0;
                    copy_to = copy_to + strlen(CLD_KEYTO);
                    char *status = strstr (copy_to, CLD_KEYSTATUS);
                    int is_status_defined = 0;
                    if (status!=NULL)
                    {
                        *status = 0;
                        status += strlen(CLD_KEYSTATUS);
                        is_opt_defined (&status, &is_status_defined, file_name, lnum);
                    }
                    else
                    {
                        status="";
                    }
                    char *copy_from = mtext;
                    END_TEXT_LINE
                    oprintf("%s %s%scld_copy_file (%s, %s);\n", (status[0]!=0 && is_status_defined==1) ? "int":"", status[0]!=0 ? status:"",status[0]!=0 ?"=":"",copy_from, copy_to);
                    BEGIN_TEXT_LINE
                    continue;

                }
                else if ((newI=recog_markup (line, i, "write-file", &mtext, &msize, 0, file_name, lnum)) != 0)  
                {
                    i = newI;
                    char *from = strstr (mtext, CLD_KEYFROM);
                    if (from == NULL)
                    {
                        _cld_report_error( "'from' keyword is missing in write-file reading file [%s] at line [%d]", file_name, lnum);
                    }
                    *from = 0;
                    char *write_from = from+strlen(CLD_KEYFROM);
                    char *append = strstr (write_from, CLD_KEYAPPEND);
                    char *length = strstr (write_from, CLD_KEYLENGTH);
                    char *status = strstr (write_from, CLD_KEYSTATUS);
                    int is_status_defined = 0;
                    int is_append = 0;
                    if (append!=NULL)
                    {
                        *append = 0;
                        is_append = 1;
                    }
                    if (status!=NULL)
                    {
                        *status = 0;
                        status += strlen(CLD_KEYSTATUS);
                        is_opt_defined (&status, &is_status_defined, file_name, lnum);
                    }
                    else
                    {
                        status="";
                    }
                    if (length!=NULL)
                    {
                        *length = 0;
                        length += strlen(CLD_KEYLENGTH);
                    }
                    else
                    {
                        length="0";
                    }
                    char *write_to = mtext;
                    END_TEXT_LINE
                    oprintf("%s %s%scld_write_file (%s, %s, %s, %d);\n", (status[0]!=0 && is_status_defined==1) ? "int":"", status[0]!=0 ? status:"",status[0]!=0 ?"=":"",write_to, write_from, length, is_append); 
                    BEGIN_TEXT_LINE
                    continue;
                }
                else if ((newI=recog_markup (line, i, "append-string", &mtext, &msize, 0, file_name, lnum)) != 0)  
                {
                    i = newI;
                    char *to = strstr (mtext, CLD_KEYTO);
                    if (to == NULL)
                    {
                        _cld_report_error( "'to' keyword is missing in append-string, reading file [%s] at line [%d]", file_name, lnum);
                    }
                    *to = 0;
                    char *append_to = to+strlen(CLD_KEYTO);
                    char *append_from = mtext;
                    END_TEXT_LINE
                    oprintf("cld_append_string (%s, &(%s));\n", append_from, append_to); 
                    BEGIN_TEXT_LINE
                    continue;
                }
                else if ((newI=recog_markup (line, i, "end-write-string", &mtext, &msize, 1, file_name, lnum)) != 0)  
                {
                    i = newI;
                    END_TEXT_LINE
                    oprintf("cld_write_to_string (NULL);\n"); 
                    BEGIN_TEXT_LINE
                    gen_ctx->total_write_string--;

                    // this if and setting non_cld to 0 comes from one-liner write-string. For example: <?write-string define x?>a<?end-write-string?>
                    // When end-write-string is recognized here and now, gen_ctx->total_write_string is 0 and non_cld is 1 - because
                    // there IS a non-cld character here (which is "a"). However, this is a one liner with nothing else on the line, and we should
                    // NOT output new line outside of the main loop that goes through the line.
                    // (see "Reference NEW_LINE_ON_NON_CLD" below).
                    // We should NOT clear non_cld if the line looks like 
                    // x<?write-string define x?>a<?end-write-string?> 
                    // or
                    // <?write-string define x?>a<?end-write-string?>x
                    // or
                    // x<?write-string define?>a<?end-write-string?>x
                    // because in these cases there are genuine non-cld characters (leading and trailing x character)
                    // ONLY if the line starts with <?write-string and ends here (with <?end-write-string?> can be clear the non_cld.
                    // Even if there are nested write-strings (for example
                    // <?write-string define z?>b<?write-string define x?>a<?end-write-string?>x<?end-write-string?>
                    // the logic still holds: if the line starts with <?write-string and this <?end-write-string?> is the last on line
                    // then there are NO non_cld characters on line (i.e. characters that will be printed outside the string and that
                    // warrant printing new line outside of the main loop).
                    // The if condition below reflects this logic. 'i' is the position of the last '>' in <?end-write-string?> and
                    // len is the length of the line. If i is equal (or greater) to len-1, then this '>' is the last character on the line.
                    //
                    const char *recog_write_string="<?write-string";
                    if (!strncmp (line, recog_write_string, strlen(recog_write_string)) && i >=(len-1))
                    {
                        non_cld=0;
                    }
                    continue;
                }
                else if ((newI=recog_markup (line, i, "web-address", &mtext, &msize, 1, file_name, lnum)) != 0)  
                {
                    i = newI;
                    END_TEXT_LINE
                    oprintf("cld_printf (CLD_WEB, \"%%s\", cld_web_address ());\n");
                    BEGIN_TEXT_LINE

                    continue;
                }
                else if ((newI=recog_markup (line, i, "write-string", &mtext, &msize, 0, file_name, lnum)) != 0)  
                {
                    i = newI;
                    END_TEXT_LINE
                    char *txt = cld_strdup (mtext);
                    int is_write_string_defined;
                    is_opt_defined (&txt, &is_write_string_defined, file_name, lnum);
                    if (is_write_string_defined==1)
                    {
                        oprintf("char *%s = cld_init_string (\"\");\n", txt);
                    }
                    oprintf("cld_write_to_string (&(%s));\n", txt); 
                    BEGIN_TEXT_LINE
                    gen_ctx->total_write_string++;

                    continue;
                }
                else if ((newI=recog_markup (line, i, ";", &mtext, &msize, 1, file_name, lnum)) != 0)  
                {
                    // <?;?> is used to print semi-colon at the end without being C code.
                    i = newI;
                    // no new line here - we just output semicolon 
                    oprintf(";"); 

                    continue;
                }
                else if ((newI=recog_markup (line, i, "print-long", &mtext, &msize, 0, file_name, lnum)) != 0)  
                {
                    i = newI;
                    END_TEXT_LINE
                    oprintf("cld_printf (CLD_NOENC, \"%%ld\", %.*s);\n", msize, mtext); 
                    BEGIN_TEXT_LINE

                    continue;
                }
                else if ((newI=recog_markup (line, i, "for", &mtext, &msize, 0, file_name, lnum)) != 0)  
                {
                    i = newI;
                    END_TEXT_LINE
                    oprintf("for (%.*s) { \n", msize, mtext); 
                    BEGIN_TEXT_LINE

                    check_next_for
                    continue;
                }
                else if ((newI=recog_markup (line, i, "if", &mtext, &msize, 0, file_name, lnum)) != 0)  
                {
                    i = newI;
                    END_TEXT_LINE
                    oprintf("if (%.*s) { \n", msize, mtext); 
                    BEGIN_TEXT_LINE

                    check_next_if
                    continue;
                }
                else if ((newI=recog_markup (line, i, "else", &mtext, &msize, 1, file_name, lnum)) != 0)  
                {
                    i = newI;
                    END_TEXT_LINE
                    oprintf("} else {\n");
                    BEGIN_TEXT_LINE
                    if (open_ifs == 0)
                    {
                        _cld_report_error( "Else found without an open if markup, reading file [%s] at line [%d]", file_name, lnum);
                    }
                    open_ifs--;
                    open_ifs++;

                    continue;
                }
                else if ((newI=recog_markup (line, i, "end-for", &mtext, &msize, 1, file_name, lnum)) != 0)  
                {
                    i = newI;
                    END_TEXT_LINE
                    oprintf("}\n");
                    BEGIN_TEXT_LINE
                    open_for--;

                    continue;
                }
                else if ((newI=recog_markup (line, i, "end-if", &mtext, &msize, 1, file_name, lnum)) != 0)  
                {
                    i = newI;
                    END_TEXT_LINE
                    oprintf("}\n");
                    BEGIN_TEXT_LINE
                    open_ifs--;

                    continue;
                }
                else if ((newI=recog_markup (line, i, "else-if", &mtext, &msize, 0, file_name, lnum)) != 0)  
                {
                    i = newI;
                    END_TEXT_LINE
                    oprintf("} else if (%.*s) {\n", msize, mtext);
                    BEGIN_TEXT_LINE
                    if (open_ifs == 0)
                    {
                        _cld_report_error( "Else-if found without an open if markup, reading file [%s] at line [%d]", file_name, lnum);
                    }
                    open_ifs--;
                    open_ifs++;

                    continue;
                }
                else if (usedCLD == 1)
                {
                    _cld_report_error( "Unrecognized markup, reading file [%s] at line [%d]", file_name, lnum);
                }
                else
                {
                    ; // else? if <?cld-markup not used ?>, then this character of the line is not CLD markup
                    // so it is printed out (as XHTML possibly)
                }
            }
            first_on_line = 0;

            // if this is start-c/end-c or start-comment/end-comment, we shouldn't print out anything, meaning no new lines anywhere
            // so we consider it all 'cld', i.e. not non-cld, because it is just C code or comments that we don't interpret
            if (is_c_block == 0 && is_comment_block == 0)
            {
                if (line[i] == '"') 
                {
                    non_cld = 1;
                    oprintf("\\\"");
                }
                else if (line[i] == '\\') 
                {
                    non_cld = 1;
                    oprintf("\\\\");
                }
                else if (line[i] == '\n' || line[i] == 0 || i>=len) 
                {
                    // should not get here because we cld_trim line, and never reach 0 char
                    // and should never go beyond the length of the line
                    _cld_report_error ("Parsing error, extra text found, [%s]", line);
                }
                else if (line[i] == '%')
                {
                    non_cld = 1;
                    oprintf("%%");
                }
                else 
                {
                    if (!isspace(line[i])) non_cld = 1;
                    oprintf("%c", line[i]);
                    // after the line is printed in its entirety, this last char printed CANNOT be whitespace because we trim line
                    // we use this last_char_printed to check if the last character in the line was semicolon. 
                    last_char_printed = line[i];
                }
            }
            else
            {
                // this is if C-code is on the same line with <?start-c?> or <?end-c?> or both
                oprintf("%c", line[i]);
            }
        }

        // if C code and comment, do not finish any lines, because we don't start any. We only start a new line 
        // in end-c/end-comment, at which point is_c_block/is_comment_block is now 0.
        if (is_c_block == 0 && is_comment_block == 0)
        {
            if (non_cld == 1 || (gen_ctx->total_write_string>0 && len==0))
            {
                // we print new line if there are non-white-space characters in the line, once all CLD code is removed, OR
                // if this is writing to string and (once trimmed), the line is geniuinly empty. This is different from a line
                // being empty ONCE cld code is removed - it wasn't empty to begin with - we don't want to print for those line, but
                // do want to print for lines where there (was nothing in the line AND it is writing to string). The same is not true otherwise,
                // i.e. if there is nothing in the line and it is NOT writing to string, this is just writing HTML and we don't care.
                // Reference NEW_LINE_ON_NON_CLD
                oprintf("\\n\");\n");
                if (last_char_printed==';' && is_verbatim == 0)
                {
                    // if line ends with ; we think of it as C code. We think this is a mistake and should have been C code.
                    // so we will error out and advise to use <?print-web ";"?> if it is a genuine HTML output
                    // This is ONLY if we're not outputting verbatim in which case ALL markups are ignored EXCEPT // comments.
                    _cld_report_error( "Line ending with semi-colon is considered C code, but it does not belong to a C block (c or start-c) - if this is HTML text that ends with semicolon, use <?;?>, reading file [%s] at line [%d]",  file_name, lnum);
                }
            }
            else
            {
                END_TEXT_LINE
            }
        }

        if (feof (f)) break; // something read in, then EOF
    }

    if (gen_ctx->total_write_string != 0)
    {
        _cld_report_error( "Imbalance in write-string/end-write-string markups, too many open or not closed, reading file [%s] at line [%d]", file_name, lnum);
    }

    if (is_c_block == 1)
    {
        _cld_report_error( "start-c without matching end-c, reading file [%s] at line [%d]",  file_name, lnum);
    }
    if (is_comment_block == 1)
    {
        _cld_report_error( "start-comment without matching end-comment, reading file [%s] at line [%d]",  file_name, lnum);
    }
    if (open_queries != 0)
    {
        _cld_report_error( "'query' code block imbalance at line %d, %d %s open than closed, reading file [%s] at line [%d]", last_line_query_closed, abs(open_queries), open_queries > 0 ? "more" : "less" , file_name, lnum);
    }
    if (open_for != 0)
    {
        _cld_report_error( "'for' code block imbalance at line check line %d, %d %s open than closed, reading file [%s] at line [%d]", last_line_for_closed, abs(open_for), open_for > 0 ? "more" : "less" , file_name, lnum);
    }
    if (is_verbatim != 0)
    {
        _cld_report_error( "'start-verbatim' found never closed with 'end-verbatim', reading file [%s] at line [%d]", file_name, lnum);
    }
    if (open_ifs != 0)
    {
        _cld_report_error( "'if' code block imbalance at line check line %d, %d %s open than closed, reading file [%s] at line [%d]", last_line_if_closed, abs(open_ifs), open_ifs > 0 ? "more" : "less" , file_name, lnum);
    }
    if (gen_ctx->curr_qry_ptr !=0)
    {
        _cld_report_error( "Query imbalance (too many queries opened, too few closed), reading file [%s] at line [%d]", file_name, lnum);
    }

}

#undef CLD_FILE
#undef CLD_LINE
#define CLD_FILE "[no file opened yet]"
#define CLD_LINE 0


//
// Main code, generates the source C file (as a __<file name>.c) based on input file
// and input parameters
//

int main (int argc, char* argv[])
{

    cld_memory_init();

    cld_gen_ctx *gen_ctx = (cld_gen_ctx*)cld_malloc(sizeof (cld_gen_ctx));
    init_cld_gen_ctx (gen_ctx);

    const char *tool = TOOL;

    if (argc == 2 && !strcmp (argv[1], "-version"))
    {
        // print version
        fprintf (stdout, "%s\n", CLD_MAJOR_VERSION);
        exit(0);
    }

    //
    // The following two options allow URL and WEB encoding of strings
    // from the command line. Typically used to construct URLs for command line
    // execution.
    //
    if (argc == 3 && !strcmp (argv[1], "-urlencode"))
    {
        // display URL encoded version of a string
        char *res = NULL;
        cld_encode (CLD_URL, argv[2], &res);
        fprintf (stdout, "%s", res);
        exit(0);
    }
    if (argc == 3 && !strcmp (argv[1], "-webencode"))
    {
        // display URL encoded version of a string
        char *res = NULL;
        cld_encode (CLD_WEB, argv[2], &res);
        fprintf (stdout, "%s", res);
        exit(0);
    }

    //
    // Get help from command line
    //
    if (argc == 1 || (argc == 2 && (!strcmp (argv[1], "-help") || !strcmp (argv[1], "help") || !strcmp (argv[1], "?") 
            || !strcmp (argv[1], "--help"))))
    {
        // display help
        int show_color=((argc>1 && !strcmp(argv[1],"?")) ? 0:1);
        char *BOLD_ON;
        char *RED_ON;
        char *TERMINAL_OFF;
        if (show_color==1)
        {
            BOLD_ON = "\x1B[1m";
            RED_ON="\x1B[31m";
            TERMINAL_OFF="\x1B[0m";
        }
        else
        {
            BOLD_ON = "^";
            RED_ON="~ ";
            TERMINAL_OFF="~~";
        }
        // display help and exit
        tfprintf (stdout, "\t " "%s" "Name" "%s" "\n", BOLD_ON,TERMINAL_OFF);
        tfprintf (stdout, "\t \n");
        tfprintf (stdout, "\t %s code generator, markup language and application server API, version [%s]\n", tool, CLD_MAJOR_VERSION);
        tfprintf (stdout, "\t \n");
        tfprintf (stdout, "\t " "%s" "Description" "%s" "\n", BOLD_ON,TERMINAL_OFF);
        tfprintf (stdout, "\t \n");
        tfprintf (stdout, "\t %s is a tool for building Web applications in C"
       " that run as modules on Apache web server on RedHat/Centos. It supports"
        " mariaDB database by using LGPL mariaDB client"
        " that enables connectivity to mariaDB database."
        " Each application runs under the same Apache web server user, under its own directory (i.e. application's home directory).\n", tool);
        tfprintf (stdout, "\t \n");
        tfprintf (stdout, "\t " "%s" "Synopsis" "%s" "\n", BOLD_ON,TERMINAL_OFF);
        tfprintf (stdout, "\t \n");
        tfprintf (stdout, "\t %s [<input-file-name.v>] [<command-line-options>]\n", TOOL_CMD);
        tfprintf (stdout, "\t \n");
        tfprintf (stdout, "\t " "%s" "Options" "%s" "\n", BOLD_ON,TERMINAL_OFF);
        tfprintf (stdout, "\t \n");
        tfprintf (stdout, "\t " "%s" "-help" "%s" "\n",RED_ON,TERMINAL_OFF);
        tfprintf (stdout, "\t\t Display this help.\n");
        tfprintf (stdout, "\t \n");
        tfprintf (stdout, "\t " "%s" "-out <output-file-name.c>" "%s" "\n",RED_ON,TERMINAL_OFF);
        tfprintf (stdout, "\t\t Write generated code to output file <output-file-name.c>. If this option"
        " is not used, generated code is written to stdout (standard output).\n");
        tfprintf (stdout, "\t \n");
        tfprintf (stdout, "\t " "%s" "-main" "%s" "\n",RED_ON,TERMINAL_OFF);
        tfprintf (stdout, "\t\t Generate main C code. This option cannot be used when <input-file-name.c>"
        " is specified, i.e. either C code is generated for input-file-name.c or"
        " the main() function C code is generated. \n");
        tfprintf (stdout, "\t \n");
        tfprintf (stdout, "\t " "%s" "-cmd" "%s" "\n",RED_ON,TERMINAL_OFF);
        tfprintf (stdout, "\t\t Generate C code for use as a standalone program (a command line program), rather than as an Apache module (Apache Mod) program which is the default."
        " This option can only be used together with -main.\n");
        tfprintf (stdout, "\t \n");
        tfprintf (stdout, "\t " "%s" "-mariasock <socket-file-location>" "%s" "\n",RED_ON,TERMINAL_OFF);
        tfprintf (stdout, "\t\t Specify the location of the mariaDB socket file, used by"
        " the database server (socket option in my.cnf).\n");
        tfprintf (stdout, "\t \n");
        tfprintf (stdout, "\t " "%s" "-v" "%s" "\n",RED_ON,TERMINAL_OFF);
        tfprintf (stdout, "\t\t Print out verbose information about what is being done.\n");
        tfprintf (stdout, "\t \n");
        tfprintf (stdout, "\t " "%s" "-urlencode <string>" "%s" "\n",RED_ON,TERMINAL_OFF);
        tfprintf (stdout, "\t\t Prints URL encoded <string>.\n");
        tfprintf (stdout, "\t \n");
        tfprintf (stdout, "\t " "%s" "-webencode <string>" "%s" "\n",RED_ON,TERMINAL_OFF);
        tfprintf (stdout, "\t\t Prints web encoded <string>.\n");
        tfprintf (stdout, "\t \n");
        tfprintf (stdout, "\n");    
        tfprintf (stdout, "\t " "%s" "COPYRIGHT AND LICENSE" "%s" "\n",BOLD_ON,TERMINAL_OFF);
        tfprintf (stdout, "\t \n");
        tfprintf (stdout, "\t Copyright (c) 2017 Dasoftver LLC (on the web at https://bitbucket.org/dasoftver/cloudgizer).\n");
        tfprintf (stdout, "\t Cloudgizer is free Open Source Software licensed under Apache License 2. Cloudgizer is \"AS IS\" without warranties or guarantees of any kind.\n");
        tfprintf (stdout, "\n");    
        exit (0);
    }

    //
    // parse input parameters
    //
    int i;
    char *out_file_name = NULL;
    char *file_name = NULL;
    char *_item = NULL;
    int _cmd_mode = 0;
    int _main = 0;
    for (i = 1; i < argc; i ++)
    {
        if (!strcmp (argv[i], "-main"))
        {
            _main = 1;
        }
        else if (!strcmp (argv[i], "-out"))
        {
            if (i + 1 >= argc)
            {
                fprintf(stderr, "Output file not specified after -out option\n");
                exit (1);
            }
            CLD_STRDUP (out_file_name, argv[i+1]);
            i++; // skip db location now
            continue;
        }
        else if (!strcmp (argv[i], "-cmd"))
        {
            _cmd_mode = 1;
        }
        else if (!strcmp (argv[i], "-v"))
        {
            verbose = 1;
        }
        else if (!strcmp (argv[i], "-mariasock"))
        {
            if (i + 1 >= argc)
            {
                fprintf(stderr, "Socket location for db not specified after -sock option\n");
                exit (1);
            }
            setenv ("MYSQL_UNIX_PORT", argv[i+1], 1);
            i++; // skip socket location now
            continue;
        }
        else
        {
            _item = argv[i];
            if (file_name != NULL)
            {
                fprintf(stderr, "Only one file name can be specified for processing, already specified [%s]\n", file_name);
                exit (1);
            }
            if (file_name != NULL && _main == 1)
            {
                fprintf(stderr, "Cannot specify file name to process [%s], and the -main option to generate program main code. Use one or the other.\n", file_name);
                exit (1);
            }
            file_name = _item;

        }
    }

    // get .db location (in home directory)
    char home_dir[300];
    char cwd[300];
    //
    // First we get home directory, because cld_handler_name is set to "" by default
    // It always ends with '/'
    //
    snprintf (home_dir, sizeof (home_dir), "%s", cld_home_dir());
    //
    // Then we get current working directory
    //
    if (getcwd (cwd, sizeof(cwd)) == NULL)
    {
        fprintf(stderr, "Cannot get current working directory, error [%s]\n", strerror(errno));
        exit (1);
    }
    int home_dir_len = strlen(home_dir);
    if (!strncmp (cwd, home_dir, home_dir_len))
    {
        char *app_name = cwd+home_dir_len;
        char *slash = strchr (app_name, '/');
        if (slash != NULL)
        {
            *slash = 0;
        }
        //
        // app_name is now application name, it's a directory one above home directory in which we are,
        // and it MUST have .db file there - this is how cldpackapp sets up directory structure!
        //
        cld_handler_name = cld_strdup(app_name);
        //
        // verify app_name is like C identifier lesser than 17 bytes (for mariadb user name). Cannot be deploy
        // because deployment will create a directory under home account with the name of application, and we use
        // deploy directory to unpack into first (first stage of deployment).
        //
        if (cld_is_valid_param_name(cld_handler_name) != 1 || strlen(cld_handler_name)>16 || !strcmp(cld_handler_name,"deploy"))
        {
            fprintf(stderr, "Application name [%s] must start with a character, have only characters, digits or underscore, be 16 or less in length, and it cannot be 'deploy'.\n", cld_handler_name);
            exit (1);
        }

    }
    else
    {
        fprintf(stderr, "You must be in an application directory under current user's home directory, for example %s/application_name\n", home_dir);
        exit (1);
    }
    //
    // Since we have cld_handler_name set now, cld_home_dir() will return correct application home directory
    //
    char db_config_name[300];
    snprintf (db_config_name, sizeof(db_config_name), "%s/.db", cld_home_dir());
    CLD_VERBOSE(0, "Using .db file at [%s]", db_config_name);
    CLD_STRDUP (gen_ctx->db, db_config_name);
    // we ONLY set db (file name of db credentials) so that SQL queries succeed
    // (such as describing tables)
    cld_config *pc = cld_get_config ();
    pc->app.db = gen_ctx->db;
    //
    // We must set various variables used in CLD shared library, for example, global connection data (description, transaction marker, 
    // connection marker). Encryption function is set too, as well as other variable.
    // This is the SAME code as below generated for application. This is because for CLD, we need it in order for db connection etc. to work.
    // And we need it for application too. Here in cld, we don't need it to be static since it's only one run of this process.
    // We can't really put this code in a common place because it spans two projects. Perhaps something could be done with generated code
    // but the effort doesn't seem worth it.
    //
    static MYSQL *g_con = NULL;
    static int is_begin_transaction = 0;
    static int has_connected = 0;
    CTX.db.is_begin_transaction = &is_begin_transaction;
    CTX.db.g_con = &g_con;
    CTX.db.has_connected = &has_connected;



    if (out_file_name != NULL)
    {
        outf = fopen (out_file_name, "w");
        if (outf == NULL)
        {
            fprintf(stderr, "Cannot open output file [%s] for writing, error [%s]\n", out_file_name, strerror(errno));
            exit (1);
        }
    }


    if (_main == 0 && file_name == NULL)
    {
        fprintf(stderr, "Neither -main option or the file name to process is specified.\n");
        exit (1);
    }


    if (_cmd_mode == 1)
    {
        if (_main != 1)
        {
            fprintf(stderr, "-main option must be specified when using -cmd option.\n");
            exit (1);
        }
        gen_ctx->cmd_mode = 1;
    }

    assert (gen_ctx->db);


    if (_main == 1)
    {
        // Generate C code
        //
        CLD_VERBOSE(0,"Generating main code");

        oprintf("#include \"cld.h\"\n");

        // 
        // code can be generated for standalone (program) version that can be executed from command line
        // or a web-server plug-in for the web
        //

        if (gen_ctx->cmd_mode == 0)
        {
            oprintf("int cld_main (void *apa_req)\n");
            oprintf("{\n");
        }
        else
        {
            oprintf("int main (int argc, char *argv[])\n");
            oprintf("{\n");
            oprintf("int test_version=0;\n");
            oprintf("CLD_UNUSED (argc);\n");
            oprintf("CLD_UNUSED (argv);\n");
            oprintf ("if (argc >= 2 && !strcmp(argv[1],\"-t\")) test_version=1;\n");

        }

        // BEFORE anything, must destroy previous request's memory and initialize memory for this request
        oprintf("cld_memory_init();\n");

        // even for apache mod, cld_config doesn't, it is just 'reset' with invoke
        oprintf("cld_config *pc = NULL;\n");
        // user-only permissions
        oprintf("umask (S_IRWXO+S_IRWXG);\n");
        oprintf("cld_get_tz ();\n");
        //
        // This order (clear config, then get config) MUST remain so - see usage of
        // static cache in cldrt.c to make getting config faster.
        //
        oprintf("cld_clear_config ();\n");
        oprintf("pc = cld_get_config ();\n");
        //
        // Set static variables that are application specific. For example, cached db connection is different for each
        // module, however the same shared library code is handling it for each module that might be running in the process.
        // Most variables are module-static. Some are function pointer (lik oops), so the dso calls the error handler
        // of the module actually executing and not some other module's, for instance.
        // ****
        // This code MUST execute in cld process startup above - see the same code above.
        // ****
        //
        oprintf ("static MYSQL *g_con = NULL;\n");
        oprintf ("static int is_begin_transaction = 0;\n");
        oprintf ("static int has_connected = 0;\n");
        oprintf ("CTX.db.is_begin_transaction = &is_begin_transaction;\n");
        oprintf ("CTX.db.g_con = &g_con;\n");
        oprintf ("CTX.db.has_connected = &has_connected;\n");
        oprintf ("CTX.callback.file_too_large_function = &file_too_large;\n");
        oprintf ("CTX.callback.oops_function = &oops;\n");

        // for not-cmd, we need to save the handle for request
        // we do this apa_req setting twice in each branch of IF/ELSE because if cld_get_runtime_options fails
        // we need to issue message and for that we need apa_req for apache
        if (gen_ctx->cmd_mode == 0)
        {
            oprintf("pc->ctx.apa = apa_req;\n");
        }


        // read config file
        oprintf ("if (cld_get_runtime_options(&(pc->app.version), &(pc->app.log_directory), &(pc->app.html_directory), &(pc->app.max_upload_size), &(pc->app.user_params),\n\
            &(pc->app.web), &(pc->app.email), &(pc->app.file_directory), &(pc->app.tmp_directory), &(pc->app.db), &(pc->app.mariadb_socket), &(pc->app.ignore_mismatch)) != 1)\n");
        oprintf ("{\n");
        char *conf_message = "Cannot read 'config' configuration file. Please make sure this file exists in the application's home directory and has the appropriate privileges.<br/>";
        if (gen_ctx->cmd_mode == 0)
        {
            oprintf ("cld_ws_set_content_type(pc->ctx.apa, \"text/html;charset=utf-8\");\n");
            oprintf ("cld_ws_printf (pc->ctx.apa, \"%%s\", \"%s\");\n",conf_message);
        }
        else
        {
            oprintf ("fputs (\"%s\", stdout);\n",conf_message);
        }
        oprintf ("return 1;\n");
        oprintf ("}\n");

        //
        // Make sure major version of CLD is the same as the one loaded in Apache
        //
        oprintf ("if (strcmp (pc->app.ignore_mismatch, \"yes\") && strcmp (cld_major_version(),CLD_MAJOR_VERSION))\n");
        oprintf ("{\n");
        char version_message[300];
        snprintf (version_message, sizeof(version_message), "Cloudgizer version on this web server is [%s]. Your application is built with version [%s]. Either build your application with version [%s] or set 'ignore_mismatch' variable in 'config' configuration file to 'yes'.<br/>", cld_major_version(), CLD_MAJOR_VERSION, cld_major_version());
        if (gen_ctx->cmd_mode == 0)
        {
            oprintf ("cld_ws_set_content_type(pc->ctx.apa, \"text/html;charset=utf-8\");\n");
            oprintf ("cld_ws_printf (pc->ctx.apa, \"%%s\", \"%s\");\n",version_message);
        }
        else
        {
            oprintf ("fputs (\"%s\", stdout);\n",version_message);
        }
        oprintf ("return 1;\n");
        oprintf ("}\n");
        if (gen_ctx->cmd_mode == 1)
        {
            oprintf ("if (test_version==1) {fputs(\"OK\\n\",stdout);\nreturn 0;}\n");
        }

        //
        // Obtain debugging options
        //
        oprintf("cld_get_debug_options();\n");

        //
        // trace_cld must be the very first one, or tracing won't work
        //
        oprintf("cld_open_trace();\n");
        //
        // Setup crash handler. Also print out all shared libraries loaded and their start/end addresses.
        //
        oprintf("cld_set_crash_handler (cld_get_config()->app.log_directory);\n");
        oprintf("so_info *so;\n");
        oprintf("int tot_so = cld_total_so (&so);\n");
        oprintf ("int it; for (it = 0; it < tot_so; it++) {CLD_TRACE(\"Library loaded: [%%s], start [%%p], end [%%p]\", so[it].mod_name, so[it].mod_addr, so[it].mod_end);}\n");

        //
        // Setup mariadb socket port
        //
        oprintf("if (pc->app.mariadb_socket != NULL) setenv(\"MYSQL_UNIX_PORT\", pc->app.mariadb_socket, 1);\n");
        //
        // Initialize curl
        //
        oprintf("curl_global_init(CURL_GLOBAL_ALL);\n");

        oprintf("CLD_TRACE (\"max_upload_size = %%ld\", pc->app.max_upload_size);\n");
        oprintf("CLD_TRACE (\"web = %%s\", pc->app.web);\n");
        oprintf("CLD_TRACE (\"email = %%s\", pc->app.email);\n");
        oprintf("CLD_TRACE (\"file_directory = %%s\", pc->app.file_directory);\n");
        oprintf("CLD_TRACE (\"db = %%s\", pc->app.db);\n");
        oprintf("CLD_TRACE (\"mariadb_socket = %%s\", pc->app.mariadb_socket);\n");
        oprintf("reset_cld_config (pc);\n");

        //
        // If in web server container, initialize request handler
        //
        if (gen_ctx->cmd_mode == 0)
        {
            oprintf("pc->ctx.apa = apa_req;\n");
        }


        // need to startprint, it is automatic with first cld_printf

        oprintf("input_req *req = (input_req*)cld_malloc (sizeof (input_req));\n");
        oprintf("cld_init_input_req(req);\n");


        // if debugging (sleep parameter in trace/debug file), sleep to give programmer
        // a chance to attach to program 
        oprintf("CLD_TRACE (\"STARTING REQUEST [%%s]\", pc->app.log_directory);\n");
        oprintf("if (pc->debug.sleep != -1) sleep (pc->debug.sleep);\n");

        // get URL input and if bad, show error message to browser
        oprintf("pc->ctx.req = req;\n");
        oprintf("req->app = &(pc->app);\n");

        // this gets input from either web server or from the environment
        // We check if return is 1 or 0. If not 1, this means there was a problem, but it was
        // handled (such as Forbidden reply) (otherwise there would be an erorring out). If return
        // value is 0, we just go directly to cld_shut() to flush the response out.
        //
        oprintf("if (cld_get_input(req, NULL, NULL) == 1)\n");
        oprintf("{\n");

        // main function that handles everything - programmer must implement this
        oprintf("cld_handle_request();\n");

        // if there is a transaction that wasn't explicitly committed or rollbacked
        // rollback it, so it doesn't mess up the next request. Application programmer
        // should use cld_check_transaction() it app code to error out prior to this (
        // in cld_handle_request()).
        oprintf("cld_check_transaction (2);\n");
        oprintf("}\n");

        //
        // cld_shut MUST ALWAYS be called at the end - no request can bypass it
        // It does much cleanup and resetting of variables that it cannot be skipped
        // for the next request to succeed.
        //
        oprintf("cld_shut(req);\n");
        oprintf("return 0;\n");
        oprintf("}\n");
        CLD_VERBOSE(0,"End generating main code");
    }
    else
    {
        CLD_VERBOSE(0,"Generating code for [%s]", file_name);
        cld_gen_c_code (gen_ctx, file_name);
    }

    oprintf("// END OF GENERATED CODE\n");
    oprintf (NULL); //  flush output
    if (outf != NULL) fclose (outf);

    // release of memory done automatically by the OS and likely faster, so no
    // cld_done();
    return 0;
}




