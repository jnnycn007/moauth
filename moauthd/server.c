//
// Server support for moauth daemon
//
// Copyright © 2017-2024 by Michael R Sweet
//
// Licensed under Apache License v2.0.  See the file "LICENSE" for more
// information.
//

#include "moauthd.h"
#include <cups/jwt.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <syslog.h>
#include <grp.h>
#include "index-md.h"
#include "moauth-png.h"
#include "style-css.h"




//
// Local functions...
//

static int	compare_applications(moauthd_application_t *a, moauthd_application_t *b);
static moauthd_application_t *copy_application(moauthd_application_t *a);
static void	free_application(moauthd_application_t *a);
static int	get_seconds(const char *value);
static bool	load_config(moauthd_server_t *server, const char *configfile, cups_file_t *fp);
static bool	load_state(moauthd_server_t *server);


//
// 'moauthdAddApplication()' - Add an application (OAuth client) to the server.
//

moauthd_application_t *			// O - New application object
moauthdAddApplication(
    moauthd_server_t *server,		// I - Server
    const char       *client_id,	// I - Client ID
    const char       *redirect_uri,	// I - Redirection URI
    const char       *client_name,	// I - Human-readable name or `NULL` for none
    const char       *client_uri,	// I - Web page or `NULL` for none
    const char       *logo_uri,		// I - Logo URI or `NULL` for none
    const char       *tos_uri)		// I - Terms-of-service URI or `NULL` for none
{
  moauthd_application_t	temp,		// Temporary application data
			*app;		// New application


  temp.client_id    = (char *)client_id;
  temp.redirect_uri = (char *)redirect_uri;
  temp.client_name  = (char *)client_name;
  temp.client_uri   = (char *)client_uri;
  temp.logo_uri     = (char *)logo_uri;
  temp.tos_uri      = (char *)tos_uri;

  cupsMutexLock(&server->applications_lock);

  if (!server->applications)
    server->applications = cupsArrayNew((cups_array_cb_t)compare_applications, NULL, NULL, 0, (cups_acopy_cb_t)copy_application, (cups_afree_cb_t)free_application);

  cupsArrayAdd(server->applications, &temp);

  app = (moauthd_application_t *)cupsArrayFind(server->applications, &temp);

  cupsMutexUnlock(&server->applications_lock);

  return (app);
}


//
// 'moauthdCreateServer()' - Create a new server object and load the specified config file.
//

moauthd_server_t *			// O - New server object
moauthdCreateServer(
    const char *configfile,		// I - Configuration file to load
    const char *statefile,		// I - State file to save
    int        verbosity)		// I - Extra verbosity from command-line
{
  moauthd_server_t *server;		// Server object
  cups_file_t	*fp = NULL;		// Opened config file
  http_addrlist_t *addrlist,		// List of listener addresses
		*addr;			// Current address
  char		temp[1024],		// Temporary string
		*tempptr;		// Pointer into temporary string
  struct stat	tempinfo;		// Temporary information
  cups_json_t	*json,			// OpenID/RFC 8414 JSON metadata
		*jarray;		// Array value
  moauthd_resource_t *r;		// Resource
  cups_array_t	*scopes;		// List of scopes
  const char	*scope;			// Current scope


  // Open the configuration file if one is specified...
  if (configfile && (fp = cupsFileOpen(configfile, "r")) == NULL)
  {
    fprintf(stderr, "moauthd: Unable to open configuration file \"%s\": %s\n", configfile, strerror(errno));
    return (NULL);
  }

  // Allocate a server object and initialize with defaults...
  server = calloc(1, sizeof(moauthd_server_t));

  cupsMutexInit(&server->applications_lock);
  cupsRWInit(&server->resources_lock);
  cupsRWInit(&server->tokens_lock);

  server->introspect_group = -1;	// none
  server->log_file         = 2;		// stderr
  server->log_level        = MOAUTHD_LOGLEVEL_ERROR;
  server->max_grant_life   = 300;	// 5 minutes
  server->max_token_life   = 604800;	// 1 week
  server->register_group   = -1;	// none

  if (fp)
  {
    bool status = load_config(server, configfile, fp);

    cupsFileClose(fp);

    if (!status)
      goto create_failed;
  }

  if (!server->name)
  {
    char	name[256],		// Host name
		*nameptr;		// Pointer into name

    httpGetHostname(NULL, name, sizeof(name));
    nameptr = name + strlen(name) - 1;
    if (nameptr > name && *nameptr == '.')
      *nameptr = '\0';			// Strip trailing "." from hostname

    if ((server->name = strdup(name)) == NULL)
    {
      fprintf(stderr, "moauthd: Unable to allocate server name: %s\n", strerror(errno));
      goto create_failed;
    }
  }

  if (!server->port)
    server->port = 9000 + (getuid() % 1000);

  // Update logging and log our authorization server's URL...
  if (verbosity == 1 && server->log_level < MOAUTHD_LOGLEVEL_DEBUG)
    server->log_level ++;
  else if (verbosity > 1)
    server->log_level = MOAUTHD_LOGLEVEL_DEBUG;

  // Save state file...
  server->state_file = strdup(statefile);

  if (!load_state(server))
    goto create_failed;

  // Setup listeners...
  snprintf(temp, sizeof(temp), "%d", server->port);

  for (addrlist = httpAddrGetList(NULL, AF_UNSPEC, temp), addr = addrlist; addr; addr = addr->next)
  {
    int			sock = httpAddrListen(&(addr->addr), server->port);
					// Listener socket
    struct pollfd	*lis = server->listeners + server->num_listeners;
					// Pointer to polling data

    if (sock < 0)
    {
      fprintf(stderr, "moauthd: Unable to listen to \"%s:%d\": %s\n", httpAddrGetString(&(addr->addr), temp, sizeof(temp)), server->port, strerror(errno));
      continue;
    }

    if (server->num_listeners >= (int)(sizeof(server->listeners) / sizeof(server->listeners[0])))
    {
      // Unlikely, but ignore more than N listeners...
      fputs("moauthd: Ignoring extra listener addresses.\n", stderr);
      break;
    }

    server->num_listeners ++;
    lis->fd     = sock;
    lis->events = POLLIN | POLLHUP | POLLERR;
  }

  httpAddrFreeList(addrlist);

  if (server->num_listeners == 0)
  {
    moauthdLogs(server, MOAUTHD_LOGLEVEL_ERROR, "No working listener sockets.");
    goto create_failed;
  }

  moauthdLogs(server, MOAUTHD_LOGLEVEL_INFO, "Authorization server is \"https://%s:%d\".", server->name, server->port);

  if (!server->auth_service)
    server->auth_service = strdup("login");

  cupsSetServerCredentials(getenv("SNAP_DATA"), server->name, true);

  // Generate OpenID/RFC 8414 JSON metadata...
  json = cupsJSONNew(NULL, NULL, CUPS_JTYPE_OBJECT);

  // issuer
  //
  // REQUIRED. URL using the https scheme with no query or fragment component
  // that the OP asserts as its Issuer Identifier. If Issuer discovery is
  // supported (see Section 2), this value MUST be identical to the issuer value
  // returned by WebFinger. This also MUST be identical to the iss Claim value
  // in ID Tokens issued from this Issuer.
  httpAssembleURI(HTTP_URI_CODING_ALL, temp, sizeof(temp), "https", /*userpass*/NULL, server->name, server->port, "/");
  cupsJSONNewString(json, cupsJSONNewKey(json, NULL, "issuer"), temp);

  // authorization_endpoint
  //
  // REQUIRED. URL of the OP's OAuth 2.0 Authorization Endpoint [RFC8414].
  httpAssembleURI(HTTP_URI_CODING_ALL, temp, sizeof(temp), "https", /*userpass*/NULL, server->name, server->port, "/authorize");
  cupsJSONNewString(json, cupsJSONNewKey(json, NULL, "authorization_endpoint"), temp);

  // token_endpoint
  //
  // URL of the OP's OAuth 2.0 Token Endpoint [RFC8414]. This is REQUIRED
  // unless only the Implicit Flow is used.
  httpAssembleURI(HTTP_URI_CODING_ALL, temp, sizeof(temp), "https", /*userpass*/NULL, server->name, server->port, "/token");
  cupsJSONNewString(json, cupsJSONNewKey(json, NULL, "token_endpoint"), temp);

  // userinfo_endpoint
  //
  // RECOMMENDED. URL of the OP's UserInfo Endpoint [OpenID.Core]. This URL MUST
  // use the https scheme and MAY contain port, path, and query parameter
  // components.
  httpAssembleURI(HTTP_URI_CODING_ALL, temp, sizeof(temp), "https", /*userpass*/NULL, server->name, server->port, "/userinfo");
  cupsJSONNewString(json, cupsJSONNewKey(json, NULL, "userinfo_endpoint"), temp);

  // jwks_uri
  //
  // REQUIRED. URL of the OP's JSON Web Key Set [RFC7517] document. This
  // contains the signing key(s) the RP uses to validate signatures from the OP.
  // The JWK Set MAY also contain the Server's encryption key(s), which are used
  // by RPs to encrypt requests to the Server. When both signing and encryption
  // keys are made available, a use (Key Use) parameter value is REQUIRED for
  // all keys in the referenced JWK Set to indicate each key's intended usage.
  // Although some algorithms allow the same key to be used for both signatures
  // and encryption, doing so is NOT RECOMMENDED, as it is less secure. The JWK
  // x5c parameter MAY be used to provide X.509 representations of keys
  // provided. When used, the bare key values MUST still be present and MUST
  // match those in the certificate.
  httpAssembleURI(HTTP_URI_CODING_ALL, temp, sizeof(temp), "https", /*userpass*/NULL, server->name, server->port, "/.well-known/jwks.json");
  cupsJSONNewString(json, cupsJSONNewKey(json, NULL, "jwks_uri"), temp);

  // registration_endpoint
  //
  // RECOMMENDED. URL of the OP's Dynamic Client Registration Endpoint [RFC7591].
  httpAssembleURI(HTTP_URI_CODING_ALL, temp, sizeof(temp), "https", /*userpass*/NULL, server->name, server->port, "/register");
  cupsJSONNewString(json, cupsJSONNewKey(json, NULL, "registration_endpoint"), temp);

  // scopes_supported
  //
  // RECOMMENDED. JSON array containing a list of the OAuth 2.0 [RFC6749]
  // scope values that this server supports. The server MUST support the
  // openid scope value. Servers MAY choose not to advertise some supported
  // scope values even when this parameter is used, although those defined in
  // [OpenID.Core] SHOULD be listed, if supported.
  jarray = cupsJSONNew(json, cupsJSONNewKey(json, NULL, "scopes_supported"), CUPS_JTYPE_ARRAY);
  cupsJSONNewString(jarray, NULL, "openid");

  // Loop through resources and collect the list of scope names...
  scopes = cupsArrayNewStrings(NULL, '\0');

  for (r = (moauthd_resource_t *)cupsArrayGetFirst(server->resources); r; r = (moauthd_resource_t *)cupsArrayGetNext(server->resources))
  {
    if (!cupsArrayFind(scopes, r->scope))
      cupsArrayAdd(scopes, r->scope);
  }

  for (scope = (const char *)cupsArrayGetFirst(scopes); scope; scope = (const char *)cupsArrayGetNext(scopes))
    cupsJSONNewString(jarray, NULL, scope);

  cupsArrayDelete(scopes);

  // response_types_supported
  //
  // REQUIRED. JSON array containing a list of the OAuth 2.0 response_type
  // values that this OP supports. Dynamic OpenID Providers MUST support the
  // code, id_token, and the token Response Type values.
  jarray = cupsJSONNew(json, cupsJSONNewKey(json, NULL, "response_types_supported"), CUPS_JTYPE_ARRAY);
  cupsJSONNewString(jarray, NULL, "code");
  cupsJSONNewString(jarray, NULL, "id_token");
  cupsJSONNewString(jarray, NULL, "token");

  // subject_types_supported
  //
  // REQUIRED. JSON array containing a list of the Subject Identifier types that
  // this OP supports. Valid types include pairwise and public.
  jarray = cupsJSONNew(json, cupsJSONNewKey(json, NULL, "subject_types_supported"), CUPS_JTYPE_ARRAY);
  cupsJSONNewString(jarray, NULL, "pairwise");
  cupsJSONNewString(jarray, NULL, "public");

  // id_token_signing_alg_values_supported
  //
  // REQUIRED. JSON array containing a list of the JWS signing algorithms
  // (alg values) supported by the OP for the ID Token to encode the Claims
  // in a JWT [JWT]. The algorithm RS256 MUST be included. The value none MAY
  // be supported but MUST NOT be used unless the Response Type used returns
  // no ID Token from the Authorization Endpoint (such as when using the
  // Authorization Code Flow).
  jarray = cupsJSONNew(json, cupsJSONNewKey(json, NULL, "id_token_signing_alg_values_supported"), CUPS_JTYPE_ARRAY);
  cupsJSONNewString(jarray, NULL, "RS256");

  // claims_supported
  //
  // RECOMMENDED. JSON array containing a list of the Claim Names of the Claims
  // that the OpenID Provider MAY be able to supply values for. Note that for
  // privacy or other reasons, this might not be an exhaustive list.
  jarray = cupsJSONNew(json, cupsJSONNewKey(json, NULL, "claims_supported"), CUPS_JTYPE_ARRAY);
  cupsJSONNewString(jarray, NULL, "email");
  cupsJSONNewString(jarray, NULL, "name");
  cupsJSONNewString(jarray, NULL, "phone_number");
  cupsJSONNewString(jarray, NULL, "preferred_username");
  cupsJSONNewString(jarray, NULL, "sub");
  cupsJSONNewString(jarray, NULL, "updated_at");

  // token_endpoint_auth_methods_supported
  //
  // List of auth methods for the token endpoint [RFC8414].  The default is
  // "client_secret_basic" but we want "none".
  jarray = cupsJSONNew(json, cupsJSONNewKey(json, NULL, "token_endpoint_auth_methods_supported"), CUPS_JTYPE_ARRAY);
  cupsJSONNewString(jarray, NULL, "none");

  // introspection_endpoint
  //
  // URL of the OP's OAuth 2.0 Introspection Endpoint [RFC8414] [RFC7662].
  httpAssembleURI(HTTP_URI_CODING_ALL, temp, sizeof(temp), "https", /*userpass*/NULL, server->name, server->port, "/introspect");
  cupsJSONNewString(json, cupsJSONNewKey(json, NULL, "introspection_endpoint"), temp);

  // grant_types_supported
  //
  // OPTIONAL. JSON array containing a list of the OAuth 2.0 Grant Type values
  // that this OP supports. Dynamic OpenID Providers MUST support the
  // authorization_code and implicit Grant Type values and MAY support other
  // Grant Types. If omitted, the default value is
  // ["authorization_code", "implicit"].
  jarray = cupsJSONNew(json, cupsJSONNewKey(json, NULL, "grant_types_supported"), CUPS_JTYPE_ARRAY);
  cupsJSONNewString(jarray, NULL, "authorization_code");
  cupsJSONNewString(jarray, NULL, "password");
  cupsJSONNewString(jarray, NULL, "refresh_token");

  // Encode the metadata for later delivery to clients...
  server->metadata = cupsJSONExportString(json);
  cupsJSONDelete(json);

  // Make the JSON Web Key Set from our private key...
  json = cupsJSONNew(NULL, NULL, CUPS_JTYPE_OBJECT);
  jarray = cupsJSONNew(json, cupsJSONNewKey(json, NULL, "keys"), CUPS_JTYPE_ARRAY);
  cupsJSONAdd(jarray, NULL, cupsJWTMakePublicKey(server->private_key));

  server->public_key = cupsJSONExportString(json);
  cupsJSONDelete(json);

  // Final setup...
  time(&server->start_time);

  if (!server->secret)
  {
    // Generate a random secret string that is used when creating token UUIDs.
    _moauthGetRandomBytes(temp, sizeof(temp) - 1);
    for (tempptr = temp; tempptr < (temp + sizeof(temp) - 1); tempptr ++)
      *tempptr = (*tempptr % 95) + ' ';
    *tempptr = '\0';
    server->secret = strdup(temp);
  }

  // Add RFC 8414 configuration file.
  r = moauthdCreateResource(server, MOAUTHD_RESTYPE_STATIC_FILE, "/.well-known/oauth-authorization-server", NULL, "text/json", "public");
  r->data   = server->metadata;
  r->length = strlen(server->metadata);

  // Add OpenID configuration file.
  r = moauthdCreateResource(server, MOAUTHD_RESTYPE_STATIC_FILE, "/.well-known/openid-configuration", NULL, "text/json", "public");
  r->data   = server->metadata;
  r->length = strlen(server->metadata);

  // Add JWKS file.
  r = moauthdCreateResource(server, MOAUTHD_RESTYPE_STATIC_FILE, "/.well-known/jwks.json", NULL, "text/json", "public");
  r->data   = server->public_key;
  r->length = strlen(server->public_key);

  // Add other standard resources...
  if (!moauthdFindResource(server, "/index.html", temp, sizeof(temp), &tempinfo) && !moauthdFindResource(server, "/index.md", temp, sizeof(temp), &tempinfo))
  {
    // Add default home page file...
    r = moauthdCreateResource(server, MOAUTHD_RESTYPE_STATIC_FILE, "/index.md", NULL, "text/markdown", "public");
    r->data   = index_md;
    r->length = strlen(index_md);
  }

  if (!moauthdFindResource(server, "/moauth.png", temp, sizeof(temp), &tempinfo))
  {
    // Add default moauth.png file...
    r = moauthdCreateResource(server, MOAUTHD_RESTYPE_STATIC_FILE, "/moauth.png", NULL, "image/png", "public");
    r->data   = moauth_png;
    r->length = sizeof(moauth_png);
  }

  if (!moauthdFindResource(server, "/style.css", temp, sizeof(temp), &tempinfo))
  {
    // Add default style.css file...
    r = moauthdCreateResource(server, MOAUTHD_RESTYPE_STATIC_FILE, "/style.css", NULL, "text/css", "public");
    r->data   = style_css;
    r->length = strlen(style_css);
  }

  // Return the server object...
  return (server);


  // If we get here something went wrong...
  create_failed:

  moauthdDeleteServer(server);

  return (NULL);
}


//
// 'moauthdDeleteServer()' - Delete a server object.
//

void
moauthdDeleteServer(
    moauthd_server_t *server)		// I - Server object
{
  int	i;				// Looping var


  free(server->name);
  free(server->state_file);
  free(server->auth_service);

  for (i = 0; i < server->num_listeners; i ++)
    httpAddrClose(NULL, server->listeners[i].fd);

  cupsArrayDelete(server->applications);
  cupsArrayDelete(server->resources);
  cupsArrayDelete(server->tokens);

  cupsMutexDestroy(&server->applications_lock);
  cupsRWDestroy(&server->resources_lock);
  cupsRWDestroy(&server->tokens_lock);

  cupsJSONDelete(server->private_key);

  free(server->public_key);
  free(server->test_password);
  free(server->metadata);

  free(server);
}


//
// 'moauthdFindApplication()' - Find an application by its client ID.
//

moauthd_application_t *			// O - Matching application, if any
moauthdFindApplication(
    moauthd_server_t *server,		// I - Server object
    const char       *client_id,	// I - Client ID
    const char       *redirect_uri)	// I - Redirect URI or NULL
{
  moauthd_application_t	*app,		// Matching application
			key;		// Search key


  cupsMutexLock(&server->applications_lock);

  if (redirect_uri)
  {
    // Exact match...
    key.client_id    = (char *)client_id;
    key.redirect_uri = (char *)redirect_uri;

    app = (moauthd_application_t *)cupsArrayFind(server->applications, &key);
  }
  else
  {
    // First matching client ID...
    for (app = (moauthd_application_t *)cupsArrayGetFirst(server->applications); app; app = (moauthd_application_t *)cupsArrayGetNext(server->applications))
    {
      if (!strcmp(app->client_id, client_id))
        break;
    }
  }

  cupsMutexUnlock(&server->applications_lock);

  return (app);
}


//
// 'moauthdRunServer()' - Listen for client connections and process requests.
//

int					// O - Exit status
moauthdRunServer(
    moauthd_server_t *server)		// I - Server object
{
  bool	done = false;			// Are we done yet?


  if (!server)
    return (1);

  moauthdLogs(server, MOAUTHD_LOGLEVEL_INFO, "Listening for client connections.");

  while (!done)
  {
    if (poll(server->listeners, server->num_listeners, -1) < 0)
    {
      if (errno != EAGAIN && errno != EINTR)
      {
        moauthdLogs(server, MOAUTHD_LOGLEVEL_ERROR, "poll() failed: %s", strerror(errno));
        done = true;
      }
    }
    else
    {
      int		i;		// Looping var
      struct pollfd	*lis;		// Current listener

      for (i = server->num_listeners, lis = server->listeners; i > 0; i --, lis ++)
      {
        if (lis->revents & POLLIN)
	{
	  moauthd_client_t *client = moauthdCreateClient(server, lis->fd);
					// New client

          if (client)
          {
            cups_thread_t tid;		// New processing thread

            if ((tid = cupsThreadCreate((void *(*)(void *))moauthdRunClient, client)) == CUPS_THREAD_INVALID)
            {
              // Unable to create client thread...
              moauthdLogs(server, MOAUTHD_LOGLEVEL_ERROR, "Unable to create client processing thread: %s", strerror(errno));
              moauthdDeleteClient(client);
	    }
	    else
	    {
	      // Client thread created, detach!
	      cupsThreadDetach(tid);
	    }
          }
	}
      }
    }
  }

  return (0);
}


//
// 'moauthdSaveServer()' - Save the server state.
//

bool					// O - `true` on success, `false` on error
moauthdSaveServer(
    moauthd_server_t *server)		// I - Server
{
  cups_file_t	*fp;			// State file
  char		oldfile[1024],		// Old state file
		newfile[1024],		// New state file
		*temp;			// Temporary string


  // Write the new state then rename the old...
  snprintf(oldfile, sizeof(oldfile), "%s.O", server->state_file);
  snprintf(newfile, sizeof(newfile), "%s.N", server->state_file);

  if ((fp = cupsFileOpen(newfile, "w")) == NULL)
  {
    fprintf(stderr, "moauthd: Unable to write state file \"%s\": %s\n", newfile, strerror(errno));
    return (false);
  }

  // State files are only readable by the owner...
  fchmod(cupsFileNumber(fp), 0600);

  // Write state...
  if ((temp = cupsJSONExportString(server->private_key)) != NULL)
  {
    cupsFilePutConf(fp, "PrivateKey", temp);
    free(temp);
  }

  // Close file and rename...
  cupsFileClose(fp);
  if (rename(server->state_file, oldfile) && errno != ENOENT)
  {
    // Can't rename old state, try removing...
    fprintf(stderr, "moauthd: Unable to rename state file \"%s\": %s\n", server->state_file, strerror(errno));
    if (unlink(server->state_file))
      return (false);
  }

  if (rename(newfile, server->state_file))
  {
    fprintf(stderr, "moauthd: Unable to rename state file \"%s\": %s\n", newfile, strerror(errno));
    return (false);
  }

  return (true);
}


//
// 'compare_applications()' - Compare two application registrations.
//

static int				// O - Result of comparison
compare_applications(
    moauthd_application_t *a,		// I - First application
    moauthd_application_t *b)		// I - Second application
{
  return (strcmp(a->client_id, b->client_id));
}


//
// 'copy_application()' - Make a copy of an application object.
//

static moauthd_application_t *		// O - New application object
copy_application(
    moauthd_application_t *a)		// I - Application object
{
  moauthd_application_t	*na;		// New application object


  if ((na = (moauthd_application_t *)calloc(1, sizeof(moauthd_application_t))) != NULL)
  {
    na->client_id    = strdup(a->client_id);
    na->redirect_uri = strdup(a->redirect_uri);

    if (a->client_name)
      na->client_name = strdup(a->client_name);
    if (a->client_uri)
      na->client_uri = strdup(a->client_uri);
    if (a->logo_uri)
      na->logo_uri = strdup(a->logo_uri);
    if (a->tos_uri)
      na->tos_uri = strdup(a->tos_uri);
  }

  return (na);
}


//
// 'free_application()' - Free an application object.
//

static void
free_application(
    moauthd_application_t *a)		// I - Application object
{
  free(a->client_id);
  free(a->redirect_uri);
  free(a->client_name);
  free(a->client_uri);
  free(a->logo_uri);
  free(a->tos_uri);
  free(a);
}


//
// 'get_seconds()' - Get a time value in seconds.
//

static int				// O - Number of seconds or -1 on error
get_seconds(const char *value)		// I - Value string
{
  char	*units;				// Pointer to units
  int	tval = (int)strtol(value, &units, 10);
					// Time value

  if (!strcasecmp(units, "m"))
    tval *= 60;
  else if (!strcasecmp(units, "h"))
    tval *= 3600;
  else if (!strcasecmp(units, "d"))
    tval *= 86400;
  else if (!strcasecmp(units, "w"))
    tval *= 604800;
  else
    tval = -1;

  return (tval);
}


//
// 'load_config()' - Load the server configuration.
//

static bool				// O - `true` on success, `false` on failure
load_config(
    moauthd_server_t *server,		// I - Server
    const char       *configfile,	// I - Config filename
    cups_file_t      *fp)		// I - Config file
{
  char		line[2048],		// Line from config file
		*value,			// Value from config file
		*ptr;			// Pointer into value
  int		linenum = 0;		// Current line number
  struct group	*group;			// Group information


  // Load configuration from file...
  while (cupsFileGetConf(fp, line, sizeof(line), &value, &linenum))
  {
    if (!strcasecmp(line, "Application"))
    {
      // Application client-id redirect-uri client-name
      const char	*client_id,	// Client ID
		      *client_name,	// Client name
		      *redirect_uri;	// Redirection URI

      if (!value)
      {
	fprintf(stderr, "moauthd: Missing client ID, redirect URI, and name on line %d of \"%s\".\n", linenum, configfile);
	return (false);
      }

      client_id = ptr = value;
      while (*ptr && !isspace(*ptr))
	ptr ++;
      while (*ptr && isspace(*ptr))
	*ptr++ = '\0';

      redirect_uri = ptr;
      while (*ptr && !isspace(*ptr))
	ptr ++;
      while (*ptr && isspace(*ptr))
	*ptr++ = '\0';

      client_name = ptr;

      if (!*client_id || !*redirect_uri)
      {
	fprintf(stderr, "moauthd: Missing client ID and redirect URI on line %d of \"%s\".\n", linenum, configfile);
	return (false);
      }

      moauthdAddApplication(server, client_id, redirect_uri, client_name, NULL, NULL, NULL);
    }
    else if (!strcasecmp(line, "LogFile"))
    {
      // LogFile {filename,none,stderr,syslog}
      if (!value || !strcasecmp(value, "stderr"))
      {
	server->log_file = 2;
      }
      else if (!strcmp(value, "none"))
      {
	server->log_file = -1;
      }
      else if (!strcasecmp(value, "syslog"))
      {
	server->log_file = 0;
	openlog("moauthd", LOG_CONS, LOG_AUTH);
      }
      else if ((server->log_file = open(value, O_WRONLY | O_CREAT | O_APPEND, 0600)) < 0)
      {
	fprintf(stderr, "moauthd: Unable to open log file \"%s\" on line %d of \"%s\": %s\n", value, linenum, configfile, strerror(errno));
	return (false);
      }
    }
    else if (!strcasecmp(line, "LogLevel"))
    {
      // LogLevel {error,info,debug}
      if (!value)
      {
	fprintf(stderr, "moauthd: Missing log level on line %d of \"%s\".\n", linenum, configfile);
	return (false);
      }
      else if (!strcasecmp(value, "error"))
	server->log_level = MOAUTHD_LOGLEVEL_ERROR;
      else if (!strcasecmp(value, "info"))
	server->log_level = MOAUTHD_LOGLEVEL_INFO;
      else if (!strcasecmp(value, "debug"))
	server->log_level = MOAUTHD_LOGLEVEL_DEBUG;
      else
      {
	fprintf(stderr, "moauthd: Unknown LogLevel \"%s\" on line %d of \"%s\" ignored.\n", value, linenum, configfile);
      }
    }
    else if (!strcasecmp(line, "IntrospectGroup"))
    {
      // IntrospectGroup nnn
      // IntrospectGroup name
      //
      // Required group membership (and thus required authentication for)
      // token introspection.
      if (!value)
      {
	fprintf(stderr, "moauthd: Missing IntrospectGroup on line %d of \"%s\".\n", linenum, configfile);
	return (false);
      }
      else if (isdigit(*value))
      {
	server->introspect_group = (gid_t)strtol(value, &ptr, 10);

	if (ptr && *ptr)
	{
	  fprintf(stderr, "moauthd: Bad IntrospectGroup \"%s\" on line %d of \"%s\".\n", value, linenum, configfile);
	  return (false);
	}
      }
      else if ((group = getgrnam(value)) != NULL)
      {
	server->introspect_group = group->gr_gid;
      }
      else
      {
	fprintf(stderr, "moauthd: Unknown IntrospectGroup \"%s\" on line %d of \"%s\".\n", value, linenum, configfile);
	return (false);
      }
    }
    else if (!strcasecmp(line, "RegisterGroup"))
    {
      // RegisterGroup nnn
      // RegisterGroup name
      //
      // Required group membership (and thus required authentication for)
      // client registration.
      if (!value)
      {
	fprintf(stderr, "moauthd: Missing RegisterGroup on line %d of \"%s\".\n", linenum, configfile);
	return (false);
      }
      else if (isdigit(*value))
      {
	server->register_group = (gid_t)strtol(value, &ptr, 10);

	if (ptr && *ptr)
	{
	  fprintf(stderr, "moauthd: Bad RegisterGroup \"%s\" on line %d of \"%s\".\n", value, linenum, configfile);
	  return (false);
	}
      }
      else if ((group = getgrnam(value)) != NULL)
      {
	server->register_group = group->gr_gid;
      }
      else
      {
	fprintf(stderr, "moauthd: Unknown RegisterGroup \"%s\" on line %d of \"%s\".\n", value, linenum, configfile);
	return (false);
      }
    }
    else if (!strcasecmp(line, "MaxGrantLife"))
    {
      // MaxGrantLife NNN{m,h,d,w}
      //
      // Default units are seconds.  "m" is minutes, "h" is hours, "d" is days,
      // and "w" is weeks.
      int	max_grant_life;		// Maximum grant life value

      if (!value)
      {
	fprintf(stderr, "moauthd: Missing time value on line %d of \"%s\".\n", linenum, configfile);
	return (false);
      }

      if ((max_grant_life = get_seconds(value)) < 0)
      {
	fprintf(stderr, "moauthd: Unknown time value \"%s\" on line %d of \"%s\".\n", value, linenum, configfile);
	return (false);
      }

      server->max_grant_life = max_grant_life;
    }
    else if (!strcasecmp(line, "MaxTokenLife"))
    {
      // MaxTokenLife NNN{m,h,d,w}
      //
      // Default units are seconds.  "m" is minutes, "h" is hours, "d" is days,
      // and "w" is weeks.
      int	max_token_life;		// Maximum token life value

      if (!value)
      {
	fprintf(stderr, "moauthd: Missing time value on line %d of \"%s\".\n", linenum, configfile);
	return (false);
      }

      if ((max_token_life = get_seconds(value)) < 0)
      {
	fprintf(stderr, "moauthd: Unknown time value \"%s\" on line %d of \"%s\".\n", value, linenum, configfile);
	return (false);
      }

      server->max_token_life = max_token_life;
    }
    else if (!strcasecmp(line, "Option"))
    {
      // Option {[-]BasicAuth}
      if (!value)
      {
	fprintf(stderr, "moauthd: Bad Option on line %d of \"%s\".\n", linenum, configfile);
	return (false);
      }

      if (!strcasecmp(value, "BasicAuth"))
	server->options |= MOAUTHD_OPTION_BASIC_AUTH;
      else
	fprintf(stderr, "moauthd: Unknown Option %s on line %d of \"%s\".\n", value, linenum, configfile);
    }
    else if (!strcasecmp(line, "Resource"))
    {
      // Resource {public,private,shared} /remote/path /local/path
      char		*scope,		// Access scope
			*remote_path,	// Remote path
			*local_path;	// Local path
      struct stat	local_info;	// Local file info

      if (!value)
      {
	fprintf(stderr, "moauthd: Bad Resource on line %d of \"%s\".\n", linenum, configfile);
	return (false);
      }

      scope       = strtok(value, " \t");
      remote_path = strtok(NULL, " \t");
      local_path  = strtok(NULL, " \t");

      if (!scope || !remote_path || !local_path)
      {
	fprintf(stderr, "moauthd: Bad Resource on line %d of \"%s\".\n", linenum, configfile);
	return (false);
      }

      if (stat(local_path, &local_info))
      {
	fprintf(stderr, "moauthd: Unable to access Resource on line %d of \"%s\": %s\n", linenum, configfile, strerror(errno));
	return (false);
      }

      moauthdCreateResource(server, S_ISREG(local_info.st_mode) ? MOAUTHD_RESTYPE_FILE : MOAUTHD_RESTYPE_DIR, remote_path, local_path, NULL, scope);
    }
    else if (!strcasecmp(line, "ServerName"))
    {
      // ServerName hostname[:port]
      char	*portptr;		// Pointer to port in line

      if (!value)
      {
	fprintf(stderr, "moauthd: Missing server name on line %d of \"%s\".\n", linenum, configfile);
	return (false);
      }

      if ((portptr = strrchr(value, ':')) != NULL && isdigit(portptr[1] & 255))
      {
	// Extract ":port" portion...
	*portptr++   = '\0';
	server->port = atoi(portptr);
      }

      if ((server->name = strdup(value)) == NULL)
      {
        fprintf(stderr, "moauthd: Unable to allocate memory for server name on line %d of \"%s\".\n", linenum, configfile);
        return (false);
      }
    }
    else if (!strcasecmp(line, "TestPassword"))
    {
      if (value)
      {
	server->test_password = strdup(value);
      }
      else
      {
	fprintf(stderr, "moauthd: Missing password on line %d of \"%s\".\n", linenum, configfile);
	return (false);
      }
    }
    else
    {
      fprintf(stderr, "moauthd: Unknown configuration directive \"%s\" on line %d of \"%s\" ignored.\n", line, linenum, configfile);
    }
  }

  return (true);
}


//
// 'load_state()' - Load the server state.
//

static bool				// O - `true` on success, `false` on failure
load_state(moauthd_server_t *server)	// I - Server
{
  cups_file_t	*fp;			// State file
  char		line[16384],		// Line from config/state file
		*value;			// Value from config/state file
  int		linenum;		// Current line number


  if ((fp = cupsFileOpen(server->state_file, "r")) == NULL)
  {
    if (errno != ENOENT)
    {
      // Something other than the state file doesn't exist...
      fprintf(stderr, "moauthd: Unable to open state file \"%s\": %s\n", server->state_file, strerror(errno));
      return (false);
    }

    // No file means we need to generate the private key...
    server->private_key = cupsJWTMakePrivateKey(CUPS_JWA_RS256);
    return (server->private_key != NULL && moauthdSaveServer(server));
  }

  // Read lines from the state file...
  while (cupsFileGetConf(fp, line, sizeof(line), &value, &linenum))
  {
    if (!strcmp(line, "PrivateKey") && value)
      server->private_key = cupsJSONImportString(value);
    else
      fprintf(stderr, "moauthd: Unknown state directive \"%s\" on line %d of \"%s\".\n", line, linenum, server->state_file);
  }

  cupsFileClose(fp);

  return (server->private_key != NULL);
}
