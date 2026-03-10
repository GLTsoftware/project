/***************************************************************
 *
 * endProject.c
 *
 * GLT command to end the current observing project.
 * Clears the current display variables (glt:project:current:*)
 * and sets status to idle (0).  The permanent log is untouched.
 *
 * NAP, March 2026
 *
 ****************************************************************/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <hiredis/hiredis.h>

#define REDIS_SERVER "192.168.1.141"
#define REDIS_PORT   6379

/* Current display keys (must match project.c) */
#define RKEY_CURRENT_PI          "glt:project:current:pi"
#define RKEY_CURRENT_OBSERVER    "glt:project:current:observer"
#define RKEY_CURRENT_LOCATION    "glt:project:current:location"
#define RKEY_CURRENT_DESCRIPTION "glt:project:current:description"
#define RKEY_CURRENT_TYPE        "glt:project:current:type"
#define RKEY_CURRENT_RECEIVER    "glt:project:current:receiver"
#define RKEY_CURRENT_COMMENT     "glt:project:current:comment"
#define RKEY_CURRENT_STATUS      "glt:project:current:status"
#define RKEY_CURRENT_CODE        "glt:project:current:code"
#define RKEY_CURRENT_TIMESTAMP   "glt:project:current:timestamp"

/* Permanent log */
#define RKEY_PROJECT_LOG         "glt:project:log"

#define STATUS_IDLE 0

int main(int argc, char **argv) {
    /* Connect to Redis */
    redisContext *c = redisConnect(REDIS_SERVER, REDIS_PORT);
    if (c == NULL || c->err) {
        if (c) {
            fprintf(stderr, "Redis connection error: %s\n", c->errstr);
            redisFree(c);
        } else {
            fprintf(stderr, "Redis connection error: cannot allocate context\n");
        }
        return EXIT_FAILURE;
    }

    /* Check if there is an active project */
    redisReply *reply = redisCommand(c, "GET %s", RKEY_CURRENT_STATUS);
    if (reply != NULL && reply->type == REDIS_REPLY_STRING) {
        int status = atoi(reply->str);
        freeReplyObject(reply);
        if (status == STATUS_IDLE) {
            printf("No active project to end (status is already idle).\n");
            redisFree(c);
            return EXIT_SUCCESS;
        }
    } else {
        if (reply) freeReplyObject(reply);
        printf("No project found.\n");
        redisFree(c);
        return EXIT_SUCCESS;
    }

    /* Read current project code for the log entry */
    char project_code[64] = "";
    reply = redisCommand(c, "GET %s", RKEY_CURRENT_CODE);
    if (reply != NULL && reply->type == REDIS_REPLY_STRING && reply->str)
        strncpy(project_code, reply->str, sizeof(project_code) - 1);
    if (reply) freeReplyObject(reply);

    /* Log the project end event */
    time_t now = time(NULL);
    struct tm *tm_info = localtime(&now);
    char timestamp[32];
    strftime(timestamp, sizeof(timestamp), "%Y-%m-%d %H:%M:%S", tm_info);

    char logentry[256];
    snprintf(logentry, sizeof(logentry),
             "%s | code=%s | PROJECT ENDED", timestamp, project_code);
    reply = redisCommand(c, "LPUSH %s %s", RKEY_PROJECT_LOG, logentry);
    if (reply) freeReplyObject(reply);

    /* Clear all current display fields to blank strings */
    const char *keys_to_clear[] = {
        RKEY_CURRENT_PI,
        RKEY_CURRENT_OBSERVER,
        RKEY_CURRENT_LOCATION,
        RKEY_CURRENT_DESCRIPTION,
        RKEY_CURRENT_TYPE,
        RKEY_CURRENT_RECEIVER,
        RKEY_CURRENT_COMMENT,
        RKEY_CURRENT_CODE,
        RKEY_CURRENT_TIMESTAMP,
        NULL
    };

    for (int i = 0; keys_to_clear[i] != NULL; i++) {
        reply = redisCommand(c, "SET %s %s", keys_to_clear[i], "");
        if (reply == NULL)
            fprintf(stderr, "Warning: failed to clear %s\n", keys_to_clear[i]);
        else
            freeReplyObject(reply);
    }

    /* Set status to idle */
    reply = redisCommand(c, "SET %s %d", RKEY_CURRENT_STATUS, STATUS_IDLE);
    if (reply == NULL)
        fprintf(stderr, "Warning: failed to set status to idle\n");
    else
        freeReplyObject(reply);

    printf("Project %s ended at %s. Status set to idle.\n",
           project_code, timestamp);

    redisFree(c);
    return EXIT_SUCCESS;
}
