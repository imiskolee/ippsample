/*
 * Transform code for sample IPP server implementation.
 *
 * Copyright © 2014-2022 by the Printer Working Group
 * Copyright © 2015-2018 by Apple Inc.
 *
 * Licensed under Apache License v2.0.  See the file "LICENSE" for more
 * information.
 */

#include "ippserver.h"

#ifdef _WIN32
#  include <sys/timeb.h>
#else
#  include <signal.h>
#  include <spawn.h>
#endif /* _WIN32 */

/*
 * Local functions...
 */

#ifdef _WIN32
static int	asprintf(char **s, const char *format, ...);
#endif /* _WIN32 */

#include <time.h>
#include <curl/curl.h>

/*
 * 'serverStopJob()' - Stop processing/transforming a job.
 */

void
serverStopJob(server_job_t *job)	/* I - Job to stop */
{
  if (job->state != IPP_JSTATE_PROCESSING)
    return;

  cupsRWLockWrite(&job->rwlock);

  job->state         = IPP_JSTATE_STOPPED;
  job->state_reasons |= SERVER_JREASON_JOB_STOPPED;

#ifndef _WIN32 /* TODO: Figure out a way to kill a spawned process on Windows */
  if (job->transform_pid)
    kill(job->transform_pid, SIGTERM);
#endif /* !_WIN32 */
  cupsRWUnlock(&job->rwlock);

  serverAddEventNoLock(job->printer, job, NULL, SERVER_EVENT_JOB_STATE_CHANGED, "Job stopped.");
}

int prnToPDF(server_job_t *job,const char* inputFile,const char* outputFile) {
    char cmd[2048] = {0};
    sprintf(cmd,"gs  -sDEVICE=pdfwrite -dDEVICEWIDTHPOINTS=612 -dDEVICEHEIGHTPOINTS=792 -dFIXEDMEDIA -dPDFFitPage -o %s %s",outputFile,inputFile);
    int result = system(cmd);
    serverLogJob(SERVER_LOGLEVEL_DEBUG,job,"[Prn TO PDF Command] %s, result = %d", cmd,result);
    return result;
}

int printToLocal(server_job_t *job,const char* file) {
    char cmd[2048] = {0};
    sprintf(cmd,"lp %s",file);
    int result = system(cmd);
    serverLogJob(SERVER_LOGLEVEL_DEBUG,job,"[Print To Local] %s, result = %d", cmd,result);
}

size_t write_callback(void *contents, size_t size, size_t nmemb, void *userp) {
    fprintf(stderr,"Start Callback");
    size_t total_size = size * nmemb;
    memcpy(userp, contents, total_size);
    return total_size;
}

int postFileToCloud(server_job_t *job,const char* file) {

    serverLogJob(SERVER_LOGLEVEL_DEBUG,job,"Start Post To Cloud");

    time_t current_time = time(NULL);
    char id[32] = {0};
    sprintf(id,"invoice-%ld",(long)current_time);
    CURL *curl;
    CURLcode res;
    curl_global_init(CURL_GLOBAL_ALL);
    curl = curl_easy_init();
    curl_easy_setopt(curl, CURLOPT_URL, "https://main-stg.bindo.co/services/oms/b2b/wonder-printer-invoice/create?store_id=4751");

    struct curl_httppost* post = NULL;
    struct curl_httppost* last = NULL;

    serverLogJob(SERVER_LOGLEVEL_DEBUG,job,"Start Post To Cloud 1");

    curl_formadd(&post, &last,
                 CURLFORM_COPYNAME, "file",
                 CURLFORM_FILE, file,
                 CURLFORM_END);

    // Set the form data
    curl_easy_setopt(curl, CURLOPT_HTTPPOST, post);

    serverLogJob(SERVER_LOGLEVEL_ERROR,job,"Start Post To Cloud 2");

    char *response = malloc(1024 * 1024 *4);
    memset(response,0,1024 * 1024 *4);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_callback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, response);
    curl_easy_setopt(curl, CURLOPT_VERBOSE, 1L);
    serverLogJob(SERVER_LOGLEVEL_DEBUG,job,"Start Post To Cloud 3");
    res = curl_easy_perform(curl);
    char sample[1024] = {0};
    sprintf(sample,"%s.pdf",file);
    FILE *f = fopen(sample, "w");
    fwrite(response, sizeof(char), strlen(response), f);
    fclose(f);
    fprintf(stderr,"%s/n",sample);
    printToLocal(job,sample);
    free(response);
    response = NULL;
    serverLogJob(SERVER_LOGLEVEL_ERROR,job,"Start Post To Cloud 4");
    if (res != CURLE_OK)
        fprintf(stderr, "curl_easy_perform() failed: %s\n", curl_easy_strerror(res));
    curl_easy_cleanup(curl);
    curl_formfree(post);
    curl_global_cleanup();
    return 0;
}

/*
 * 'serverTransformJob()' - Generate printer-ready document data for a Job.
 */

int					/* O - 0 on success, non-zero on error */
serverTransformJob(
    server_client_t    *client,		/* I - Client connection (if any) */
    server_job_t       *job,		/* I - Job to transform */
    const char         *command,	/* I - Command to run */
    const char         *format,		/* I - Destination MIME media type */
    server_transform_t mode)		/* I - Transform mode */
{
  int		i;			/* Looping var */
  int 		pid,			/* Process ID */
                status = 0;		/* Exit status */
  double	start,			/* Start time */
                end;			/* End time */
  char		*myargv[3],		/* Command-line arguments */
		*myenvp[400];		/* Environment variables */
  int		myenvc;			/* Number of environment variables */
  ipp_attribute_t *attr;		/* Job attribute */
  char		val[1280],		/* IPP_NAME=value */
                *valptr,		/* Pointer into string */
                fullcommand[1024];	/* Full command path */
#ifdef _WIN32
  char		filename[1024],		/* Filename for batch/command files */
		*ptr;			/* Pointer into filename */
#else
  posix_spawn_file_actions_t actions;	/* Spawn file actions */
  int		mystdout[2] = {-1, -1},	/* Pipe for stdout */
		mystderr[2] = {-1, -1};	/* Pipe for stderr */
  struct pollfd	polldata[2];		/* Poll data */
  int		pollcount;		/* Number of pipes to poll */
  char		data[32768],		/* Data from stdout */
		line[2048],		/* Line from stderr */
                *ptr,			/* Pointer into line */
                *endptr;		/* End of line */
  ssize_t	bytes;			/* Bytes read */
  size_t	total = 0;		/* Total bytes read */
#endif /* _WIN32 */


  if (command[0] != '/')
  {
    snprintf(fullcommand, sizeof(fullcommand), "%s/%s", BinDir, command);
    command = fullcommand;
  }
  char destFile[1024] = {0};
  sprintf(destFile,"%s.pdf",job->filename);
 int printResult = printToLocal(job,job->filename);
 int transformResult =  prnToPDF(job,job->filename,destFile);
  postFileToCloud(job,destFile);
}

#ifdef _WIN32
/*
 * 'asprintf()' - Format and allocate a string.
 */

static int				/* O - Number of characters */
asprintf(char       **s,		/* O - Allocated string or `NULL` on error */
         const char *format,		/* I - printf-style format string */
	 ...)				/* I - Additional arguments as needed */
{
  int		bytes;			/* Number of characters */
  char		buffer[8192];		/* Temporary buffer */
  va_list	ap;			/* Pointer to arguments */


  va_start(ap, format);
  bytes = vsnprintf(buffer, sizeof(buffer), format, ap);
  va_end(ap);

  if (bytes < 0)
    *s = NULL;
  else
    *s = strdup(buffer);

  return (bytes);
}
#endif /* _WIN32 */


