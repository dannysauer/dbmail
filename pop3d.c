/* $Id$
* (c) 2000-2002 IC&S, The Netherlands
*
* pop3d.c
*
* main prg for pop3 daemon
*/

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <signal.h>
#include <errno.h>
#include "imap4.h"
#include "server.h"
#include "debug.h"
#include "misc.h"
#include "config.h"
#include "clientinfo.h"
#include "pop3.h"
#ifdef PROC_TITLES
#include "proctitleutils.h"
#endif


#define PNAME "dbmail/pop3d"

/* server timeout error */
#define POP_TIMEOUT_MSG "-ERR I'm leaving, you're tooo slow"


char *configFile = "/etc/dbmail.conf";

/* set up database login data */
extern field_t _db_host;
extern field_t _db_db;
extern field_t _db_user;
extern field_t _db_pass;

int error_count = 0;

void SetConfigItems(serverConfig_t *config, struct list *items);
void Daemonize();
int SetMainSigHandler();
void MainSigHandler(int sig, siginfo_t *info, void *data);


int pop_before_smtp = 0;
int mainRestart = 0;
int mainStop = 0;

int state; 					/* current pop state */
char *username=NULL, *password=NULL;
struct session curr_session;
char *myhostname;
char *apop_stamp;
char *timeout_setting;

#ifdef PROC_TITLES
int main(int argc, char *argv[], char **envp)
#else
     int main(int argc, char *argv[])
#endif
{
  serverConfig_t config;
  struct list popItems, sysItems;
  int result, status;
  pid_t pid;

  openlog(PNAME, LOG_PID, LOG_MAIL);

  if (argc >= 2 && strcmp(argv[1], "-f") == 0)
    {
      if (!argv[2])
	trace(TRACE_FATAL,"main(): no file specified for -f option. Fatal.");

      configFile = argv[2];
    }

  SetMainSigHandler();
  Daemonize();
  result = 0;

  do
    {
      mainStop = 0;
      mainRestart = 0;

      trace(TRACE_DEBUG, "main(): reading config");
#ifdef PROC_TITLES
      init_set_proc_title(argc, argv, envp, PNAME);
      set_proc_title("%s", "Idle");
#endif

      ReadConfig("POP", configFile, &popItems);
      ReadConfig("DBMAIL", configFile, &sysItems);
      SetConfigItems(&config, &popItems);
      SetTraceLevel(&popItems);
      GetDBParams(_db_host, _db_db, _db_user, _db_pass, &sysItems);

      config.ClientHandler = pop3_handle_connection;
      config.timeoutMsg = POP_TIMEOUT_MSG;

      CreateSocket(&config);
      trace(TRACE_DEBUG, "main(): socket created, starting server");

      switch ( (pid = fork()) )
	{
	case -1:
	  close(config.listenSocket);
	  trace(TRACE_FATAL, "main(): fork failed [%s]", strerror(errno));

	case 0:
	  /* child process */
	  drop_priviledges(config.serverUser, config.serverGroup);
	  result = StartServer(&config);




	  trace(TRACE_INFO, "main(): server done, exit.");
	  exit(result);

	default:
	  /* parent process, wait for child to exit */
	  while (waitpid(pid, &status, WNOHANG|WUNTRACED) == 0)
	    {
	      if (mainStop)
		kill(pid, SIGTERM);

	      if (mainRestart)
		kill(pid, SIGHUP);

	      sleep(2);
	    }

	  if (WIFEXITED(status))
	    {
	      /* child process terminated neatly */
	      result = WEXITSTATUS(status);
	      trace(TRACE_DEBUG, "main(): server has exited, exit status [%d]", result);
	    }
	  else
	    {
	      /* child stopped or signaled, don't like */
	      /* make sure it is dead */
	      trace(TRACE_DEBUG, "main(): server has not exited normally. Killing..");

	      kill(pid, SIGKILL);
	      result = 0;
	    }
	}

      list_freelist(&popItems.start);
      list_freelist(&sysItems.start);
      close(config.listenSocket);

    } while (result == 1 && !mainStop) ; /* 1 means reread-config and restart */

  trace(TRACE_INFO, "main(): exit");
  return 0;
}


void MainSigHandler(int sig, siginfo_t *info, void *data)
{
  trace(TRACE_DEBUG, "MainSigHandler(): got signal [%d]", sig);

  if (sig == SIGHUP)
    mainRestart = 1;
  else
    mainStop = 1;
}


void Daemonize()
{
  if (fork())
    exit(0);
  setsid();

  if (fork())
    exit(0);
}


int SetMainSigHandler()
{
  struct sigaction act;

  /* init & install signal handlers */
  memset(&act, 0, sizeof(act));

  act.sa_sigaction = MainSigHandler;
  sigemptyset(&act.sa_mask);
  act.sa_flags = SA_SIGINFO;

  sigaction(SIGINT, &act, 0);
  sigaction(SIGQUIT, &act, 0);
  sigaction(SIGTERM, &act, 0);
  sigaction(SIGHUP, &act, 0);

  return 0;
}


void SetConfigItems(serverConfig_t *config, struct list *items)
{
  field_t val;

  /* read items: NCHILDREN */
  GetConfigValue("NCHILDREN", items, val);
  if (strlen(val) == 0)
    trace(TRACE_FATAL, "SetConfigItems(): no value for NCHILDREN in config file");

  if ( (config->nChildren = atoi(val)) <= 0)
    trace(TRACE_FATAL, "SetConfigItems(): value for NCHILDREN is invalid: [%d]", config->nChildren);

  trace(TRACE_DEBUG, "SetConfigItems(): server will create  [%d] children", config->nChildren);


  /* read items: MAXCONNECTS */
  GetConfigValue("MAXCONNECTS", items, val);
  if (strlen(val) == 0)
    trace(TRACE_FATAL, "SetConfigItems(): no value for MAXCONNECTS in config file");

  if ( (config->childMaxConnect = atoi(val)) <= 0)
    trace(TRACE_FATAL, "SetConfigItems(): value for MAXCONNECTS is invalid: [%d]", config->childMaxConnect);

  trace(TRACE_DEBUG, "SetConfigItems(): children will make max. [%d] connections", config->childMaxConnect);


  /* read items: TIMEOUT */
  GetConfigValue("TIMEOUT", items, val);
  if (strlen(val) == 0)
    {
      trace(TRACE_DEBUG, "SetConfigItems(): no value for TIMEOUT in config file");
      config->timeout = 0;
    }
  else if ( (config->timeout = atoi(val)) <= 30)
    trace(TRACE_FATAL, "SetConfigItems(): value for TIMEOUT is invalid: [%d]", config->timeout);

  trace(TRACE_DEBUG, "SetConfigItems(): timeout [%d] seconds", config->timeout);


  /* read items: PORT */
  GetConfigValue("PORT", items, val);
  if (strlen(val) == 0)
    trace(TRACE_FATAL, "SetConfigItems(): no value for PORT in config file");

  if ( (config->port = atoi(val)) <= 0)
    trace(TRACE_FATAL, "SetConfigItems(): value for PORT is invalid: [%d]", config->port);

  trace(TRACE_DEBUG, "SetConfigItems(): binding to PORT [%d]", config->port);


  /* read items: BINDIP */
  GetConfigValue("BINDIP", items, val);
  if (strlen(val) == 0)
    trace(TRACE_FATAL, "SetConfigItems(): no value for BINDIP in config file");

  strncpy(config->ip, val, IPLEN);
  config->ip[IPLEN-1] = '\0';

  trace(TRACE_DEBUG, "SetConfigItems(): binding to IP [%s]", config->ip);


  /* read items: RESOLVE_IP */
  GetConfigValue("RESOLVE_IP", items, val);
  if (strlen(val) == 0)
    trace(TRACE_DEBUG, "SetConfigItems(): no value for RESOLVE_IP in config file");

  config->resolveIP = (strcasecmp(val, "yes") == 0);

  trace(TRACE_DEBUG, "SetConfigItems(): %sresolving client IP", config->resolveIP ? "" : "not ");


  /* read items: IMAP-BEFORE-SMTP */
  GetConfigValue("POP_BEFORE_SMTP", items, val);
  if (strlen(val) == 0)
    trace(TRACE_DEBUG, "SetConfigItems(): no value for POP_BEFORE_SMTP  in config file");

  pop_before_smtp = (strcasecmp(val, "yes") == 0);

  trace(TRACE_DEBUG, "SetConfigItems(): %s POP-before-SMTP",
	pop_before_smtp ? "Enabling" : "Disabling");


  /* read items: EFFECTIVE-USER */
  GetConfigValue("EFFECTIVE_USER", items, val);
  if (strlen(val) == 0)
    trace(TRACE_FATAL, "SetConfigItems(): no value for EFFECTIVE_USER in config file");

  strncpy(config->serverUser, val, FIELDLEN);
  config->serverUser[FIELDLEN-1] = '\0';

  trace(TRACE_DEBUG, "SetConfigItems(): effective user shall be [%s]", config->serverUser);


  /* read items: EFFECTIVE-GROUP */
  GetConfigValue("EFFECTIVE_GROUP", items, val);
  if (strlen(val) == 0)
    trace(TRACE_FATAL, "SetConfigItems(): no value for EFFECTIVE_GROUP in config file");

  strncpy(config->serverGroup, val, FIELDLEN);
  config->serverGroup[FIELDLEN-1] = '\0';

  trace(TRACE_DEBUG, "SetConfigItems(): effective group shall be [%s]", config->serverGroup);



}
