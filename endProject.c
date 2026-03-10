/***************************************************************
 *
 * endProject.c
 *
 * GLT command to end the current observing project.
 * Clears the project info in Redis so the monitor displays
 * blanks, and sets the project status to idle (0).
 * The historical log entries are preserved.
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

/* Redis keys (must match project.c) */
#define RKEY_PROJECT_PI          "glt:project:pi"
#define RKEY_PROJECT_OBSERVER    "glt:project:observer"
#define RKEY_PROJECT_LOCATION    "glt:project:location"
#define RKEY_PROJECT_DESCRIPTION "glt:project:description"
#define RKEY_PROJECT_TYPE        "glt:project:type"
#define RKEY_PROJECT_RECEIVER    "glt:project:receiver"
#define RKEY_PROJECT_COMMENT     "glt:project:comment"
#define RKEY_PROJECT_STATUS      "glt:project:status"
#define RKEY_PROJECT_CODE        "glt:project:code"
#define RKEY_PROJECT_TIMESTAMP   "glt:project:timestamp"
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
    redisReply *reply = redisCommand(c, "GET %s", RKEY_PROJECT_STATUS);
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

    /* Read current project code for the log entry before clearing */
    char project_code[64] = "";
    reply = redisCommand(c, "GET %s", RKEY_PROJECT_CODE);
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

    /* Clear all display fields to blank strings */
    const char *keys_to_clear[] = {
        RKEY_PROJECT_PI,
        RKEY_PROJECT_OBSERVER,
        RKEY_PROJECT_LOCATION,
        RKEY_PROJECT_DESCRIPTION,
        RKEY_PROJECT_TYPE,
        RKEY_PROJECT_RECEIVER,
        RKEY_PROJECT_COMMENT,
        RKEY_PROJECT_CODE,
        RKEY_PROJECT_TIMESTAMP,
        NULL
    };

    for (int i = 0; keys_to_clear[i] != NULL; i++) {
        reply = redisCommand(c, "SET %s %s", keys_to_clear[i], "");
        if (reply == NULL) {
            fprintf(stderr, "Warning: failed to clear %s\n", keys_to_clear[i]);
        } else {
            freeReplyObject(reply);
        }
    }

    /* Set status to idle */
    reply = redisCommand(c, "SET %s %d", RKEY_PROJECT_STATUS, STATUS_IDLE);
    if (reply == NULL) {
        fprintf(stderr, "Warning: failed to set status to idle\n");
    } else {
        freeReplyObject(reply);
    }

    printf("Project %s ended at %s. Status set to idle.\n",
           project_code, timestamp);

    redisFree(c);
    return EXIT_SUCCESS;
}
