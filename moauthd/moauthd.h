//
// Header file for moauth daemon
//
// Copyright © 2017-2022 by Michael R Sweet
//
// Licensed under Apache License v2.0.  See the file "LICENSE" for more information.
//

#ifndef MOAUTHD_H
#  define MOAUTHD_H
#  include <config.h>
#  include <moauth/moauth-private.h>
#  include <stdio.h>
#  include <stdlib.h>
#  include <stdbool.h>
#  include <string.h>
#  include <ctype.h>
#  include <errno.h>
#  include <poll.h>
#  include <sys/stat.h>
#  include <cups/thread.h>


//
// Constants...
//

#  define MOAUTHD_MAX_LISTENERS	4	// Maximum number of listener sockets


//
// Types...
//

typedef struct moauthd_application_s	//// Application (Client)
{
  char	*client_id,			// Client identifier
	*redirect_uri,			// Redirection URI
	*client_name,			// Name, if any
	*client_uri,			// Web page, if any
	*logo_uri,			// Logo URI, if any
	*tos_uri;			// Terms-of-service URI, if any
} moauthd_application_t;


typedef enum moauthd_restype_e		// Resource Types
{
  MOAUTHD_RESTYPE_DIR,			// Explicit directory
  MOAUTHD_RESTYPE_USER_DIR,		// Wildcard user directory
  MOAUTHD_RESTYPE_FILE,			// Explicit file
  MOAUTHD_RESTYPE_CACHED_FILE,		// Static (cached) file
  MOAUTHD_RESTYPE_STATIC_FILE		// Static (compiled in) file
} moauthd_restype_t;


typedef struct moauthd_resource_s	// Resource
{
  moauthd_restype_t	type;		// Resource type
  char			*remote_path,	// Remote path
			*local_path,	// Local path
			*content_type,	// MIME media type, if any
			*scope;		// Access scope
  gid_t			scope_gid;	// Scope group ID
  size_t		remote_len;	// Length of remote path
  const void		*data;		// Data (static files)
  size_t		length;		// Length (static files)
} moauthd_resource_t;


typedef enum moauthd_toktype_e		// Token Type
{
  MOAUTHD_TOKTYPE_ACCESS,		// Access token
  MOAUTHD_TOKTYPE_GRANT,		// Grant token
  MOAUTHD_TOKTYPE_RENEWAL		// Renewal token
} moauthd_toktype_t;


typedef struct moauthd_token_s		// Token
{
  moauthd_toktype_t	type;		// Type of token
  char			*token,		// Token string
			*challenge,	// Challenge string
			*user;		// Authenticated user
  moauthd_application_t	*application;	// Client ID/redirection URI used
  char			*scopes;	// Scope(s) string
  cups_array_t		*scopes_array;	// Scope(s) array
  uid_t			uid;		// Authenticated UID
  gid_t			gid;		// Primary group ID
  time_t		created;	// When the token was created
  time_t		expires;	// When the token expires
} moauthd_token_t;


typedef enum moauthd_loglevel_e		// Log Levels
{
  MOAUTHD_LOGLEVEL_ERROR,		// Error messages only
  MOAUTHD_LOGLEVEL_INFO,		// Errors and informational messages
  MOAUTHD_LOGLEVEL_DEBUG,		// All messages
} moauthd_loglevel_t;


typedef enum moauthd_option_e		// Server options
{
  MOAUTHD_OPTION_BASIC_AUTH = 1		// Enable Basic authentication as a backup
} moauthd_option_t;


typedef struct moauthd_server_s		// Server
{
  char		*name;			// Server hostname
  int		port;			// Server port
  char		*state_file;		// State file
  int		log_file;		// Log file descriptor
  moauthd_loglevel_t log_level;		// Log level
  char		*auth_service;		// PAM authentication service
  int		num_clients;		// Number of clients served
  int		num_listeners;		// Number of listener sockets
  struct pollfd	listeners[MOAUTHD_MAX_LISTENERS];
					// Listener sockets
  unsigned	options;		// Server option flags
  gid_t		introspect_group,	// Group allowed to introspect tokens
		register_group;		// Group allowed to register clients
  int		max_grant_life,		// Maximum life of a grant in seconds
		max_token_life;		// Maximum life of a token in seconds
  int		num_tokens;		// Number of tokens issued
  char		*secret;		// Secret value string for this invocation
  cups_array_t	*applications;		// "Registered" applications
  pthread_mutex_t applications_lock;	// Mutex for applications array
  cups_array_t	*resources;		// Resources that are shared
  pthread_rwlock_t resources_lock;	// R/W lock for resources array
  cups_array_t	*tokens;		// Tokens that have been issued
  pthread_rwlock_t tokens_lock;		// R/W lock for tokens array
  time_t	start_time;		// Startup time
  cups_json_t	*private_key;		// JWT private key
  char		*public_key;		// JWT public key
  char		*test_password;		// Testing password
  char		*metadata;		// JSON metadata
} moauthd_server_t;


typedef struct moauthd_client_s		// Client Information
{
  int		number;			// Client number
  moauthd_server_t *server;		// Server
  http_t	*http;			// HTTP connection
  http_state_t	request_method;		// Request method
  char		path_info[4096],	// Request path/URI
		*query_string;		// Query string (if any)
  char		remote_host[256],	// Remote hostname
		remote_user[256];	// Authenticated username, if any
  uid_t		remote_uid;		// Authenticated UID, if any
  int		num_remote_gids;	// Number of remote groups, if any
#ifdef __APPLE__
  int		remote_gids[100];	// Authenticated groups, if any
#else
  gid_t		remote_gids[100];	// Authenticated groups, if any
#endif // __APPLE__
  moauthd_token_t *remote_token;	// Access token used, if any
} moauthd_client_t;


//
// Functions...
//

extern moauthd_application_t *moauthdAddApplication(moauthd_server_t *server, const char *client_id, const char *redirect_uri, const char *client_name, const char *client_uri, const char *logo_uri, const char *tos_uri);
extern bool		moauthdAuthenticateUser(moauthd_client_t *client, const char *username, const char *password);
extern moauthd_client_t	*moauthdCreateClient(moauthd_server_t *server, int fd);
extern moauthd_resource_t *moauthdCreateResource(moauthd_server_t *server, moauthd_restype_t type, const char *remote_path, const char *local_path, const char *content_type, const char *scope);
extern moauthd_server_t	*moauthdCreateServer(const char *configfile, const char *statefile, int verbosity);
extern moauthd_token_t	*moauthdCreateToken(moauthd_server_t *server, moauthd_toktype_t type, moauthd_application_t *application, const char *user, const char *scopes);
extern void		moauthdDeleteClient(moauthd_client_t *client);
extern void		moauthdDeleteServer(moauthd_server_t *server);
extern void		moauthdDeleteToken(moauthd_server_t *server, moauthd_token_t *token);
extern moauthd_application_t *moauthdFindApplication(moauthd_server_t *server, const char *client_id, const char *redirect_uri);
extern moauthd_resource_t *moauthdFindResource(moauthd_server_t *server, const char *path_info, char *name, size_t namesize, struct stat *info);
extern moauthd_token_t	*moauthdFindToken(moauthd_server_t *server, const char *token_id);
extern http_status_t	moauthdGetFile(moauthd_client_t *client);
extern void		moauthdHTMLFooter(moauthd_client_t *client);
extern void		moauthdHTMLHeader(moauthd_client_t *client, const char *title);
extern void		moauthdHTMLPrintf(moauthd_client_t *client, const char *format, ...) __attribute__((__format__(__printf__, 2, 3)));
extern void		moauthdLogc(moauthd_client_t *client, moauthd_loglevel_t level, const char *message, ...) __attribute__((__format__(__printf__, 3, 4)));
extern void		moauthdLogs(moauthd_server_t *server, moauthd_loglevel_t level, const char *message, ...) __attribute__((__format__(__printf__, 3, 4)));
extern bool		moauthdRespondClient(moauthd_client_t *client, http_status_t code, const char *type, const char *uri, time_t mtime, size_t length);
extern void		*moauthdRunClient(moauthd_client_t *client);
extern int		moauthdRunServer(moauthd_server_t *server);
extern bool		moauthdSaveServer(moauthd_server_t *server);

#endif // !MOAUTHD_H
