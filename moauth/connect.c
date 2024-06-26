//
// Connection support for moauth library
//
// Copyright © 2017-2024 by Michael R Sweet
//
// Licensed under Apache License v2.0.  See the file "LICENSE" for more information.
//

#include <config.h>
#include "moauth-private.h"


//
// 'moauthClose()' - Close an OAuth server connection.
//

void
moauthClose(moauth_t *server)		// I - OAuth server connection
{
  if (server)
  {
    cupsJSONDelete(server->metadata);
    free(server);
  }
}


//
// '_moauthConnect()' - Connect to the server for the provided URI and return
//                      the associated resource.
//

http_t *				// O - HTTP connection or `NULL`
_moauthConnect(const char *uri,		// I - URI to connect to
               char       *resource,	// I - Resource buffer
               size_t     resourcelen)	// I - Size of resource buffer
{
  char		scheme[32],		// URI scheme
		userpass[256],		// Username:password (unused)
		host[256];		// Host
  int		port;			// Port number
  http_t	*http;			// HTTP connection
  char		*peercreds;		// Peer credentials


//  fprintf(stderr, "_moauthConect(uri=\"%s\", resource=%p, resourcelen=%lu)\n", uri, (void *)resource, (unsigned long)resourcelen);

  if (httpSeparateURI(HTTP_URI_CODING_ALL, uri, scheme, sizeof(scheme), userpass, sizeof(userpass), host, sizeof(host), &port, resource, (int)resourcelen) < HTTP_URI_STATUS_OK || strcmp(scheme, "https"))
  {
//    fputs("_moauthConnect: Bad URI\n", stderr);
    return (NULL);			// Bad URI
  }

  http = httpConnect(host, port, NULL, AF_UNSPEC, HTTP_ENCRYPTION_ALWAYS, true, 30000, NULL);
//  if (!http)
//    fprintf(stderr, "_moauthConnect: Unable to connect to %s:%d: %s\n", host, port, cupsGetErrorString());

  if ((peercreds = httpCopyPeerCredentials(http)) == NULL)
  {
    httpClose(http);
    return (NULL);
  }

  switch (cupsGetCredentialsTrust(/*path*/NULL, host, peercreds, /*require_ca*/true))
  {
    case HTTP_TRUST_OK :      // Credentials are OK/trusted
    case HTTP_TRUST_RENEWED : // Credentials have been renewed
    case HTTP_TRUST_UNKNOWN : // Credentials are unknown/new
        break;

    case HTTP_TRUST_INVALID : // Credentials are invalid
    case HTTP_TRUST_CHANGED : // Credentials have changed
    case HTTP_TRUST_EXPIRED : // Credentials are expired
//        fputs("_moauthConnect: Unable to trust.\n", stderr);
        httpClose(http);
        free(peercreds);
        return (NULL);
  }

  cupsSaveCredentials(/*path*/NULL, host, peercreds, /*key*/NULL);
  free(peercreds);

  return (http);
}


//
// 'moauthConnect()' - Open a connection to an OAuth server.
//

moauth_t *				// O - OAuth server connection or `NULL`
moauthConnect(
    const char *oauth_uri)		// I - Authorization URI
{
  http_t	*http;			// Connection to OAuth server
  char		resource[256];		// Resource path
  moauth_t	*server;		// OAuth server connection
  http_status_t	status;			// HTTP GET response status
  const char	*content_type = NULL;	// Message body format
  char		*body = NULL;		// HTTP message body


  // Connect to the OAuth URI...
  if ((http = _moauthConnect(oauth_uri, resource, sizeof(resource))) == NULL)
    return (NULL);			// Unable to connect to server

  if ((server = calloc(1, sizeof(moauth_t))) == NULL)
    return (NULL);			// Unable to allocate server structure

  // Get the metadata from the specified URL.  If the resource is "/" (default)
  // then grab the well-known RFC 8414 or OpenID configuration paths.
  if (!strcmp(resource, "/"))
  {
    httpClearFields(http);

    if (httpWriteRequest(http, "GET", "/.well-known/oauth-authorization-server"))
    {
      // GET succeeded, grab the response...
      while ((status = httpUpdate(http)) == HTTP_STATUS_CONTINUE);
    }
    else
    {
      status = HTTP_STATUS_ERROR;
    }

    if (status == HTTP_STATUS_OK)
    {
      content_type = httpGetField(http, HTTP_FIELD_CONTENT_TYPE);
      body         = _moauthCopyMessageBody(http);
    }
    else
    {
      httpFlush(http);
    }
  }

  if (!strcmp(resource, "/") && !body)
  {
    httpClearFields(http);

    if (httpWriteRequest(http, "GET", "/.well-known/openid-configuration"))
    {
      // GET succeeded, grab the response...
      while ((status = httpUpdate(http)) == HTTP_STATUS_CONTINUE);
    }
    else
    {
      status = HTTP_STATUS_ERROR;
    }

    if (status == HTTP_STATUS_OK)
    {
      content_type = httpGetField(http, HTTP_FIELD_CONTENT_TYPE);
      body         = _moauthCopyMessageBody(http);
    }
    else
    {
      httpFlush(http);
    }
  }

  if (!body)
  {
    httpClearFields(http);

    if (httpWriteRequest(http, "GET", resource))
    {
      // GET succeeded, grab the response...
      while (httpUpdate(http) == HTTP_STATUS_CONTINUE);

      content_type = httpGetField(http, HTTP_FIELD_CONTENT_TYPE);
      body         = _moauthCopyMessageBody(http);
    }
  }

  if (content_type && body)
  {
    char	scheme[32],		// URI scheme
		userpass[256],		// Username:password (unused)
		host[256];		// Host
    int		port;			// Port number
    bool	is_json = !*content_type || !strcmp(content_type, "text/json");
					// JSON metadata?

    httpClose(http);

    if (is_json)
    {
      // OpenID/RFC 8414 JSON metadata...
      const char *uri;			// Authorization/token URI

      server->metadata = cupsJSONImportString(body);

      if ((uri = cupsJSONGetString(cupsJSONFind(server->metadata, "authorization_endpoint"))) != NULL)
      {
	if (httpSeparateURI(HTTP_URI_CODING_ALL, uri, scheme, sizeof(scheme), userpass, sizeof(userpass), host, sizeof(host), &port, resource, sizeof(resource)) < HTTP_URI_STATUS_OK || strcmp(scheme, "https"))
        {
          // Bad authorization URI...
          moauthClose(server);
	  return (NULL);
	}

        server->authorization_endpoint = uri;
      }

      if ((uri = cupsJSONGetString(cupsJSONFind(server->metadata, "introspection_endpoint"))) != NULL)
      {
	if (httpSeparateURI(HTTP_URI_CODING_ALL, uri, scheme, sizeof(scheme), userpass, sizeof(userpass), host, sizeof(host), &port, resource, sizeof(resource)) < HTTP_URI_STATUS_OK || strcmp(scheme, "https"))
        {
          // Bad introspection URI...
          moauthClose(server);
	  return (NULL);
	}

        server->introspection_endpoint = uri;
      }

      if ((uri = cupsJSONGetString(cupsJSONFind(server->metadata, "registration_endpoint"))) != NULL)
      {
	if (httpSeparateURI(HTTP_URI_CODING_ALL, uri, scheme, sizeof(scheme), userpass, sizeof(userpass), host, sizeof(host), &port, resource, sizeof(resource)) < HTTP_URI_STATUS_OK || strcmp(scheme, "https"))
        {
          // Bad registration URI...
          moauthClose(server);
	  return (NULL);
	}

        server->registration_endpoint = uri;
      }

      if ((uri = cupsJSONGetString(cupsJSONFind(server->metadata, "token_endpoint"))) != NULL)
      {
	if (httpSeparateURI(HTTP_URI_CODING_ALL, uri, scheme, sizeof(scheme), userpass, sizeof(userpass), host, sizeof(host), &port, resource, sizeof(resource)) < HTTP_URI_STATUS_OK || strcmp(scheme, "https"))
        {
          // Bad token URI...
          moauthClose(server);
	  return (NULL);
	}

        server->token_endpoint = uri;
      }
    }

    free(body);
  }

  if (!server->authorization_endpoint || !server->token_endpoint)
  {
    // OAuth server does not provide endpoints, unable to support it.
    moauthClose(server);
    return (NULL);
  }

  return (server);
}


//
// 'moauthErrorString()' - Return a description of the last error that occurred,
//                         if any.
//

const char *				// O - Last error description or `NULL` if none
moauthErrorString(moauth_t *server)	// I - OAuth server connection
{
  return ((server && server->error[0]) ? server->error : NULL);
}
