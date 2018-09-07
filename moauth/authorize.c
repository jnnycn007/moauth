/*
 * Authorization support for moauth library
 *
 * Copyright © 2017-2018 by Michael R Sweet
 *
 * Licensed under Apache License v2.0.  See the file "LICENSE" for more information.
 */

#include <config.h>
#include "moauth-private.h"

#ifdef __APPLE__
#  include <CoreFoundation/CoreFoundation.h>
#  include <CoreServices/CoreServices.h>
#else
#  include <spawn.h>
extern char **environ;
#endif /* __APPLE__ */


/*
 * 'moauthAuthorize()' - Open the authorization web page for an OAuth server.
 *
 * This function returns as soon as the web page has been opened.
 *
 * The "code_verifier" string is transformed using the S256 method.
 */

int					/* O - 1 on success, 0 on error */
moauthAuthorize(
    moauth_t   *server,			/* I - OAuth server connection */
    const char *redirect_uri,		/* I - Redirection URI */
    const char *client_id,		/* I - Client identifier */
    const char *state,			/* I - Client state string or @code NULL@ */
    const char *code_verifier,		/* I - Code verifier string or @code NULL@ if none */
    const char *scope)			/* I - Scope to request or `NULL` */
{
  char		scheme[32],		/* URI scheme */
		userpass[256],		/* Username:password (unused) */
		host[256],		/* Host */
		resource[256];		/* Resource path */
  int		port;			/* Port number */
  char		url[2048];		/* URL for authorization page */
  int		status = 1;		/* Return status */
  unsigned char	sha256[32];		/* SHA-256 hash of code verifier */
  char		code_challenge[64];	/* Hashed code verifier string */
  int		num_vars = 0;		/* Number of form variables */
  cups_option_t	*vars = NULL;		/* Form variables */
  char		*formdata;		/* Encoded form data */


 /*
  * Range check input...
  */

  if (!server || !redirect_uri || !client_id)
  {
    if (server)
      snprintf(server->error, sizeof(server->error), "Bad arguments to function.");

    return (0);
  }

 /*
  * Make the authorization URL using the information supplied...
  */

  httpSeparateURI(HTTP_URI_CODING_ALL, server->authorization_endpoint, scheme, sizeof(scheme), userpass, sizeof(userpass), host, sizeof(host), &port, resource, sizeof(resource));

  num_vars = cupsAddOption("response_type", "code", num_vars, &vars);
  num_vars = cupsAddOption("client_id", client_id, num_vars, &vars);
  num_vars = cupsAddOption("redirect_uri", redirect_uri, num_vars, &vars);

  if (scope)
    num_vars = cupsAddOption("scope", scope, num_vars, &vars);

  if (state)
    num_vars = cupsAddOption("state", state, num_vars, &vars);

  if (code_verifier)
  {
    cupsHashData("sha2-256", code_verifier, strlen(code_verifier), sha256, sizeof(sha256));
    httpEncode64_2(code_challenge, (int)sizeof(code_challenge), (char *)sha256, (int)sizeof(sha256));
    num_vars = cupsAddOption("code_challenge", code_challenge, num_vars, &vars);
  }

  formdata = _moauthFormEncode(num_vars, vars);

  if (snprintf(url, sizeof(url), "https://%s:%d%s%s%s", host, port, resource, strchr(resource, '?') != NULL ? "&" : "?", formdata) >= sizeof(url))
  {
   /*
    * URL is too long...
    */

    snprintf(server->error, sizeof(server->error), "Unable to create authorization URL.");

    free(formdata);
    cupsFreeOptions(num_vars, vars);

    return (0);
  }

  free(formdata);
  cupsFreeOptions(num_vars, vars);

#ifdef __APPLE__
  CFURLRef cfurl = CFURLCreateWithBytes(kCFAllocatorDefault, (const UInt8 *)url, (CFIndex)strlen(url), kCFStringEncodingASCII, NULL);

  if (cfurl)
  {
    if (LSOpenCFURLRef(cfurl, NULL) != noErr)
    {
      snprintf(server->error, sizeof(server->error), "Unable to open authorization URL.");
      status = 0;			/* Couldn't open URL */
    }

    CFRelease(cfurl);
  }
  else
  {
    snprintf(server->error, sizeof(server->error), "Unable to create authorization URL.");
    status = 0;				/* Couldn't create CFURL object */
  }

#else
  pid_t		pid = 0;		/* Process ID */
  int		estatus;		/* Exit status */
  static char * const xdg_open_argv[] =	/* xdg-open arguments */
  {
    "xdg-open",
    NULL,
    NULL
  };

  xdg_open_argv[1] = url;

  if (posix_spawnp(&pid, "xdg-open", NULL, NULL, xdg_open_argv, environ))
    status = 0;				/* Couldn't run xdg-open */
  else if (waitpid(pid, &estatus, 0))
    status = 0;				/* Couldn't get exit status */
  else if (estatus)
    status = 0;				/* Non-zero exit status */

  if (!status)
    snprintf(server->error, sizeof(server->error), "Unable to open authorization URL.");
#endif /* __APPLE__ */

  return (status);
}
