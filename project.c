/***************************************************************
 *
 * project.c
 *
 * GLT command for creating/revising an observing project definition.
 *
 * Two Redis namespaces are used:
 *
 *   glt:project:log          - permanent append-only list of all project
 *                              start/revise events (for history and for
 *                              deriving the next sequence number).
 *
 *   glt:project:current:*   - current display variables read by gltmonitor.
 *                              Written here on project start/revise;
 *                              cleared by endProject.
 *
 * NAP, March 2026
 *
 ****************************************************************/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <getopt.h>
#include <hiredis/hiredis.h>

#define REDIS_SERVER "192.168.1.141"
#define REDIS_PORT   6379

#define MAX_PI_LEN          64
#define MAX_OBSERVER_LEN    64
#define MAX_LOCATION_LEN   128
#define MAX_DESC_LEN       512
#define MAX_TYPE_LEN        32
#define MAX_COMMENT_LEN    256
#define MAX_CODE_LEN        32

/* Current display keys (read by gltmonitor, cleared by endProject) */
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

/* Permanent log: one hash per project, plus a sorted set index */
#define RKEY_PROJECT_LOG_PREFIX  "glt:project:log:"
#define RKEY_PROJECT_INDEX       "glt:project:index"

/* Status codes */
#define STATUS_LOCKOUT -1
#define STATUS_IDLE     0
#define STATUS_ACTIVE   1

/* Valid project types */
static const char *valid_types[] = {
    "vlbi", "single-dish", "engineering", "pointing",
    "holography", "lockout", NULL
};

static void usage(const char *progname) {
    fprintf(stderr,
        "Usage: %s [OPTIONS]\n"
        "\n"
        "Create or revise a GLT observing project definition.\n"
        "If no options are given, interactive mode prompts for each field.\n"
        "\n"
        "Options:\n"
        "  -p, --pi NAME           PI name\n"
        "  -o, --observer NAME     Observer name\n"
        "  -l, --location LOC      Observer's operating location\n"
        "  -d, --description DESC  Project description (max 512 chars)\n"
        "  -t, --type TYPE         Project type: vlbi, single-dish, engineering,\n"
        "                          pointing, holography, lockout\n"
        "  -R, --receiver RX       Receiver (integer: 86, 230, or 345)\n"
        "  -c, --comment TEXT      Operator's phone/email\n"
        "  -r, --revise            Revise existing project (only given fields updated)\n"
        "  -h, --help              Show this help message\n"
        "\n"
        "Examples:\n"
        "  %s -p \"J. Doe\" -o \"A. Smith\" -l \"Summit\" -d \"VLBI obs\" -t vlbi -R 230 -c \"x1234\"\n"
        "  %s -r -d \"Updated description\" -c \"new-email@example.com\"\n"
        "  %s          (interactive mode)\n",
        progname, progname, progname, progname);
}

/* Validate project type against allowed list */
static int validate_type(const char *type) {
    for (int i = 0; valid_types[i] != NULL; i++) {
        if (strcasecmp(type, valid_types[i]) == 0)
            return 1;
    }
    return 0;
}

/* Validate receiver value */
static int validate_receiver(int rx) {
    return (rx == 86 || rx == 230 || rx == 345);
}

/* Determine status code from project type */
static int status_from_type(const char *type) {
    if (strcasecmp(type, "lockout") == 0)
        return STATUS_LOCKOUT;
    return STATUS_ACTIVE;
}

/* Connect to Redis, exit on failure */
static redisContext *redis_connect(void) {
    redisContext *c = redisConnect(REDIS_SERVER, REDIS_PORT);
    if (c == NULL || c->err) {
        if (c) {
            fprintf(stderr, "Redis connection error: %s\n", c->errstr);
            redisFree(c);
        } else {
            fprintf(stderr, "Redis connection error: cannot allocate context\n");
        }
        exit(EXIT_FAILURE);
    }
    return c;
}

/* Helper: SET a redis string key; returns 0 on success */
static int redis_set(redisContext *c, const char *key, const char *value) {
    redisReply *reply = redisCommand(c, "SET %s %s", key, value);
    if (reply == NULL) {
        fprintf(stderr, "Redis SET error for key %s\n", key);
        return -1;
    }
    freeReplyObject(reply);
    return 0;
}

/* Helper: GET a redis string key; caller must free returned string (strdup'd).
   Returns NULL if key does not exist or is empty. */
static char *redis_get(redisContext *c, const char *key) {
    redisReply *reply = redisCommand(c, "GET %s", key);
    if (reply == NULL)
        return NULL;
    char *result = NULL;
    if (reply->type == REDIS_REPLY_STRING && reply->str != NULL)
        result = strdup(reply->str);
    freeReplyObject(reply);
    return result;
}

/* Generate project code: YYYY_MM_DD_SEQ.
   Queries the sorted set index for the most recently created code.
   If its date matches today, increments the sequence; otherwise starts at 1. */
static void generate_project_code(redisContext *c, char *code, size_t len) {
    time_t now = time(NULL);
    struct tm *tm = localtime(&now);
    char today[16];
    snprintf(today, sizeof(today), "%04d_%02d_%02d",
             tm->tm_year + 1900, tm->tm_mon + 1, tm->tm_mday);

    int seq = 1;

    /* Highest score (most recent timestamp) is at index -1 with ZREVRANGE */
    redisReply *reply = redisCommand(c, "ZREVRANGE %s 0 0", RKEY_PROJECT_INDEX);
    if (reply != NULL && reply->type == REDIS_REPLY_ARRAY &&
        reply->elements > 0 && reply->element[0]->str != NULL) {
        const char *last_code = reply->element[0]->str;
        if (strlen(last_code) >= 10 &&
            strncmp(last_code, today, 10) == 0) {
            const char *last_underscore = strrchr(last_code, '_');
            if (last_underscore != NULL) {
                int last_seq = atoi(last_underscore + 1);
                if (last_seq > 0)
                    seq = last_seq + 1;
            }
        }
    }
    if (reply) freeReplyObject(reply);

    snprintf(code, len, "%s_%d", today, seq);
}

/* Read a line from stdin into buf (stripping trailing newline).
   prompt is printed first. Returns 0 on success, -1 on EOF. */
static int prompt_input(const char *prompt, char *buf, size_t buflen) {
    printf("%s", prompt);
    fflush(stdout);
    if (fgets(buf, buflen, stdin) == NULL)
        return -1;
    buf[strcspn(buf, "\n")] = '\0';
    return 0;
}

/* Log the project event to Redis.
 *
 * Each project gets a hash at glt:project:log:<code> with all its fields.
 * For new projects the hash is created and the code is added to the sorted
 * set glt:project:index (scored by Unix timestamp) for ordered retrieval.
 * For revisions the hash fields are updated and revised_timestamp is set.
 */
static void log_project(redisContext *c, const char *code,
                         const char *pi, const char *observer,
                         const char *location, const char *description,
                         const char *type, int receiver,
                         const char *comment, int status,
                         const char *timestamp, time_t ts_epoch,
                         int is_new)
{
    char hashkey[64];
    snprintf(hashkey, sizeof(hashkey), "%s%s", RKEY_PROJECT_LOG_PREFIX, code);

    char receiver_str[16], status_str[16];
    snprintf(receiver_str, sizeof(receiver_str), "%d", receiver);
    snprintf(status_str,   sizeof(status_str),   "%d", status);

    redisReply *reply;

    if (is_new) {
        reply = redisCommand(c,
            "HSET %s start_timestamp %s pi %s observer %s location %s "
            "description %s type %s receiver %s comment %s status %s",
            hashkey, timestamp, pi, observer, location,
            description, type, receiver_str, comment, status_str);
        if (reply == NULL)
            fprintf(stderr, "Warning: failed to write project hash\n");
        else
            freeReplyObject(reply);

        /* Add to sorted set, scored by creation time */
        reply = redisCommand(c, "ZADD %s %ld %s",
                             RKEY_PROJECT_INDEX, (long)ts_epoch, code);
        if (reply == NULL)
            fprintf(stderr, "Warning: failed to update project index\n");
        else
            freeReplyObject(reply);
    } else {
        /* Revise: update all fields and record revision timestamp */
        reply = redisCommand(c,
            "HSET %s revised_timestamp %s pi %s observer %s location %s "
            "description %s type %s receiver %s comment %s status %s",
            hashkey, timestamp, pi, observer, location,
            description, type, receiver_str, comment, status_str);
        if (reply == NULL)
            fprintf(stderr, "Warning: failed to update project hash\n");
        else
            freeReplyObject(reply);
    }
}

int main(int argc, char **argv) {
    char pi[MAX_PI_LEN] = "";
    char observer[MAX_OBSERVER_LEN] = "";
    char location[MAX_LOCATION_LEN] = "";
    char description[MAX_DESC_LEN] = "";
    char type[MAX_TYPE_LEN] = "";
    char comment[MAX_COMMENT_LEN] = "";
    int  receiver = 0;
    int  reviseFlag = 0;

    /* Flags for which fields were given on command line */
    int piFlag = 0, observerFlag = 0, locationFlag = 0;
    int descriptionFlag = 0, typeFlag = 0, receiverFlag = 0;
    int commentFlag = 0;

    static struct option long_options[] = {
        {"pi",          required_argument, 0, 'p'},
        {"observer",    required_argument, 0, 'o'},
        {"location",    required_argument, 0, 'l'},
        {"description", required_argument, 0, 'd'},
        {"type",        required_argument, 0, 't'},
        {"receiver",    required_argument, 0, 'R'},
        {"comment",     required_argument, 0, 'c'},
        {"revise",      no_argument,       0, 'r'},
        {"help",        no_argument,       0, 'h'},
        {0, 0, 0, 0}
    };

    int opt;
    while ((opt = getopt_long(argc, argv, "p:o:l:d:t:R:c:rh",
                              long_options, NULL)) != -1) {
        switch (opt) {
        case 'p':
            strncpy(pi, optarg, MAX_PI_LEN - 1);
            piFlag = 1;
            break;
        case 'o':
            strncpy(observer, optarg, MAX_OBSERVER_LEN - 1);
            observerFlag = 1;
            break;
        case 'l':
            strncpy(location, optarg, MAX_LOCATION_LEN - 1);
            locationFlag = 1;
            break;
        case 'd':
            strncpy(description, optarg, MAX_DESC_LEN - 1);
            descriptionFlag = 1;
            break;
        case 't':
            strncpy(type, optarg, MAX_TYPE_LEN - 1);
            typeFlag = 1;
            break;
        case 'R':
            receiver = atoi(optarg);
            receiverFlag = 1;
            break;
        case 'c':
            strncpy(comment, optarg, MAX_COMMENT_LEN - 1);
            commentFlag = 1;
            break;
        case 'r':
            reviseFlag = 1;
            break;
        case 'h':
            usage(argv[0]);
            return EXIT_SUCCESS;
        default:
            usage(argv[0]);
            return EXIT_FAILURE;
        }
    }

    /*
     * If no arguments given, enter interactive mode.
     * Prompt user for each required field, then proceed.
     */
    if (argc < 2) {
        printf("No arguments given. Entering interactive mode...\n");
        printf("(Use 'project --help' for command-line usage, or Ctrl-C to abort.)\n\n");

        prompt_input("Enter PI name: ", pi, sizeof(pi));
        piFlag = 1;

        prompt_input("Enter observer name: ", observer, sizeof(observer));
        observerFlag = 1;

        prompt_input("Enter operating location: ", location, sizeof(location));
        locationFlag = 1;

        prompt_input("Enter project description (max 512 chars): ",
                     description, sizeof(description));
        descriptionFlag = 1;

        char typebuf[MAX_TYPE_LEN];
        do {
            prompt_input("Enter project type (vlbi, single-dish, engineering, "
                         "pointing, holography, lockout): ",
                         typebuf, sizeof(typebuf));
            if (validate_type(typebuf)) {
                strncpy(type, typebuf, MAX_TYPE_LEN - 1);
                typeFlag = 1;
            } else {
                printf("Invalid type. Please try again.\n");
            }
        } while (!typeFlag);

        char rxbuf[16];
        do {
            prompt_input("Enter receiver (86, 230, or 345): ",
                         rxbuf, sizeof(rxbuf));
            receiver = atoi(rxbuf);
            if (validate_receiver(receiver)) {
                receiverFlag = 1;
            } else {
                printf("Invalid receiver. Please enter 86, 230, or 345.\n");
            }
        } while (!receiverFlag);

        prompt_input("Enter comment (phone/email): ", comment, sizeof(comment));
        commentFlag = 1;
    }

    /* Validate inputs that were provided */
    if (typeFlag && !validate_type(type)) {
        fprintf(stderr, "Error: invalid project type '%s'.\n"
                "Valid types: vlbi, single-dish, engineering, pointing, "
                "holography, lockout\n", type);
        return EXIT_FAILURE;
    }
    if (receiverFlag && !validate_receiver(receiver)) {
        fprintf(stderr, "Error: invalid receiver %d. Must be 86, 230, or 345.\n",
                receiver);
        return EXIT_FAILURE;
    }

    /* For a new project (not revise), all fields are required */
    if (!reviseFlag) {
        if (!piFlag || !observerFlag || !locationFlag ||
            !descriptionFlag || !typeFlag || !receiverFlag) {
            fprintf(stderr,
                "Error: for a new project, all fields are required:\n"
                "  -p (PI), -o (observer), -l (location), -d (description),\n"
                "  -t (type), -R (receiver).\n"
                "  -c (comment) is optional.\n"
                "Or run with no arguments for interactive mode.\n");
            return EXIT_FAILURE;
        }
    }

    /* For revise, at least one field must be given */
    if (reviseFlag) {
        if (!piFlag && !observerFlag && !locationFlag &&
            !descriptionFlag && !typeFlag && !receiverFlag && !commentFlag) {
            fprintf(stderr,
                "Error: -r (revise) requires at least one field to update.\n");
            return EXIT_FAILURE;
        }
    }

    /* Connect to Redis */
    redisContext *c = redis_connect();

    /* Check for existing active project */
    if (!reviseFlag) {
        char *current_status = redis_get(c, RKEY_CURRENT_STATUS);
        if (current_status != NULL) {
            int st = atoi(current_status);
            free(current_status);
            if (st == STATUS_ACTIVE || st == STATUS_LOCKOUT) {
                fprintf(stderr,
                    "Error: there is an active project (status=%d).\n"
                    "Use -r to revise, or run endProject first.\n", st);
                redisFree(c);
                return EXIT_FAILURE;
            }
        }
    }

    /* If revising, verify a project actually exists */
    if (reviseFlag) {
        char *current_code = redis_get(c, RKEY_CURRENT_CODE);
        if (current_code == NULL || strlen(current_code) == 0) {
            fprintf(stderr,
                "Error: no existing project to revise. "
                "Start a new project first.\n");
            if (current_code) free(current_code);
            redisFree(c);
            return EXIT_FAILURE;
        }
        free(current_code);
    }

    /* Generate timestamp */
    time_t now = time(NULL);
    struct tm *tm_info = localtime(&now);
    char timestamp[32];
    strftime(timestamp, sizeof(timestamp), "%Y-%m-%d %H:%M:%S", tm_info);

    /* For a new project, generate project code from log history */
    char project_code[MAX_CODE_LEN];
    if (!reviseFlag) {
        generate_project_code(c, project_code, sizeof(project_code));
    } else {
        /* Keep existing project code for revision */
        char *existing_code = redis_get(c, RKEY_CURRENT_CODE);
        if (existing_code) {
            strncpy(project_code, existing_code, MAX_CODE_LEN - 1);
            project_code[MAX_CODE_LEN - 1] = '\0';
            free(existing_code);
        }
    }

    /* Determine status from project type */
    int status;
    if (typeFlag) {
        status = status_from_type(type);
    } else if (reviseFlag) {
        char *existing_status = redis_get(c, RKEY_CURRENT_STATUS);
        status = existing_status ? atoi(existing_status) : STATUS_ACTIVE;
        if (existing_status) free(existing_status);
    } else {
        status = STATUS_ACTIVE;
    }

    /* Write fields to current display keys */
    char numbuf[16];

    if (piFlag)
        redis_set(c, RKEY_CURRENT_PI, pi);
    if (observerFlag)
        redis_set(c, RKEY_CURRENT_OBSERVER, observer);
    if (locationFlag)
        redis_set(c, RKEY_CURRENT_LOCATION, location);
    if (descriptionFlag)
        redis_set(c, RKEY_CURRENT_DESCRIPTION, description);
    if (typeFlag)
        redis_set(c, RKEY_CURRENT_TYPE, type);
    if (receiverFlag) {
        snprintf(numbuf, sizeof(numbuf), "%d", receiver);
        redis_set(c, RKEY_CURRENT_RECEIVER, numbuf);
    }
    if (commentFlag)
        redis_set(c, RKEY_CURRENT_COMMENT, comment);

    /* Always write status, code, and timestamp to current keys */
    snprintf(numbuf, sizeof(numbuf), "%d", status);
    redis_set(c, RKEY_CURRENT_STATUS, numbuf);
    redis_set(c, RKEY_CURRENT_CODE, project_code);
    redis_set(c, RKEY_CURRENT_TIMESTAMP, timestamp);

    /*
     * For the log entry, read back any fields not given on this invocation
     * (in case of revise) so the log has the complete picture.
     */
    char log_pi[MAX_PI_LEN], log_observer[MAX_OBSERVER_LEN];
    char log_location[MAX_LOCATION_LEN], log_description[MAX_DESC_LEN];
    char log_type[MAX_TYPE_LEN], log_comment[MAX_COMMENT_LEN];
    int  log_receiver;

    if (piFlag) {
        strncpy(log_pi, pi, MAX_PI_LEN);
    } else {
        char *v = redis_get(c, RKEY_CURRENT_PI);
        strncpy(log_pi, v ? v : "", MAX_PI_LEN);
        if (v) free(v);
    }
    if (observerFlag) {
        strncpy(log_observer, observer, MAX_OBSERVER_LEN);
    } else {
        char *v = redis_get(c, RKEY_CURRENT_OBSERVER);
        strncpy(log_observer, v ? v : "", MAX_OBSERVER_LEN);
        if (v) free(v);
    }
    if (locationFlag) {
        strncpy(log_location, location, MAX_LOCATION_LEN);
    } else {
        char *v = redis_get(c, RKEY_CURRENT_LOCATION);
        strncpy(log_location, v ? v : "", MAX_LOCATION_LEN);
        if (v) free(v);
    }
    if (descriptionFlag) {
        strncpy(log_description, description, MAX_DESC_LEN);
    } else {
        char *v = redis_get(c, RKEY_CURRENT_DESCRIPTION);
        strncpy(log_description, v ? v : "", MAX_DESC_LEN);
        if (v) free(v);
    }
    if (typeFlag) {
        strncpy(log_type, type, MAX_TYPE_LEN);
    } else {
        char *v = redis_get(c, RKEY_CURRENT_TYPE);
        strncpy(log_type, v ? v : "", MAX_TYPE_LEN);
        if (v) free(v);
    }
    if (receiverFlag) {
        log_receiver = receiver;
    } else {
        char *v = redis_get(c, RKEY_CURRENT_RECEIVER);
        log_receiver = v ? atoi(v) : 0;
        if (v) free(v);
    }
    if (commentFlag) {
        strncpy(log_comment, comment, MAX_COMMENT_LEN);
    } else {
        char *v = redis_get(c, RKEY_CURRENT_COMMENT);
        strncpy(log_comment, v ? v : "", MAX_COMMENT_LEN);
        if (v) free(v);
    }

    /* Write to permanent log (hash + sorted set index) */
    log_project(c, project_code, log_pi, log_observer, log_location,
                log_description, log_type, log_receiver, log_comment,
                status, timestamp, now, !reviseFlag);

    /* Print summary */
    printf("Project %s:\n", reviseFlag ? "revised" : "created");
    printf("  Code:        %s\n", project_code);
    printf("  PI:          %s\n", log_pi);
    printf("  Observer:    %s\n", log_observer);
    printf("  Location:    %s\n", log_location);
    printf("  Description: %s\n", log_description);
    printf("  Type:        %s\n", log_type);
    printf("  Receiver:    %d\n", log_receiver);
    printf("  Comment:     %s\n", log_comment);
    printf("  Status:      %d (%s)\n", status,
           status == STATUS_ACTIVE  ? "active" :
           status == STATUS_LOCKOUT ? "lockout" : "idle");
    printf("  Timestamp:   %s\n", timestamp);

    redisFree(c);
    return EXIT_SUCCESS;
}
