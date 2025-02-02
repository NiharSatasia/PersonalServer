/*
 * A partial implementation of HTTP/1.0
 *
 * This code is mainly intended as a replacement for the book's 'tiny.c' server
 * It provides a *partial* implementation of HTTP/1.0 which can form a basis for
 * the assignment.
 *
 * @author G. Back for CS 3214 Spring 2018
 */
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <string.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdbool.h>
#include <errno.h>
#include <unistd.h>
#include <time.h>
#include <fcntl.h>
#include <assert.h>
#include <linux/limits.h>
#include "http.h"
#include "hexdump.h"
#include "socket.h"
#include "bufio.h"
#include "main.h"
#include <dirent.h>
#include <jansson.h>

// Need macros here because of the sizeof
#define CRLF "\r\n"
#define CR "\r"
#define STARTS_WITH(field_name, header) \
    (!strncasecmp(field_name, header, sizeof(header) - 1))

/* Parse HTTP request line, setting req_method, req_path, and req_version. */
static bool
http_parse_request(struct http_transaction *ta)
{
    size_t req_offset;
    ssize_t len = bufio_readline(ta->client->bufio, &req_offset);
    if (len < 2) // error, EOF, or less than 2 characters
        return false;

    char *request = bufio_offset2ptr(ta->client->bufio, req_offset);
    request[len - 2] = '\0'; // replace LF with 0 to ensure zero-termination
    char *endptr;
    char *method = strtok_r(request, " ", &endptr);
    if (method == NULL)
        return false;

    if (!strcmp(method, "GET"))
        ta->req_method = HTTP_GET;
    else if (!strcmp(method, "POST"))
        ta->req_method = HTTP_POST;
    else
        ta->req_method = HTTP_UNKNOWN;

    char *req_path = strtok_r(NULL, " ", &endptr);
    if (req_path == NULL)
        return false;

    ta->req_path = bufio_ptr2offset(ta->client->bufio, req_path);

    char *http_version = strtok_r(NULL, CR, &endptr);
    if (http_version == NULL) // would be HTTP 0.9
        return false;

    // record client's HTTP version in request
    if (!strcmp(http_version, "HTTP/1.1"))
        ta->req_version = HTTP_1_1;
    else if (!strcmp(http_version, "HTTP/1.0"))
        ta->req_version = HTTP_1_0;
    else
        return false;

    return true;
}

/* Process HTTP headers. */
static bool
http_process_headers(struct http_transaction *ta)
{
    for (;;)
    {
        size_t header_offset;
        ssize_t len = bufio_readline(ta->client->bufio, &header_offset);
        if (len <= 0)
            return false;

        char *header = bufio_offset2ptr(ta->client->bufio, header_offset);
        if (len == 2 && STARTS_WITH(header, CRLF)) // empty CRLF
            return true;

        header[len - 2] = '\0';
        /* Each header field consists of a name followed by a
         * colon (":") and the field value. Field names are
         * case-insensitive. The field value MAY be preceded by
         * any amount of LWS, though a single SP is preferred.
         */
        char *endptr;
        char *field_name = strtok_r(header, ":", &endptr);
        if (field_name == NULL)
            return false;

        // skip white space
        char *field_value = endptr;
        while (*field_value == ' ' || *field_value == '\t')
            field_value++;

        // you may print the header like so
        // printf("Header: %s: %s\n", field_name, field_value);
        if (!strcasecmp(field_name, "Content-Length"))
        {
            ta->req_content_len = atoi(field_value);
        }

        /* Handle other headers here. Both field_value and field_name
         * are zero-terminated strings.
         */
        // video test 4
        if (!strcasecmp(field_name, "Range"))
        {
            int start;
            int end;
            int result = sscanf(field_value, "bytes=%d-%d", &start, &end);

            // Both start and end are present
            if (result == 2)
            {
                ta->range.start = start;
                ta->range.end = end;
                ta->range.is_set = true;
            }
            // Only start is present
            else if (result == 1)
            {
                ta->range.start = start;
                ta->range.end = -1;
                ta->range.is_set = true;
            }
            else
            {
                ta->range.is_set = false;
            }
        }
        if (!strcasecmp(field_name, "Cookie"))
        {
            // need to get a token, maybe a validation time too
            char *token;
            char *ptr;
            token = strtok_r(field_value, "=; ", &ptr);
            // use null for field_value in while loop after
            while (token)
            {
                // if token name = auth_jwt_token then get value and set field of http transaction
                if (!strcmp(token, "auth_jwt_token"))
                {
                    // iterate to next value, the token
                    token = strtok_r(NULL, "=; ", &ptr);
                    // store in transaction struct as bufio2ptroffset
                    ta->token = bufio_ptr2offset(ta->client->bufio, token);
                    break;
                }
                // if not continue
                token = strtok_r(NULL, "=; ", &ptr);
            }
        }
    }
}

const int MAX_HEADER_LEN = 2048;

/* add a formatted header to the response buffer. */
void http_add_header(buffer_t *resp, char *key, char *fmt, ...)
{
    va_list ap;

    buffer_appends(resp, key);
    buffer_appends(resp, ": ");

    va_start(ap, fmt);
    char *error = buffer_ensure_capacity(resp, MAX_HEADER_LEN);
    int len = vsnprintf(error, MAX_HEADER_LEN, fmt, ap);
    resp->len += len > MAX_HEADER_LEN ? MAX_HEADER_LEN - 1 : len;
    va_end(ap);

    buffer_appends(resp, "\r\n");
}

/* add a content-length header. */
static void
add_content_length(buffer_t *res, size_t len)
{
    http_add_header(res, "Content-Length", "%ld", len);
}

/* start the response by writing the first line of the response
 * to the response buffer.  Used in send_response_header */
static void
start_response(struct http_transaction *ta, buffer_t *res)
{
    buffer_init(res, 80);

    /* Hint: you must change this as you implement HTTP/1.1.
     * Respond with the highest version the client supports
     * as indicated in the version field of the request.
     */
    // if (ta->req_version && ta->req_version == HTTP_1_1) {
    //     buffer_appends(res, "HTTP/1.1 ");
    // }
    // else {
    buffer_appends(res, "HTTP/1.0 ");
    //}

    switch (ta->resp_status)
    {
    case HTTP_OK:
        buffer_appends(res, "200 OK");
        break;
    case HTTP_PARTIAL_CONTENT:
        buffer_appends(res, "206 Partial Content");
        break;
    case HTTP_BAD_REQUEST:
        buffer_appends(res, "400 Bad Request");
        break;
    case HTTP_PERMISSION_DENIED:
        buffer_appends(res, "403 Permission Denied");
        break;
    case HTTP_NOT_FOUND:
        buffer_appends(res, "404 Not Found");
        break;
    case HTTP_METHOD_NOT_ALLOWED:
        buffer_appends(res, "405 Method Not Allowed");
        break;
    case HTTP_REQUEST_TIMEOUT:
        buffer_appends(res, "408 Request Timeout");
        break;
    case HTTP_REQUEST_TOO_LONG:
        buffer_appends(res, "414 Request Too Long");
        break;
    case HTTP_NOT_IMPLEMENTED:
        buffer_appends(res, "501 Not Implemented");
        break;
    case HTTP_SERVICE_UNAVAILABLE:
        buffer_appends(res, "503 Service Unavailable");
        break;
    case HTTP_INTERNAL_ERROR:
        buffer_appends(res, "500 Internal Server Error");
        break;
    default: /* else */
        buffer_appends(res, "500 This is not a valid status code."
                            "Did you forget to set resp_status?");
        break;
    }
    buffer_appends(res, CRLF);
}

/* Send response headers to client in a single system call. */
static bool
send_response_header(struct http_transaction *ta)
{
    buffer_t response;
    start_response(ta, &response);
    buffer_appends(&ta->resp_headers, CRLF);

    buffer_t *response_and_headers[2] = {
        &response, &ta->resp_headers};

    int rc = bufio_sendbuffers(ta->client->bufio, response_and_headers, 2);
    buffer_delete(&response);
    return rc != -1;
}

/* Send a full response to client with the content in resp_body. */
static bool
send_response(struct http_transaction *ta)
{
    // add content-length.  All other headers must have already been set.
    add_content_length(&ta->resp_headers, ta->resp_body.len);
    buffer_appends(&ta->resp_headers, CRLF);

    buffer_t response;
    start_response(ta, &response);

    buffer_t *response_and_headers[3] = {
        &response, &ta->resp_headers, &ta->resp_body};

    int rc = bufio_sendbuffers(ta->client->bufio, response_and_headers, 3);
    buffer_delete(&response);
    return rc != -1;
}

const int MAX_ERROR_LEN = 2048;

/* Send an error response. */
static bool
send_error(struct http_transaction *ta, enum http_response_status status, const char *fmt, ...)
{
    va_list ap;

    va_start(ap, fmt);
    char *error = buffer_ensure_capacity(&ta->resp_body, MAX_ERROR_LEN);
    int len = vsnprintf(error, MAX_ERROR_LEN, fmt, ap);
    ta->resp_body.len += len > MAX_ERROR_LEN ? MAX_ERROR_LEN - 1 : len;
    va_end(ap);
    ta->resp_status = status;
    http_add_header(&ta->resp_headers, "Content-Type", "text/plain");
    return send_response(ta);
}

/* Send Not Found response. */
static bool
send_not_found(struct http_transaction *ta)
{
    return send_error(ta, HTTP_NOT_FOUND, "File %s not found",
                      bufio_offset2ptr(ta->client->bufio, ta->req_path));
}

/* A start at assigning an appropriate mime type.  Real-world
 * servers use more extensive lists such as /etc/mime.types
 */
static const char *
guess_mime_type(char *filename)
{
    char *suffix = strrchr(filename, '.');
    if (suffix == NULL)
        return "text/plain";

    if (!strcasecmp(suffix, ".html"))
        return "text/html";

    if (!strcasecmp(suffix, ".gif"))
        return "image/gif";

    if (!strcasecmp(suffix, ".png"))
        return "image/png";

    if (!strcasecmp(suffix, ".jpg"))
        return "image/jpeg";

    if (!strcasecmp(suffix, ".js"))
        return "text/javascript";

    /* hint: you need to add support for (at least) .css, .svg, and .mp4
     * You can grep /etc/mime.types for the correct types */

    if (!strcasecmp(suffix, ".css"))
        return "text/css";

    if (!strcasecmp(suffix, ".svg"))
        return "image/svg+xml";

    // video test 3/4
    if (!strcasecmp(suffix, ".mp4"))
        return "video/mp4";

    return "text/plain";
}

// Helper function to check if the path has a dot
static bool has_dot(const char *path)
{
    // Go until null character
    while (*path != '\0')
    {
        if (*path == '.')
        {
            return true;
        }
        path++;
    }
    return false;
}

/* Handle HTTP transaction for static files. */
static bool
handle_static_asset(struct http_transaction *ta, char *basedir)
{
    char fname[PATH_MAX];
    char fname2[PATH_MAX];
    assert(basedir != NULL || !!!"No base directory. Did you specify -R?");
    char *req_path = bufio_offset2ptr(ta->client->bufio, ta->req_path);
    if (!strcmp(req_path, "/"))
    {
        req_path = "/index.html";
    }
    // The code below is vulnerable to an attack.  Can you see
    // which?  Fix it to avoid indirect object reference (IDOR) attacks.
    else if (!has_dot(req_path))
    {
        snprintf(fname2, sizeof fname2, "%s.html", req_path);
        req_path = fname2;
    }
    snprintf(fname, sizeof fname, "%s%s", basedir, req_path);
    if (access(fname, R_OK) == -1)
    {
        if (errno == EACCES)
            return send_error(ta, HTTP_PERMISSION_DENIED, "Permission denied.");
        else
        {
            if (!strstr(req_path, "/api"))
            {
                req_path = "/200.html";
                snprintf(fname, sizeof fname, "%s%s", basedir, req_path);
                if (access(fname, R_OK) == -1)
                {
                    return send_not_found(ta);
                }
            }
            else
            {
                return send_not_found(ta);
            }
        }
    }

    // Determine file size
    struct stat st;
    int rc = stat(fname, &st);
    /* Remove this line once your code handles this case */
    // assert(!(html5_fallback && rc == 0 && S_ISDIR(st.st_mode)));

    if (rc == -1)
        return send_error(ta, HTTP_INTERNAL_ERROR, "Could not stat file.");

    int filefd = open(fname, O_RDONLY);
    if (filefd == -1)
    {
        return send_not_found(ta);
    }

    ta->resp_status = HTTP_OK;
    http_add_header(&ta->resp_headers, "Content-Type", "%s", guess_mime_type(fname));
    // video test 1/3/4
    http_add_header(&ta->resp_headers, "Accept-Ranges", "bytes");
    off_t from = 0, to = st.st_size - 1;
    if (ta->range.is_set)
    {
        ta->resp_status = HTTP_PARTIAL_CONTENT;
        from = ta->range.start;
        if (ta->range.end > 0)
        {
            to = ta->range.end;
        }
        http_add_header(&ta->resp_headers, "Content-Range", "bytes %ld-%ld/%ld", from, to, st.st_size);
    }

    off_t content_length = to + 1 - from;
    add_content_length(&ta->resp_headers, content_length);

    bool success = send_response_header(ta);
    if (!success)
        goto out;

    // sendfile may send fewer bytes than requested, hence the loop
    while (success && from <= to)
        success = bufio_sendfile(ta->client->bufio, filefd, &from, to + 1 - from) > 0;

out:
    close(filefd);
    return success;
}

// Helper function to generate a jwt
static char *generate_jwt(const char *username)
{
    jwt_t *jwt = NULL;
    const char *secret = getenv("SECRET");
    time_t now = time(NULL);
    int exp = now + token_expiration_time;

    jwt_new(&jwt);
    jwt_add_grant(jwt, "sub", username);
    jwt_add_grant_int(jwt, "iat", now);
    jwt_add_grant_int(jwt, "exp", exp);
    jwt_set_alg(jwt, JWT_ALG_HS256, (unsigned char *)secret, strlen(secret));

    char *encoded = jwt_encode_str(jwt);
    jwt_free(jwt);

    return encoded;
}

// Helper function to validate a jwt
static bool validate_jwt(const char *token)
{
    jwt_t *jwt = NULL;
    const char *secret = getenv("SECRET");
    time_t now = time(NULL);
    int exp = now + token_expiration_time;

    if (exp < now)
    {
        jwt_free(jwt);
        return false;
    }

    int valid = 0;
    valid = jwt_decode(&jwt, token, (unsigned char *)secret, strlen(secret));
    jwt_free(jwt);
    return (valid == 0);
}

static bool
handle_api(struct http_transaction *ta)
{
    char *req_path = bufio_offset2ptr(ta->client->bufio, ta->req_path);
    if (strcmp(req_path, "/api/login") == 0)
    {
        ta->resp_status = HTTP_OK;
        if (ta->req_method == HTTP_GET)
        {
            // respond with the claims if token is valid, or an empty json if not
            if (ta->token && validate_jwt(bufio_offset2ptr(ta->client->bufio, ta->token)))
            {
                jwt_t *jwt = NULL;
                const char *secret = getenv("SECRET");
                char *token_str = bufio_offset2ptr(ta->client->bufio, ta->token);
                jwt_decode(&jwt, token_str, (unsigned char *)secret, strlen(secret));
                char *claims_json = jwt_get_grants_json(jwt, NULL);
                buffer_appends(&ta->resp_body, claims_json);
                jwt_free(jwt);
            }
            else
            {
                buffer_appends(&ta->resp_body, "{}");
            }
            http_add_header(&ta->resp_headers, "Content-Type", "application/json");
            return send_response(ta);
        }

        if (ta->req_method == HTTP_POST)
        {
            ta->resp_status = HTTP_OK;

            char *body = bufio_offset2ptr(ta->client->bufio, ta->req_body);
            json_error_t error;
            json_t *root = json_loadb(body, ta->req_content_len, 0, &error);

            // Getting username and pass from request/env
            const char *user = json_string_value(json_object_get(root, "username"));
            const char *pass = json_string_value(json_object_get(root, "password"));
            const char *env_user = getenv("USER_NAME");
            const char *env_pass = getenv("USER_PASS");

            if (!user || !pass || strcmp(user, env_user) != 0 || strcmp(pass, env_pass) != 0)
            {
                return send_error(ta, HTTP_PERMISSION_DENIED, "Invalid username or password");
            }
            else
            {
                // Generate jwt
                char *token = generate_jwt(user);
                if (token)
                {
                    jwt_t *jwt = NULL;
                    const char *secret = getenv("SECRET");
                    jwt_decode(&jwt, token, (unsigned char *)secret, strlen(secret));
                    char *claims_json = jwt_get_grants_json(jwt, NULL);
                    buffer_appends(&ta->resp_body, claims_json);
                    jwt_free(jwt);

                    char fname[PATH_MAX];
                    snprintf(fname, sizeof fname, "auth_jwt_token=%s; Path=/; HttpOnly; SameSite=Lax; Max-Age=%d", token, token_expiration_time);
                    http_add_header(&ta->resp_headers, "Set-Cookie", "%s", fname);
                    http_add_header(&ta->resp_headers, "Content-Type", "application/json");
                    return send_response(ta);
                }
                else
                {
                    return send_error(ta, HTTP_INTERNAL_ERROR, "Token generation failed");
                }
            }
        }
    }
    // else if (strcmp(req_path, "/api/logout") == 0) {
    //     ta->resp_status = HTTP_OK;
    // }
    // video test 2
    else if (strcmp(req_path, "/api/video") == 0)
    {
        if (ta->req_method == HTTP_GET)
        {
            ta->resp_status = HTTP_OK;

            /*
            OPENDIR(3)                 Linux Programmer's Manual                OPENDIR(3)

            NAME
                                        opendir, fdopendir - open a directory

            SYNOPSIS
                                        #include <sys/types.h>
                                        #include <dirent.h>

                                        DIR *opendir(const char *name);
                                        DIR *fdopendir(int fd);
            */
            DIR *dir = opendir(server_root);
            struct dirent *file;
            json_t *arr = json_array();

            for (file = readdir(dir); file != NULL; file = readdir(dir))
            {
                json_t *json = json_object();
                char fname[PATH_MAX];
                snprintf(fname, sizeof fname, "%s/%s", server_root, file->d_name);

                // Determine file size
                struct stat st;
                int rc = stat(fname, &st);
                if (rc == -1)
                {
                    return send_error(ta, HTTP_INTERNAL_ERROR, "Could not stat file.");
                }
                else
                {
                    json_object_set_new(json, "size", json_integer(st.st_size));
                    json_object_set_new(json, "name", json_string(file->d_name));
                    json_array_append_new(arr, json);
                }
            }
            closedir(dir);
            buffer_appends(&ta->resp_body, json_dumps(arr, JSON_COMPACT));
            http_add_header(&ta->resp_headers, "Content-Type", "application/json");
            return send_response(ta);
        }
    }
    else if (strcmp(req_path, "/api/logout") == 0)
    {
        if (ta->req_method == HTTP_POST)
        {
            ta->resp_status = HTTP_OK;
            http_add_header(&ta->resp_headers, "Set-Cookie", "auth_jwt_token=deleted; Path=/; HttpOnly; SameSite=Lax; Max-Age=0");
            buffer_appends(&ta->resp_body, "{}");
            http_add_header(&ta->resp_headers, "Content-Type", "application/json");
            return send_response(ta);
        }
        else
        {
            return send_error(ta, HTTP_METHOD_NOT_ALLOWED, "Method has to be POST");
        }
    }

    return send_error(ta, HTTP_NOT_FOUND, "API not implemented");
}

/* Set up an http client, associating it with a bufio buffer. */
void http_setup_client(struct http_client *self, struct bufio *bufio)
{
    self->bufio = bufio;
}

/* Handle a single HTTP transaction.  Returns true on success. */
bool http_handle_transaction(struct http_client *self)
{
    struct http_transaction ta;
    memset(&ta, 0, sizeof ta);
    ta.client = self;

    if (!http_parse_request(&ta))
        return false;

    if (!http_process_headers(&ta))
        return false;

    if (ta.req_content_len > 0)
    {
        int rc = bufio_read(self->bufio, ta.req_content_len, &ta.req_body);
        if (rc != ta.req_content_len)
            return false;

        // To see the body, use this:
        // char *body = bufio_offset2ptr(ta.client->bufio, ta.req_body);
        // hexdump(body, ta.req_content_len);
    }

    buffer_init(&ta.resp_headers, 1024);
    http_add_header(&ta.resp_headers, "Server", "CS3214-Personal-Server");
    buffer_init(&ta.resp_body, 0);

    bool rc = false;
    char *req_path = bufio_offset2ptr(ta.client->bufio, ta.req_path);
    if (!STARTS_WITH(req_path, "/private") && strstr(req_path, "/private"))
    {
        send_error(&ta, HTTP_BAD_REQUEST, "Bad path");
    }
    if (STARTS_WITH(req_path, "/api"))
    {
        rc = handle_api(&ta);
    }
    else if (STARTS_WITH(req_path, "/private"))
    {
        // priv:
        /* implemented */
        bool error = false;
        char *token = "";
        if (ta.token)
        {
            token = bufio_offset2ptr(ta.client->bufio, ta.token);
            if (validate_jwt(token))
            {
                rc = handle_static_asset(&ta, server_root);
            }
            else
            {
                error = true;
            }
        }
        else
        {
            error = true;
        }
        if (error)
        {
            send_error(&ta, HTTP_PERMISSION_DENIED, "Invalid token");
        }
    }
    else
    {
        rc = handle_static_asset(&ta, server_root);
    }

    buffer_delete(&ta.resp_headers);
    buffer_delete(&ta.resp_body);

    return rc;
}
