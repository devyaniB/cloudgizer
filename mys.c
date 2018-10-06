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
// interface to MariaDB: uses MariaDB LGPL client only
// DB connection is for the life of process - if it goes down, so does
// the process. Set wait_timeout mariadb server variable to maximum to
// avoid daily disconnects (if inactive).
//

#include "cld.h"

//
// Note: we ALWAYS use cld_get_db_connection (fname) explicitly whenever the database
// connection is needed (as opposed to setting a variable MYSQL *con = cld_get_db_connection (fname);
// and then using the variable). The reason is that, if db connection drops and can be reestablished,
// we will reconnect. This happens somewhat transparent, so code that would use a variable like above
// may use stale (old and unusable) connection instead of a new, reconnected one. For that reason, the
// practice of using the latest cld_get_db_connection (fname) makes sure we always use actual live
// db connection.
//


// 
// set in a caller (CLD generates this) so that errors in SQL code have a caller contents
// (function name and line number)
//
extern const char *func_name;
extern int func_line;

// max number of columns
#define MYS_COL_LIMIT 4096


// function prototypes
int cld_handle_error (const char *s, MYSQL *con, unsigned int *er, const char **err_message, int retry);

// 
// Close database connection
// We NEVER close connection in our code, EXCEPT on shutdown of command line program.
//
void cld_close_db_conn ()
{
    CLD_TRACE("");
    cld_get_db_connection (NULL);
}

// 
// Get database connection. 'fname' is the file name with database
// credentials. Caches connection and connects ONE TIME only. We use
// a db connection "for-ever", i.e. as long as possible.
// Returns MYSQL connection data or NULL if failed to connect.
// If fname is NULL, connection to the db will be closed.
// Sets connection charset to UTF-8.
// 
MYSQL *cld_get_db_connection (const char *fname)
{
    CLD_TRACE("");

    if (*(CTX.db.g_con) != NULL) 
    {
        CLD_TRACE ("using cached db connection");
        if (fname == NULL)
        {
            //
            // This is closing DB connection
            //
            *(CTX.db.is_begin_transaction) = 0; // we are no longer in a transaction if
                        // connection closed

            mysql_close (*(CTX.db.g_con));
            *(CTX.db.g_con) = NULL;
            return NULL;
        }
        return *(CTX.db.g_con); 
            // already connected
    }
    else
    {
        // this is closing an already closed connection
        if (fname == NULL) return NULL;
    }
    

    // 
    // One process has ONLY one connection. We can reconnect ONLY if we were NOT in a transaction. 
    // 'Reconnecting' means 'has_connected' is 1, and we're trying to connect again.
    // We cannot reconnect if we are in a transaction, because the database would have automatically
    // rollback on disconnect (that happened prior to this) and it would be complicated for an application to recover from this.
    // If we try to connect a second time in this case, error out.
    // *** You should set wait_timeout server variable to its maximum value to avoid daily
    // *** disconnects!!! (assuming inactivity)
    //
    if (*(CTX.db.has_connected)==1)
    {
        if (*(CTX.db.is_begin_transaction) ==1) 
        {
            //
            // If we're in a transaction, we have to exit as we cannot recover from a rollback of a
            // transaction initiated by the application. If however, we are not in a transaction,
            // we can safely establish a new connection and let application continue with a new
            // transaction or a single-statement transaction.
            //
            cld_report_error ("The connection to database has been lost, exiting..."); 
        }
        //
        // Here we are if we're not in a transaction
        //
    }
    *(CTX.db.has_connected) = 1;


    // 
    // The following (due to above guard) is executed ONLY ONCE for the life of process
    //

    *(CTX.db.is_begin_transaction) = 0; // we are no longer in a transaction if
                        // connection is being (re)opened, this is implicit rollback

    *(CTX.db.g_con) = mysql_init(NULL);
   
    if (*(CTX.db.g_con) == NULL) 
    {
        cld_report_error ("Cannot initialize database connection");
        return NULL; // just for compiler, never gets here
    }  

    // Obtain credentials from a secure store
    // and wipe them from memory once connection is
    // established
    char host[CLD_SECURITY_FIELD_LEN + 1];
    char name[CLD_SECURITY_FIELD_LEN + 1];
    char passwd[CLD_SECURITY_FIELD_LEN + 1];
    char db[CLD_SECURITY_FIELD_LEN + 1];
    if (cld_get_credentials(host,name,passwd,db,fname) != 0)
    {
        *(CTX.db.g_con) = NULL;
        struct passwd *pwd = getpwuid(geteuid()); 

        cld_report_error ("Cannot get database credentials, make sure default credentials file has the correct server name, user name, password and existing database name. Credentials file is [%s]: it must have access permission of 600, it must be owned by this user (%s) and the directory leading to it must be accessible to this user", fname, pwd->pw_name);
        return NULL;
    }

    CLD_TRACE ("Logging in to database: Connecting to host [%s], user [%s], passwd [%s], db [%s]", host, name, passwd, db);
    if (mysql_real_connect(*(CTX.db.g_con), host, name, passwd, 
                   db, 0, NULL, 0) == NULL) 
    {
        CLD_TRACE("Error is [%s]", mysql_error(*(CTX.db.g_con)));
        cld_report_error ("Error in logging in to database: Connecting to host [%s], user [%s], passwd [...], db [%s], error [%s]", host, name, db, mysql_error(*(CTX.db.g_con)));
        return NULL;
    }    

    memset(host, 0, CLD_SECURITY_FIELD_LEN);
    memset(name, 0, CLD_SECURITY_FIELD_LEN);
    memset(passwd, 0, CLD_SECURITY_FIELD_LEN);
    memset(db, 0, CLD_SECURITY_FIELD_LEN);

    //
    // These are the most common settings. ANSI_QUOTES means that single quotes
    // are used for string and defense agains SQL injection depends on this (could be done
    // either way or for both, but the odds are ansi_quotes is used anyway). UTF8 is used for
    // web communication.
    // So in short, do NOT change either one of these settings!
    //
    if (mysql_query(*(CTX.db.g_con), "set names utf8")) 
    {
        cld_report_error ("Cannot set names to utf8");
    }

    if (mysql_query(*(CTX.db.g_con), "set session sql_mode=ansi_quotes")) 
    {
        cld_report_error ("Cannot set sql_mode to ansi_quotes");
    }
    return *(CTX.db.g_con);
}
             
// 
// Begin transaction. 
//
void cld_begin_transaction()
{
    CLD_TRACE("");

    unsigned int er;
    int rows;
    const char *errm="";

    if (cld_execute_SQL ("start transaction", &rows, &er, &errm) != 1)
    {
        cld_report_error ("Cannot start transaction, error number [%d], error [%s]", er, errm);
    }
    *(CTX.db.is_begin_transaction) = 1;
}

// 
// Check if transaction is opened. If so, and if check_mode is 1, return 1.
// If transaction opened, and if check_mode is 2, rollback transaction and return 0.
// If transaction opened, and check_mode is 0, error out. 
// If transaction is not opened, return 0.
// This is useful to determine if transaction is open, and to either commit or
// rollback (or implicitly rollback).
//
int cld_check_transaction(int check_mode)
{
    CLD_TRACE("");
    if (*(CTX.db.is_begin_transaction) == 1) 
    {
        if (check_mode==1) return 1;
        if (check_mode==2)
        {
            cld_rollback();
            return 0;
        }
        cld_report_error ("Started transaction, but was never committed or rollbacked");
    }
    else return 0;
}

// 
// Commit transaction. 
// Returns status of mysql_commit.
//
int cld_commit()
{
    CLD_TRACE("");
    const char *fname=cld_get_config ()->app.db;

    *(CTX.db.is_begin_transaction) = 0;

    return mysql_commit (cld_get_db_connection (fname));
}

// 
// Rollbacks the transaction. 
// Returns status of mysql_rollback.
//
int cld_rollback()
{
    CLD_TRACE("");
    const char *fname=cld_get_config ()->app.db;

    *(CTX.db.is_begin_transaction) = 0;

    return mysql_rollback (cld_get_db_connection (fname));
}


// 
// After an insert into auto_increment column (i.e. a table with such column),
// get the value of this column (which is 'val', the buffer of size 'sizeVal').
// Errors out if buffer is too small.
//
void cld_get_insert_id(char *val, int sizeVal)
{
    CLD_TRACE("");
    const char *fname=cld_get_config ()->app.db;
    assert (val);


    long long id = mysql_insert_id (cld_get_db_connection (fname));
    int sz = snprintf (val, sizeVal - 1, "%lld", id);
    if (sz >= sizeVal - 1)
    {
        cld_report_error("Buffer too small for last id [%d]", sz);
        return;
    }
    return;

}

// 
// Execute SQL. 
// 's' is the SQL, 'rows' is the output number of rows returned.
// 'er' is the error number (if there was an error), and err_messsage is allocated and filled with error message, if there was an error.
// Returns  0 if there is an error, 1 if it is okay.
//
int cld_execute_SQL (const char *s,  int *rows, unsigned int *er, const char **err_message)
{
    CLD_TRACE("");
    const char *fname=cld_get_config ()->app.db;
    assert (s);
    assert (rows);
    assert (er);
    
    CLD_TRACE ("Query executing: [%s]", s);

    *er = 0;


    if (mysql_query(cld_get_db_connection (fname), s)) 
    {
        //
        // Error in execution. If we're not in a transaction, try to reconnect ONCE.
        // If we ARE in a transaction, we CANNOT reconnect.
        //
        if (*(CTX.db.is_begin_transaction) == 0)
        {
            //
            // While handling error, if error is unrecoverable, cld_handle_error
            // will stop the program. If error can be reported back to the application,
            // then  we will set rows to 0 and return 0.
            //
            // Since we're NOT in transaction here, try to reconnect ONCE if the error is 'lost
            // connection' or such. The 5th parameter of 1 means that only in case of such errors,
            // try to reconnect. If the error is not lost connection, then we will report it
            // back by setting rows to 0 and returning 0.
            //
            if (cld_handle_error (s, cld_get_db_connection (fname), er, err_message, 1) == 0)
            {
                //
                // This means there was an error which is not 'lost connection'. Return to application
                //
                *rows = 0;
                return 0;
            }
            else
            {
                //
                // This means connection was lost and was successfully reestablished. Try executing
                // statement again, but get NEW connection first. Remember, we were NOT in a transaction
                // to begin with, so we can actually do retry here.
                //
                if (mysql_query(cld_get_db_connection (fname), s)) 
                {
                    //
                    // There was still an error, and this time we will NOT try reconnecting. Handle error -
                    // if connection lost, exit. If other error, report to application.
                    // We don't check return value, because IF cld_handle_error returns, it has to be 0.
                    //
                    cld_handle_error (s, cld_get_db_connection (fname), er, err_message, 0);
                    *rows = 0;
                    return 0;
                }
                else
                {
                    // 
                    // SQL statement executed correctly after reconnecting. Just proceed
                    //
                    CLD_TRACE("SQL statement executed OKAY after reconnecting to database.");
                }
            }
        }
        else
        {
            //
            // This is if we're in a transaction. Do NOT try to reconnect - if we lost connection, EXIT.
            // Otherwise, report error back to the application.
            // We don't check return value, because IF cld_handle_error returns, it has to be 0.
            //
            cld_handle_error (s, cld_get_db_connection (fname), er, err_message, 0);
            *rows = 0;
            return 0;
        }
    }

    //
    // If we're here, the statement executed OKAY, without any reconnection OR with at most one reconnection.
    //
    *rows = (int) mysql_affected_rows (cld_get_db_connection (fname));
    // for SELECT, this may be -1 - it's incorrect until mysql_store_result is called or all
    // date retrieved with mysql_use_result!!!
    CLD_TRACE("Query OK, affected rows [%d] - incorrect for SELECT, see further for that.", *rows);

    return 1;
}


//
// Handle error of execution of SQL. 's' is the statement. 'con' is the db connection.
// 'er' is the output error, and its text is in output variable err_message.
// If 'retry' is 1 AND this is a lost connection, try to reconnect ONCE. If 'retry' is 1 AND this
// is something other than lost connection, then handle it by returning 0. If 'retry' is 0, NEVER try to reconnect regardless.
// If this is lost connection and reconnection failed (or was never tried because retry is 0), then stop program.
// Returns 1 if reconnect was successful (meaning retry had to be 1 for this to happen), 0 in any other case.
//
int cld_handle_error (const char *s, MYSQL *con, unsigned int *er, const char **err_message, int retry)
{
    CLD_TRACE("");
    // This static is fine - it is used only within a single request, i.e. it doesnt span multiple request.
    // Errm is set HERE and used right after it - it doesn't go beyond a request.
    static char errm[8192];
    assert (s != NULL);
    assert (con != NULL);
    assert (er != NULL);

    CLD_TRACE ("Error in %s: %s error %d state %s", s, mysql_error(con), mysql_errno(con), 
        mysql_sqlstate(con));

    int local_error = mysql_errno (con);

    // get location in source code (if set, CLD automatically does this)
    char *sname = "";
    int lnum = 0;
    cld_location (&sname, &lnum, 0);

    switch (local_error)
    {
        case ER_DUP_ENTRY:
        case ER_DUP_ENTRY_WITH_KEY_NAME:
            /* this can happen in insert with duplicate key or update that leads to 
             * duplicate key */
            CLD_TRACE ("Duplicate key detected, no changes made");
            *er = ER_DUP_ENTRY;
            break;
         case CR_SERVER_GONE_ERROR:
         case CR_SERVER_LOST:
            // Go away if db connection lost, or try to reconnect if retry is 1

            //
            // If we're instructed to retry, we will
            //
            if (retry == 1) 
            {
                //
                // Retry connection
                //
                //
                // First we must reset connection we have already. It's lost so we're not calling mysql_close().
                // Without this, an attempt to retry connection would not work, and would just return the old cached bad
                // connection
                //
                *(CTX.db.g_con) = NULL;
                //
                // Retry it
                //
                if (cld_get_db_connection (cld_get_config()->app.db) == NULL)
                {
                    //
                    // Here we wanted to retry but it failed - stop program
                    //
                    cld_report_error ("Connection to database server is lost (after a retry), found [%s], line [%d], file [%s]", s, lnum,sname);
                }
                else
                {
                    //
                    // Retry was instructed and successful
                    //
                    CLD_TRACE("Reconnecting to database OKAY");
                    return 1;
                }
            }
            else
            {
                //
                // we don't want to retry connection, fail it
                //
                cld_report_error ("Connection to database server is lost (without retry), found [%s], line [%d], file [%s]", s, lnum,sname);
            }
         default:
            // Probably a fatal error, application must handle
            *er = local_error;
            snprintf(errm,sizeof(errm)-1,"Error during query [%s], file [%s], line [%d] : [%d]%s", s, sname, lnum, *er ,*er == ER_PARSE_ERROR ?
                "Problem with parsing SQL statement" : mysql_error(con));
            CLD_TRACE("%s", errm);
            if (err_message!=NULL) *err_message=errm;
            break;
    }
    return 0;
}

//
// Set location of SQL statement. Before it's executed, we set file name (fname),
// line number (lnum) and 'set' is 1. If there was an error, we can use this information
// to point back to the exact line where problem happened.
//
void cld_location (char **fname, int *lnum, int set)
{
    CLD_TRACE("");
    // this static variables are fine, they are used only within a single request. 
    // Before SQL is executed, cld_location is called, and if there is an error, we would
    // return that value - meaning these values are ALWAYS set in the process and THEN used
    static char *fname_loc = "";
    static int lnum_loc = 0;

    if (set == 1)
    {
        fname_loc = *fname;
        lnum_loc = *lnum;
    }
    else
    {
        *fname = fname_loc;
        *lnum = lnum_loc;
    }
} 

// 
// Select SQL. 's' is the text of the SQL and it must start with 'select'. 
//
// Outputs are 'nrow' (the number of rows in the result),
// 'ncol' (the number of columns in the result), 'col_names' is a pointer to an array (allocated here) that contains all
// column names with CLD_MAX_QUERY_OUTPUTS being the max size of the array, 'data' (if not NULL) is 
// the buffer allocated by the function that contains all data from the select. 
//
// 'data' can be NULL, but col_name cannot be. 
// Each column is present (NULL is the same as empty, or "", we do NOT make a distinction).
// 'data' is an array of pointers, whereby each array entry points to a single column. For example, if a 
// query SELECT X,Y FROM T; selects 3 rows, then data[0]={pointer to  X in 1st row}, data[1]={pointer to
// Y in 1st row}, data[2]={pointer to X in 2nd row}, data[3]={pointer to Y in 2nd row}, data[4]={pointer to
// X in 3rd row}, data[5]={pointer to Y in 3rd row}.
//
void cld_select_table (const char *s,
                  int *nrow, 
                  int *ncol, 
                  char ***col_names,
                  char ***data)
                  
{
    CLD_TRACE("");
    assert (nrow);
    assert (ncol);
    assert (s);
    const char *fname=cld_get_config ()->app.db;


    char *sname = "";
    int lnum = 0;
    // get location in end-user source code where this is called from, for error reporting purposes
    cld_location (&sname, &lnum, 0);

    if (col_names==NULL)
    {
        cld_report_error ("col_names must be non-NULL (programming error), found [%s], line [%d], file [%s]", s, lnum,sname);
    }


    // check this is SELECT and nothing else
    if (strncasecmp (s, "select", 6))
    {
        cld_report_error ("Invalid query (unrecognized operation), found [%s], line [%d], file [%s]", s, lnum,sname);
    }



    const char *errm="";
    unsigned int er = 0;

    // execute SELECT, reconnect if necessary
    int rows;
    if (!cld_execute_SQL (s, &rows, &er, &errm))
    {
        cld_report_error ("Cannot perform select, error [%d], error summary: [%s], line [%d], file [%s]", er, errm, lnum,sname);
    }
                
    MYSQL_RES *result = NULL;


    if (data != NULL)
    {
        // get all result data
        result = mysql_store_result(cld_get_db_connection (fname));
    }
    else
    {
        // no need to fetch the result yet, since we're getting column names only
        result = mysql_use_result(cld_get_db_connection (fname));
    }
                    
    if (result == NULL) 
    {
        cld_report_error ("Error storing obtained data, error %s, line [%d], file [%s]", mysql_error(cld_get_db_connection (fname)), lnum, sname);
    }

    // get number of columns
    int num_fields = mysql_num_fields(result);

    MYSQL_ROW row;

    // number of columns is needed whether we get column names only or result data
    *ncol = num_fields;

    // output columns
    MYSQL_FIELD *field;

    // 
    // allocate column names too
    //
    *col_names = (char**)cld_calloc (num_fields, sizeof(char*));

    //
    // Get column names to be either used when requested
    //
    int field_index = 0;
    while ((field = mysql_fetch_field(result)))
    {
        (*col_names)[field_index] = cld_strdup(field->name);
        field_index++;
    }
    if (data == NULL)
    {
        // clean the result and return in case we want column names ONLY
        mysql_free_result(result);
        return;
    }
                                     
    // 
    // this is getting actual data
    //
    *nrow = 0;

// get this many rows to start with, increment by this afterwards
#define CLD_INITIAL_QUERY_BATCH 200

    int query_batch = CLD_INITIAL_QUERY_BATCH;

    // allocate the array where data is
    *data = cld_calloc(query_batch*num_fields, sizeof(char**));

    int i;

    // fetch all rows, one by one (result is already here in memory)
    while ((row = mysql_fetch_row(result))) 
    { 

        // get lengths of each column
        unsigned long *lens = mysql_fetch_lengths (result);

        // check if buffer is full, if so, allocate another chunk for as long as we need it
        if (*nrow>=query_batch)
        {
            query_batch+=CLD_INITIAL_QUERY_BATCH;
            *data = cld_realloc(*data, query_batch*num_fields*sizeof(char**));
        }

        for(i = 0; i < num_fields; i++) 
        { 
            // calculate position in data
            int cpos = *nrow * num_fields + i;

            // allocate memory for each column. We don't use row directly even though it'd be a bit 
            // faster because memory allocation isn't cld_* - 
            // we would have to free the result (mysql_free_result()) at the end, which would be more complex
            // than automatically cleaning up cld_* allocated memory. mysql_free_result() implementation could change
            // and we don't know exactly what's in it. However, we could 'register' result variable as mysql and
            // free it at the end. This is TODO for a speedup in obtaining results.
            //
            (*data)[cpos] = cld_malloc (lens[i]+1);

            // copy data
            memcpy ((*data)[cpos], row[i] ? row[i] : "", lens[i]);
            (*data)[cpos][lens[i]] = 0; // end with zero in any case, even if binary
                                        // wont' hurt
        } 
        (*nrow)++;
    }
    CLD_TRACE("SELECT retrieved [%d] rows", *nrow);
    mysql_free_result(result);

}




//
// Initialize iterator for mysql result buffer obtained from cld_select_table()
// Output: 'd' is iterator that will be used in other fuctions, it will contain all query results.
// 'data' is the result buffer from cld_select_table().
// 'rows' is the number of rows from cld_select_table().
// 'cols' is the number of cols from cld_select_table().
// Note: this initializes iterator to its beginning
//
void cld_data_iterator_init (cld_iter *d, char **data, int rows, int cols)
{
    CLD_TRACE("");
    assert (d);
    assert (data);
    assert (rows);
    assert (cols);

    // initialize iterator
    d->md = data;
    d->rows = 0;
    d->cols = 0;
    d->tot_rows = rows;
    d->tot_cols = cols;
    d->tot_item = rows * cols;
}

//
// Get next piece of data from data iterator that contains query results.
//
// 'd' is the iterator prepared with cld_data_iterator_init()
// output: 'brk' is 1 if this is the end of the row, 0 otherwise (meaning 1 if this is the last column in the row)
// Returns the data for the current column or NULL if no more data
// 
char *cld_data_iterator_next (cld_iter *d, int *brk)
{
    CLD_TRACE("");
    assert (d);
    assert (brk);

    // reached the end
    if (d->tot_item == 0) 
    {
        *brk = 1; 
            // also the end of row
        return NULL;
    }

    char *return_val = d->md[d->rows*d->tot_cols + d->cols];

    // decrease  number of items outstanding and the current column
    d->tot_item --;
    d->cols++;

    // if no more column in this row, decrease the number of
    // rows outstanding, reset the number of columns outstanding,
    // and set the brk flag since it's the end of the row
    if (d->cols == d->tot_cols)
    {
        d->rows++;
        d->cols = 0;
        *brk = 1;
    }
    else
    {
        *brk = 0; 
            // not the end of the row
    }
    return return_val;
}


// 
// Fill array of arrays to present all query results to the application.
// The output is 'arr', which is a pointer that is allocated here. For example,
// arr[2][5] would be 6th column of the 3rd row (since it's 0-based indexing).
// 'data' is the result from cld_select_table().
// 'nrow' is the number of rows from cld_select_table().
// 'ncol' is the number of columns from cld_select_table().
//
void cld_data_iterator_fill_array (char **data, int nrow, int ncol, char ****arr)
{
    CLD_TRACE("");
    assert (data);
    assert (*arr == NULL);
    assert (nrow);
    assert (ncol);

    cld_iter it;
    char *d;
    int br;

    // allocate output result.
    *arr = (char***)cld_calloc (nrow, sizeof (char***));
    if (*arr == NULL)
    {
        cld_report_error ("Out of memory in result set array of size [%d]",
            nrow * (int)sizeof(char***));
    }

    // allocate each row (meaning a set of columns)
    int i;
    for (i = 0; i < nrow; i++)
    {
        (*arr)[i] = (char**)cld_malloc (sizeof (char**) * ncol);
        if ((*arr)[i] == NULL)
        {
            cld_report_error ("Out of memory in result set array of size [%d]",
                ncol * (int)sizeof(char**));
        }
    }

    int crow = 0;
    int ccol = 0;

    // use data iterator to go through all the data and to store pointers to results
    // to 'arr'
    cld_data_iterator_init (&it, data, nrow, ncol);
    while ((d = cld_data_iterator_next(&it, &br)))
    {
        (*arr)[crow][ccol] = d;
        ccol++;
        if (br) 
        {
            crow++;
            ccol = 0;
        }
    }
}

//
// Once DML operation executed (via cld_execute_SQL()), create output in form of 'arr'
// which is the same array of arrays as in cld_data_iterator_fill_array() produced for 
// any SELECT statement.
// dml result row has 3 columns: rowcount,error and insert_id, and ONLY one row
// 'arr' is the output array of arrays of the query result. 'rowcount' is the number of
// rows obtained from cld_execute_SQL(). 'err' is the error obtained from cld_execute_SQL().
// 'insert_id' is obtained from cld_get_insert_id(). The code that uses this is generated
// automatically by CLD so no manual coding is needed.
//
void cld_get_dml_row (char ****arr, int rowcount, unsigned int err, const char *insert_id)
{
    CLD_TRACE("");
    assert (*arr == NULL);
    int ncol = 3;
    int nrow = 1;

    // get result array of arrays
    *arr = (char***)cld_calloc (nrow, sizeof (char***));
    if (*arr == NULL)
    {
        cld_report_error ("Out of memory in DML result set array of size [%d]",
            nrow * (int)sizeof(char***));
    }

    // allocate the sole row
    (*arr)[0] = (char**)cld_malloc (sizeof (char**) * ncol);
    if ((*arr)[0] == NULL)
    {
       cld_report_error ("Out of memory in DML result set array of size [%d]",
           ncol * (int)sizeof(char**));
    }


    // allocate three columns
    int dml_size = 20;
    (*arr)[0][0] = (char*)cld_malloc (dml_size + 1);
    (*arr)[0][1] = (char*)cld_malloc (dml_size + 1);
    (*arr)[0][2] = (char*)cld_malloc (dml_size + 1);

    // set output data for DML
    snprintf ((*arr)[0][0], dml_size, "%d", rowcount);
    snprintf ((*arr)[0][1], dml_size, "%u", err);
    snprintf ((*arr)[0][2], dml_size, "%s", insert_id);

}

// 
// Produce an empty row for a query that we know has 'ncol' columns
// Output: 'arr' is the same result as in cld_select_table(), an array of arrays for the query results.
//
void cld_get_empty_row (char ****arr, int ncol)
{
    CLD_TRACE("");
    assert (*arr == NULL);
    assert (ncol);

    // allocate one row
    *arr = (char***)cld_calloc (1, sizeof (char***));
    if (*arr == NULL)
    {
        cld_report_error ("Out of memory in empty result set array of size [%d]",
            1 * (int)sizeof(char***));
    }

    // allocate 'ncol' columns
    (*arr)[0] = (char**)cld_malloc (sizeof (char**) * ncol);
    if ((*arr)[0] == NULL)
    {
       cld_report_error ("Out of memory in empty result set array of size [%d]",
           ncol * (int)sizeof(char**));
    }

    int ccol;

    // fill in columns with empty data
    for (ccol = 0; ccol < ncol; ccol++)
    {
        (*arr)[0][ccol] = cld_init_string("");
    }
}


// 
// Get login credentials to login to database.
// Outputs: 'host' is the host name of database (localhost for the same host),
// 'name' is the name of db user, 'passwd' is the password of this user,
// 'db' is the name of the database.
// 'fname' is the name of the file (named ".db").
// Returns -1 if cannot open file, -2 if permissions aren't correct (must be 600),
// 0 if okay.
//
int cld_get_credentials(char* host, 
                    char* name, 
                    char* passwd, 
                    char* db,
                    const char *fname)
{
  CLD_TRACE ("");
  struct stat sb;

  // stat the file
  if (stat(fname, &sb) != 0) return -1;

  // 33152 = 100600 in octal => -rw------- for owner and group only
  if (sb.st_mode != 33152) {
      CLD_TRACE("Ownership of sec file is [%d]", sb.st_mode);
      return -2;
  }
  else
  {
     FILE *login_file = fopen(fname, "r");
     if (NULL == login_file)
     {
        return -1;
     }
     // Get login information from .db file. This is outside web document root.
     // These values are protected by 0600 protection and are read, used with the database
     // and then cleared out of memory so even by accident inaccesible. 
     //
     char *res = fgets(host, CLD_SECURITY_FIELD_LEN - 1, login_file);
     if (res == NULL) return 0;
     res = fgets(name, CLD_SECURITY_FIELD_LEN - 1, login_file);
     if (res == NULL) return 0;
     res = fgets(passwd, CLD_SECURITY_FIELD_LEN - 1, login_file);
     if (res == NULL) return 0;
     res = fgets(db, CLD_SECURITY_FIELD_LEN - 1, login_file);
     if (res == NULL) return 0;
     host[strcspn (host, "\n")] = '\0';
     name[strcspn (name, "\n")] = '\0';
     passwd[strcspn (passwd, "\n")] = '\0';
     db[strcspn (db, "\n")] = '\0';

     // trim values
     int l = strlen (host);
     cld_trim (host, &l);
     l = strlen (name);
     cld_trim (name, &l);
     l = strlen (passwd);
     cld_trim (passwd, &l);
     l = strlen (db);
     cld_trim (db, &l);
     fclose(login_file);
  }
  return 0;
}








