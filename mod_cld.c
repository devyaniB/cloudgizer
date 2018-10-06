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
// Bridge functions between Cloudgizer and Apache.
//

#include "httpd.h"
#include "http_config.h"
#include "http_core.h"
#include "http_log.h"
#include "http_main.h"
#include "http_protocol.h"
#include "http_request.h"
#include "util_script.h"
#include "http_connection.h"
#include "apr_strings.h"
#include <assert.h>


//
// Function prototypes
//
int cld_ws_util_read (void * rp, char *content, int len);
const char *cld_ws_get_env(void * vmr, const char *n);
void cld_ws_set_content_type(void *rp, const char *v);
void cld_ws_set_header (void *rp, const char *n, const char *v);
void cld_ws_add_header (void *rp, const char *n, const char *v);
int cld_ws_write (void *r, const char *s, int nbyte);
int cld_ws_flush (void *r);
void cld_ws_finish (void *rp);
int cld_main (void *r);
void cld_ws_set_status (void *rp, int st, const char *line);
int cld_ws_printf (void *r, const char *fmt, ...) __attribute__ ((format (printf, 2, 3)));
void cld_ws_set_content_length(void *rp, const char *v);
const char *cld_ws_get_status (void *rp, int *status);

// 
// Get status of apache reply to the client (before it's sent)
// rp is apache request, 'status' is the status number and
// returns the string of status (description)
//
const char *cld_ws_get_status (void *rp, int *status)
{
  assert (status != NULL);
  request_rec *r = (request_rec*)rp;
  *status = r->status;
  return r->status_line;
}


// 
// Set status to be sent to the client. rp is apache request, st
// is the status number and 'line' is the status text
//
void cld_ws_set_status (void *rp, int st, const char *line)
{
  request_rec *r = (request_rec*)rp;
  r->status = st;
  r->status_line = line;
}


// 
// Add header to apache reply. rp is apache request, 'n' is the name of header item,
// 'v' is the value. This ADDS an item, this is for setting parts of header where multiples can exist (say cookies)
// Apache WILL make a copy of n/v
//
void cld_ws_add_header (void *rp, const char *n, const char *v)
{
  request_rec *r = (request_rec*)rp;
  apr_table_add(r->headers_out, n, v);
}

// 
// Set header name ('n') and value ('v"0 in apache reply. rp is apache request.
// This is for setting parts of header where ONLY one should exist (say content-type)
// Apache WILL make a copy of n/v
//
void cld_ws_set_header (void *rp, const char *n, const char *v)
{
  request_rec *r = (request_rec*)rp;
  apr_table_set(r->headers_out, n, v);
}

// 
// Set content type for the reply. rp is apache request, 'v' is the content
// type (such as text/html).
//
void cld_ws_set_content_type(void *rp, const char *v)
{
  request_rec *r = (request_rec*)rp;
  ap_set_content_type (r, v);
}

// 
// Flush data that's been written to the client so far. rp is apache request.
//
int cld_ws_flush (void *rp)
{
  request_rec *r = (request_rec*)rp;
  return ap_rflush (r);
}

// 
// Finish request so web server knows to tell client the response is done
// (especially important for chunked response)
//
void cld_ws_finish (void *rp)
{
  request_rec *r = (request_rec*)rp;
  ap_finalize_sub_req_protocol (r);
}

// 
// Set content length (such as when sending images etc.). rp is apache request.
// 'v' is the value, a number as a string
//
void cld_ws_set_content_length(void *rp, const char *v)
{
  request_rec *r = (request_rec*)rp;
  ap_set_content_length (r, (apr_off_t)atol(v));
}

// 
// Write data to the client. Fast as there is no formatting.
// r is apache request, 's' is the data, 'nbyte' is byte length of the data.
// Returns number of bytes written. This number must be nbyte, or otherwise
// the transmission to the client didn't happen and it's an error.
int cld_ws_write (void *r, const char *s, int nbyte)
{
    // one large synchronous write to the web, so it's better to buffer as we do 
    return ap_rwrite (s, nbyte, (request_rec*)r);
}

// 
// Output to the client in a printf-like format. r is apache request.
// fmt and ... are the same as for printf.
// Returns number of bytes written.
//
int cld_ws_printf (void *r, const char *fmt, ...) 
{
    if (fmt[0] == 0) return 0;
    va_list vl;
    va_start (vl, fmt);
    int res = ap_vrprintf ((request_rec*)r, fmt, vl);
    va_end (vl);
    return res;
}


#define FIXNULL(s) ((s)==NULL?"":(s))

// 
// Get environment for apache. This attempts to obtain many useful apache 
// environment variables
// vmr is apache request, n is the name of variable.
// Returns value of environment (apache) variable 'n'.
//
const char *cld_ws_get_env(void * vmr, const char *n)
{

    const apr_array_header_t *fields;
    apr_table_entry_t *e = 0;

    request_rec *r = (request_rec*)vmr;
    int i;

    // two most commonly used ones are checked first
    if (!strcmp (n, "REQUEST_METHOD")) return FIXNULL((char*)(r->method));
    if (!strcmp (n, "QUERY_STRING")) return FIXNULL((char*)(r->args));

    int if_none_match = 0;
    int cookie = 0;
    int ctype = 0;
    int clen = 0;
    int usag = 0;
    int ref = 0;
    int https = 0;
    int protocol = 0;
    int soft = 0;
    int name = 0;
    int rip = 0;
    int rport = 0;
    int port = 0;
    int admin = 0;
    int ruser = 0;
    int root = 0;
    int ip = 0;

    // 
    // here is the list of variables we obtain (plus REQUEST_METHOD and QUERY_STRING we get above)
    //
    int isvalid = (if_none_match = (!strcmp (n, "HTTP_IF_NONE_MATCH")) ||
    (cookie = !strcmp (n, "HTTP_COOKIE")) ||
    (ctype = !strcmp (n, "CONTENT_TYPE")) ||
    (clen = !strcmp (n, "CONTENT_LENGTH")) ||
    (usag = !strcmp (n, "HTTP_USER_AGENT")) ||
    (ref = !strcmp (n, "HTTP_REFERER")) ||
    (https = !strcmp (n, "HTTPS")) ||
    (soft = !strcmp (n, "SERVER_SOFTWARE")) ||
    (name = !strcmp (n, "SERVER_NAME")) ||
    (rip = !strcmp (n, "REMOTE_ADDR")) ||
    (rport = !strcmp (n, "REMOTE_PORT")) ||
    (port = !strcmp (n, "SERVER_PORT")) ||
    (admin = !strcmp (n, "SERVER_ADMIN")) ||
    (protocol = !strcmp (n, "SERVER_PROTOCOL")) ||
    (ruser = !strcmp (n, "REMOTE_USER")) ||
    (root = !strcmp (n, "DOCUMENT_ROOT")) ||
    (ip = !strcmp (n, "SERVER_ADDR"))
    );

    // if not one of the above, it's empty
    if (isvalid == 0) return "";

    // REMOTE_ADDR: CLD is for 2.4 and above only, this is from older versions 
    if (rip == 1)
    {
#if APACHE_VERSION<204
      return FIXNULL(r->connection->remote_ip);
#else
      return FIXNULL(r->connection->client_ip);
#endif
    }

    // same for REMOTE_PORT, we do 2.4 and above only
    if (rport == 1)
    {
#if APACHE_VERSION<204
      return FIXNULL(apr_psprintf (r->pool, "%d", r->connection->remote_addr->port));
#else
      return FIXNULL(apr_psprintf (r->pool, "%d", r->connection->client_addr->port));
#endif
    }

    // DOCUMENT_ROOT
    if (root == 1)
    {
      return FIXNULL(ap_document_root(r));
    }

    // REMOTE_USER
    if (ruser == 1)
    {
      return FIXNULL(r->user);
    }

    // SERVER_ADDR
    if (ip == 1)
    {
      return FIXNULL(r->connection->local_ip);
    }

    // SERVER_PORT
    if (port == 1)
    {
      return FIXNULL(apr_psprintf (r->pool, "%u", ap_get_server_port (r)));
    }

    // SERVER_ADMIN
    if (admin == 1)
    {
      return FIXNULL(r->server->server_admin);
    }

    // SERVER_NAME, 2.4 and above only
    if (name == 1)
    {
#if APACHE_VERSION<204
      return FIXNULL(ap_get_server_name(r));
#else
      return FIXNULL(ap_get_server_name_for_url(r));
#endif
    }

    // SERVER_PROTOCOL
    if (protocol == 1)
    {
      return FIXNULL(r->protocol);
    }

    // SERVER_SOFTWARE, apache 2.4 and above only
    if (soft == 1)
    {
#if APACHE_VERSION<204
      return FIXNULL(ap_get_server_version());
#else
      return FIXNULL(ap_get_server_description());
#endif
    }

    // get other environment and header information
    if (https == 1)
    {
        fields = apr_table_elts (r->subprocess_env);
    }
    else
    {
        fields = apr_table_elts (r->headers_in);
    }

    e = (apr_table_entry_t *) fields->elts;
    for (i = 0; i < fields->nelts; i++)
    {
      if (e[i].key == NULL) continue;
      if (!strcasecmp (e[i].key, "If-None-Match"))
      {
          if (if_none_match == 1) return FIXNULL(e[i].val);
      }
      else if (!strcasecmp (e[i].key, "Https"))
      {
          if (https == 1) return FIXNULL(e[i].val);
      }
      else if (!strcasecmp (e[i].key, "Cookie"))
      {
          if (cookie == 1) return FIXNULL(e[i].val);
      }
      else if (!strcasecmp (e[i].key, "Content-Type"))
      {
          if (ctype == 1) return FIXNULL(e[i].val);
      }
      else if (!strcasecmp (e[i].key, "Content-Length"))
      {
          if (clen == 1) return FIXNULL(e[i].val);
      }
      else if (!strcasecmp (e[i].key, "User-Agent"))
      {
          if (usag == 1) return FIXNULL(e[i].val);
      }
      else if (!strcasecmp (e[i].key, "Referer"))
      {
          if (ref == 1) return FIXNULL(e[i].val);
      }
    }
    return "";
}

// 
// Read POSTed content from the client. rp is apache request,
// 'content' is the buffer and the expected length of content is 'len'.
// 'content' must be allocated to at least 'len'+1 bytes.
// Returns 0 if error, 1 if successful.
//
int cld_ws_util_read (void * rp, char *content, int len)
{
  request_rec * r = (request_rec*)rp;
  int rc;
  if ((rc = ap_setup_client_block (r, REQUEST_CHUNKED_ERROR)) != OK)
  {
      return 0;
  }

  if (ap_should_client_block (r))
  {
      int len_read = 0;
      int curr_read_pos=0;
      int length = (int)r->remaining;

      // make sure there is enough space
      // we obtain the content length prior to coming here so
      // the len is correct and should never be smaller than length
      assert (length <= len);

      // we read data directly into our buffer, avoiding a massive memcpy
      while ((len_read =
	      ap_get_client_block (r, content+curr_read_pos, length-curr_read_pos)) > 0)
      {
          curr_read_pos += len_read;
      }

  } else return 0;
  return 1;
}





