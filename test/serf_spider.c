/* ====================================================================
 * The Apache Software License, Version 1.1
 *
 * Copyright (c) 2002 The Apache Software Foundation.  All rights
 * reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 *
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in
 *    the documentation and/or other materials provided with the
 *    distribution.
 *
 * 3. The end-user documentation included with the redistribution,
 *    if any, must include the following acknowledgment:
 *       "This product includes software developed by the
 *        Apache Software Foundation (http://www.apache.org/)."
 *    Alternately, this acknowledgment may appear in the software itself,
 *    if and wherever such third-party acknowledgments normally appear.
 *
 * 4. The names "Apache" and "Apache Software Foundation" must
 *    not be used to endorse or promote products derived from this
 *    software without prior written permission. For written
 *    permission, please contact apache@apache.org.
 *
 * 5. Products derived from this software may not be called "Apache",
 *    nor may "Apache" appear in their name, without prior written
 *    permission of the Apache Software Foundation.
 *
 * THIS SOFTWARE IS PROVIDED ``AS IS'' AND ANY EXPRESSED OR IMPLIED
 * WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
 * OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED.  IN NO EVENT SHALL THE APACHE SOFTWARE FOUNDATION OR
 * ITS CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF
 * USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
 * ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
 * OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT
 * OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 * ====================================================================
 *
 */
#include <apr.h>
#include <apr_queue.h>
#include <apr_strmatch.h>

#include "serf.h"
#include "serf_filters.h"
#include "serf_version.h"

#define CRLF "\r\n"

/* Yes, it'd be nice if these were command-line options... */
/* Define this to 1 to print out header information. */
#define SERF_GET_DEBUG 0
/* httpd-2.0 is cute WRT chunking and will only do it on a keep-alive.
 * Define this to 1 to test serf's ability to handle chunking.
 */
#define SERF_GET_TEST_CHUNKING 0 

static apr_pool_t *spider_pool;
static apr_queue_t *spider_queue;
static apr_table_t *spider_table;
static apr_strmatch_pattern *link_pattern;
static apr_strmatch_pattern *end_pattern;

static apr_status_t search_bucket(apr_bucket *bucket,
                                  serf_response_t *response,
                                  apr_pool_t *pool)
{
    const char *buf, *match, *new_match;
    apr_size_t length, match_length, written;
    apr_status_t status;
        
    status = apr_bucket_read(bucket, &buf, &length, APR_BLOCK_READ);

    if (status) {
        return status;
    }

    match = buf;
    match_length = length;
    new_match = apr_strmatch(link_pattern, match, match_length);
    while (new_match) {
        char *end_match;
        match = new_match + link_pattern->length;
        match_length = match - buf;
 
        end_match = apr_strmatch(end_pattern, match, match_length);
        if (end_match) {
            apr_uri_t *uri;
            char *new_uri;
            *end_match = '\0';
            uri = apr_palloc(pool, sizeof(apr_uri_t));
            apr_uri_parse(pool, match, uri);
            if (!uri->path) {
                uri->path = response->request->uri->path;
            }
            else if (*uri->path != '/') {
                uri->path = apr_pstrcat(pool,
                                        response->request->uri->path,
                                        uri->path, NULL);
            }
            if (!uri->hostinfo) {
                char *p, *q, *f;
                p = uri->path;
                q = uri->query;
                f = uri->fragment;
                uri = apr_pmemdup(pool, response->request->uri,
                                  sizeof(apr_uri_t));
                uri->path = p;
                uri->query = q;
                uri->fragment = f;
            }
            new_uri = apr_uri_unparse(spider_pool, uri, 0);
            if (!apr_table_get(spider_table, new_uri)) {
                apr_table_set(spider_table, new_uri, new_uri);
                status = apr_queue_trypush(spider_queue, new_uri);
                if (status) {
                    return status;
                }
            }

            *end_match = '"';
        } 
        new_match = apr_strmatch(link_pattern, match, length);
    }
    return APR_SUCCESS;
}

static apr_status_t print_bucket(apr_bucket *bucket, apr_file_t *file)
{
    const char *buf;
    apr_size_t length, written;
    apr_status_t status;
        
    status = apr_bucket_read(bucket, &buf, &length, APR_BLOCK_READ);

    if (status) {
        return status;
    }

    return apr_file_write_full(file, buf, length, &written);
}

static apr_status_t http_source(apr_bucket_brigade *brigade,
                                serf_request_t *request,
                                apr_pool_t *pool)
{
    apr_bucket *bucket;

    bucket = serf_bucket_request_line_create(request->method,
                                             request->uri->path,
                                             "HTTP/1.1", pool,
                                             brigade->bucket_alloc);
 
    APR_BRIGADE_INSERT_HEAD(brigade, bucket);

#if !SERF_GET_TEST_CHUNKING
    bucket = serf_bucket_header_create("Connection",
                                       "Close",
                                       pool, brigade->bucket_alloc);
    APR_BRIGADE_INSERT_TAIL(brigade, bucket);
#endif

    return APR_SUCCESS;
}

static apr_status_t host_header_filter(apr_bucket_brigade *brigade,
                                       serf_filter_t *filter,
                                       apr_pool_t *pool)
{
    apr_bucket *bucket;
    serf_request_t *request = filter->ctx;

    bucket = serf_bucket_header_create("Host",
                                       request->uri->hostname,
                                       pool, brigade->bucket_alloc);
 
    APR_BRIGADE_INSERT_TAIL(brigade, bucket);

    return APR_SUCCESS;
}

static apr_status_t user_agent_filter(apr_bucket_brigade *brigade,
                                      serf_filter_t *filter,
                                      apr_pool_t *pool)
{
    apr_bucket *bucket;

    bucket = serf_bucket_header_create("User-Agent",
                                       "Serf " SERF_VERSION_STRING,
                                       pool, brigade->bucket_alloc);
 
    APR_BRIGADE_INSERT_TAIL(brigade, bucket);

    return APR_SUCCESS;
}

static apr_status_t http_headers_filter(apr_bucket_brigade *brigade,
                                        serf_filter_t *filter,
                                        apr_pool_t *pool)
{
    apr_bucket *bucket;

    /* All we do here is stick CRLFs in the right places. */
    bucket = APR_BRIGADE_FIRST(brigade);
    while (bucket != APR_BRIGADE_SENTINEL(brigade)) {
        if (SERF_BUCKET_IS_REQUEST_LINE(bucket) ||
            SERF_BUCKET_IS_HEADER(bucket)) {
            apr_bucket *eol;

            eol = apr_bucket_immortal_create(CRLF, sizeof(CRLF)-1,
                                             brigade->bucket_alloc);

            APR_BUCKET_INSERT_AFTER(bucket, eol);
       }

        bucket = APR_BUCKET_NEXT(bucket);
    }

    /* FIXME: We need a way to indicate we are EOH. */
    bucket = apr_bucket_immortal_create(CRLF, sizeof(CRLF)-1,
                                        brigade->bucket_alloc);
    APR_BRIGADE_INSERT_TAIL(brigade, bucket);

    return APR_SUCCESS;
}

static apr_status_t debug_request(apr_bucket_brigade *brigade,
                                  serf_filter_t *filter,
                                  apr_pool_t *pool)
{
    apr_status_t status;
    apr_file_t *out_file;
    apr_bucket *bucket;

    status = apr_file_open_stdout(&out_file, pool);
    if (status) {
        return status;
    }

    for (bucket = APR_BRIGADE_FIRST(brigade);
         bucket != APR_BRIGADE_SENTINEL(brigade);
         bucket = APR_BUCKET_NEXT(bucket)) {

        status = print_bucket(bucket, out_file);
        if (status) {
            return status;
        }
    }

    return APR_SUCCESS;
}

static apr_status_t debug_response(apr_bucket_brigade *brigade,
                                   serf_filter_t *filter,
                                   apr_pool_t *pool)
{
    apr_status_t status;
    apr_file_t *out_file;
    apr_bucket *bucket;

    status = apr_file_open_stdout(&out_file, pool);
    if (status) {
        return status;
    }

    /* Print the STATUS bucket first. */ 
    for (bucket = APR_BRIGADE_FIRST(brigade);
         bucket != APR_BRIGADE_SENTINEL(brigade);
         bucket = APR_BUCKET_NEXT(bucket)) {
        if (SERF_BUCKET_IS_STATUS(bucket)) {
            status = print_bucket(bucket, out_file);
            if (status) {
                return status;
            }
            status = apr_file_putc('\n', out_file);
            if (status) {
                return status;
            }
        } 
    }

    /* Now, print all headers.  */
    for (bucket = APR_BRIGADE_FIRST(brigade);
         bucket != APR_BRIGADE_SENTINEL(brigade);
         bucket = APR_BUCKET_NEXT(bucket)) {
        if (SERF_BUCKET_IS_HEADER(bucket)) {
            status = print_bucket(bucket, out_file);
            if (status) {
                return status;
            }
            status = apr_file_putc('\n', out_file);
            if (status) {
                return status;
            }
        } 
    }

    /* Print a separator line. */
    status = apr_file_putc('\n', out_file);
    if (status) {
        return status;
    }

    return APR_SUCCESS;
}

static apr_status_t http_handler(serf_response_t *response, apr_pool_t *pool)
{
    apr_status_t status;
    apr_file_t *out_file;
    apr_bucket *bucket;
    apr_off_t length;

    status = apr_file_open_stdout(&out_file, pool);
    if (status) {
        return status;
    }

    apr_brigade_length(response->entity, 1, &length);
    apr_file_printf(out_file, "Path-Name: %s\n",
                    apr_uri_unparse(pool, response->request->uri, 0));
    apr_file_printf(out_file, "Content-Length: %ld\n\n", length);

    /* Print anything that isn't metadata. */
    for (bucket = APR_BRIGADE_FIRST(response->entity);
         bucket != APR_BRIGADE_SENTINEL(response->entity);
         bucket = APR_BUCKET_NEXT(bucket)) {
        if (!APR_BUCKET_IS_METADATA(bucket)) {
            status = search_bucket(bucket, response, pool);
            if (status) {
                return status;
            }

            status = print_bucket(bucket, out_file);
            if (status) {
                return status;
            }
        }
    }

    apr_brigade_cleanup(response->entity);

    return APR_SUCCESS;
}

int main(int argc, const char **argv)
{
    apr_status_t status;
    apr_pool_t *pool, *request_pool;
    serf_connection_t *connection;
    serf_request_t *request;
    serf_response_t *response;
    serf_filter_t *filter;
    apr_uri_t *url;
    const char *raw_url;
    int using_ssl = 0;
   
    if (argc != 2) {
        puts("Gimme a URL, stupid!");
        exit(-1);
    }
    raw_url = argv[1];

    apr_initialize();
    atexit(apr_terminate);

    apr_pool_create(&pool, NULL);
    /* serf_initialize(); */

    serf_register_filter("SOCKET_WRITE", serf_socket_write, pool);
    serf_register_filter("SOCKET_READ", serf_socket_read, pool);

#if SERF_HAS_OPENSSL
    serf_register_filter("SSL_WRITE", serf_ssl_write, pool);
    serf_register_filter("SSL_READ", serf_ssl_read, pool);
#endif

    serf_register_filter("USER_AGENT", user_agent_filter, pool);
    serf_register_filter("HOST_HEADER", host_header_filter, pool);
    serf_register_filter("HTTP_HEADERS_OUT", http_headers_filter, pool);

    serf_register_filter("HTTP_STATUS_IN", serf_http_status_read, pool);
    serf_register_filter("HTTP_HEADERS_IN", serf_http_header_read, pool);
    serf_register_filter("HTTP_DECHUNK", serf_http_dechunk, pool);

    serf_register_filter("DEFLATE_SEND_HEADER", serf_deflate_send_header, pool);
    serf_register_filter("DEFLATE_READ", serf_deflate_read, pool);

    serf_register_filter("DEBUG_REQUEST", debug_request, pool);
    serf_register_filter("DEBUG_RESPONSE", debug_response, pool);

    /*
    serf_register_filter("DEFLATE_READ", serf_deflate_read, pool);
    */

    apr_pool_create(&spider_pool, pool);

    apr_queue_create(&spider_queue, 1000, spider_pool);
    apr_queue_push(spider_queue, raw_url);

    spider_table = apr_table_make(spider_pool, 1000);

    link_pattern = apr_strmatch_precompile(pool, "<a href=\"", 0);
    end_pattern = apr_strmatch_precompile(pool, "\">", 0);

    apr_pool_create(&request_pool, pool);

    while (1) {
        void *queue_val;
        char *current_url;
        static count = 0;

        if (count++ > 20) {
            break;
        }

        status = apr_queue_trypop(spider_queue, &queue_val);
        if (APR_STATUS_IS_EAGAIN(status)) {
            break;
        }
        current_url = *(char**)queue_val;
        fprintf(stderr, "%s\n", current_url);
        url = apr_palloc(request_pool, sizeof(apr_uri_t));
        apr_uri_parse(request_pool, current_url, url);
        if (!url->port) {
            url->port = apr_uri_default_port_for_scheme(url->scheme);
        }
#if SERF_HAS_OPENSSL
        if (strcasecmp(url->scheme, "https") == 0) {
            using_ssl = 1;
        }
#endif
        status = serf_open_uri(url, &connection, &request, request_pool);

        if (status) {
            printf("Error: %d\n", status);
            exit(status);
        }

        request->source = http_source;
        request->handler = http_handler;

        request->method = "GET";
        request->uri = url;

        /* FIXME: Get serf to install an endpoint which has access to the conn. */
        if (using_ssl) {
            filter = serf_add_filter(connection->request_filters, 
                                     "SSL_WRITE", request_pool);
            filter->ctx = connection;

            filter = serf_add_filter(connection->response_filters, 
                                     "SSL_READ", request_pool);
            filter->ctx = connection;
        }
        else {
            filter = serf_add_filter(connection->request_filters, 
                                     "SOCKET_WRITE",
                                     request_pool);
            filter->ctx = connection;

            filter = serf_add_filter(connection->response_filters, 
                                     "SOCKET_READ", request_pool);
            filter->ctx = connection;
        }
#if SERF_GET_DEBUG
        filter = serf_add_filter(connection->request_filters, "DEBUG_REQUEST", request_pool);
#endif

        filter = serf_add_filter(request->request_filters, "USER_AGENT", request_pool);
        filter = serf_add_filter(request->request_filters, "HOST_HEADER", request_pool);
        filter->ctx = request;
/*
        filter = serf_add_filter(request->request_filters, "DEFLATE_SEND_HEADER",
                                 request_pool);*/
        filter = serf_add_filter(request->request_filters, "HTTP_HEADERS_OUT",
                                 request_pool);

        /* Now add the response filters. */
        filter = serf_add_filter(request->response_filters, "HTTP_STATUS_IN",
                                 request_pool);
        filter = serf_add_filter(request->response_filters, "HTTP_HEADERS_IN",
                                 request_pool);
        filter = serf_add_filter(request->response_filters, "HTTP_DECHUNK",
                                 request_pool);
        filter = serf_add_filter(request->response_filters, "DEFLATE_READ",
                                 request_pool);
#if SERF_GET_DEBUG
        filter = serf_add_filter(request->response_filters, "DEBUG_RESPONSE",
                                 pool);
#endif

        status = serf_open_connection(connection);
        if (status) {
            printf("Error: %d\n", status);
            exit(status);
        }

        status = serf_write_request(request, connection);

        if (status) {
            printf("Error: %d\n", status);
            exit(status);
        }

        status = serf_read_response(&response, connection, request_pool);

        if (status) {
            printf("Error: %d\n", status);
            exit(status);
        }

        apr_pool_clear(request_pool);
    } 
    return 0;
}
