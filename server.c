#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <pthread.h>

#include <microhttpd.h>
#include <cjson/cJSON.h>
#include <curl/curl.h>

#include "quiz_engine.h"

#define PORT 8080
#define HISTORY_FILE "history.json"

#define MAX_SESSIONS 100
#define MAX_REQUEST_BODY 8192

/* ================= SESSION MANAGEMENT ================= */

typedef struct {
    int used;
    WebQuizSession quiz;
} ServerSession;

static ServerSession sessions[MAX_SESSIONS];

static pthread_mutex_t sessions_mutex =
    PTHREAD_MUTEX_INITIALIZER;

static unsigned long session_counter = 0;

/* ================= REQUEST STRUCTURE ================= */

typedef struct {
    char body[MAX_REQUEST_BODY + 1];
    size_t length;
    int processed;
} RequestData;

/* ================= CORS HEADERS ================= */

static void add_cors_headers(struct MHD_Response *response) {
    MHD_add_response_header(
        response,
        "Access-Control-Allow-Origin",
        "*"
    );

    MHD_add_response_header(
        response,
        "Access-Control-Allow-Methods",
        "GET, POST, OPTIONS"
    );

    MHD_add_response_header(
        response,
        "Access-Control-Allow-Headers",
        "Content-Type"
    );

    MHD_add_response_header(
        response,
        "Access-Control-Max-Age",
        "86400"
    );
}

/* ================= SEND JSON RESPONSE ================= */

static enum MHD_Result send_json(
    struct MHD_Connection *connection,
    unsigned int status,
    cJSON *json
) {
    char *output = cJSON_PrintUnformatted(json);

    if (!output) {
        cJSON_Delete(json);
        return MHD_NO;
    }

    cJSON_Delete(json);

    struct MHD_Response *response =
        MHD_create_response_from_buffer(
            strlen(output),
            output,
            MHD_RESPMEM_MUST_FREE
        );

    if (!response) {
        free(output);
        return MHD_NO;
    }

    MHD_add_response_header(
        response,
        "Content-Type",
        "application/json; charset=utf-8"
    );

    add_cors_headers(response);

    enum MHD_Result result =
        MHD_queue_response(connection, status, response);

    MHD_destroy_response(response);

    return result;
}

/* ================= ERROR RESPONSE ================= */

static enum MHD_Result send_error(
    struct MHD_Connection *connection,
    unsigned int status,
    const char *message
) {
    cJSON *json = cJSON_CreateObject();

    if (!json) return MHD_NO;

    cJSON_AddBoolToObject(json, "success", 0);
    cJSON_AddStringToObject(json, "error", message);

    return send_json(connection, status, json);
}

/* ================= READ HISTORY ================= */

static char *read_history_file(void) {
    FILE *file = fopen(HISTORY_FILE, "rb");

    if (!file) return NULL;

    if (fseek(file, 0, SEEK_END) != 0) {
        fclose(file);
        return NULL;
    }

    long file_size = ftell(file);

    if (file_size < 0 ||
        file_size > 10000000 ||
        fseek(file, 0, SEEK_SET) != 0) {
        fclose(file);
        return NULL;
    }

    char *data = malloc((size_t)file_size + 1);

    if (!data) {
        fclose(file);
        return NULL;
    }

    size_t bytes_read =
        fread(data, 1, (size_t)file_size, file);

    fclose(file);

    if (bytes_read != (size_t)file_size) {
        free(data);
        return NULL;
    }

    data[bytes_read] = '\0';

    return data;
}

/* ================= HISTORY API ================= */

static enum MHD_Result handle_history_api(
    struct MHD_Connection *connection
) {
    char *history = read_history_file();

    if (!history) {
        return send_error(
            connection,
            MHD_HTTP_INTERNAL_SERVER_ERROR,
            "Unable to read history.json"
        );
    }

    cJSON *json = cJSON_Parse(history);

    free(history);

    if (!json || !cJSON_IsArray(json)) {
        cJSON_Delete(json);

        return send_error(
            connection,
            MHD_HTTP_INTERNAL_SERVER_ERROR,
            "Invalid history JSON"
        );
    }

    return send_json(
        connection,
        MHD_HTTP_OK,
        json
    );
}

/* ================= QUESTION JSON ================= */

/*
   Correct answer is deliberately excluded here.
*/

static cJSON *create_question_json(
    const Question *question,
    int index,
    int total
) {
    cJSON *json = cJSON_CreateObject();

    if (!json) return NULL;

    cJSON_AddNumberToObject(json, "number", index + 1);
    cJSON_AddNumberToObject(json, "total", total);

    cJSON_AddStringToObject(
        json,
        "question",
        question->question
    );

    cJSON *options = cJSON_AddArrayToObject(
        json,
        "options"
    );

    if (!options) {
        cJSON_Delete(json);
        return NULL;
    }

    for (int i = 0; i < 4; i++) {
        cJSON_AddItemToArray(
            options,
            cJSON_CreateString(question->options[i])
        );
    }

    return json;
}

/* ================= FIND SESSION ================= */

static ServerSession *find_session(const char *session_id) {
    if (!session_id) return NULL;

    for (int i = 0; i < MAX_SESSIONS; i++) {
        if (sessions[i].used &&
            strcmp(
                sessions[i].quiz.session_id,
                session_id
            ) == 0) {
            return &sessions[i];
        }
    }

    return NULL;
}

/* ================= START QUIZ API ================= */

/*
   POST /api/quiz/start

   JSON:
   {
       "player": "Student",
       "topic": "AI & ML",
       "count": 10
   }
*/

static enum MHD_Result handle_start_quiz(
    struct MHD_Connection *connection,
    const char *body
) {
    cJSON *input = cJSON_Parse(body);

    if (!input || !cJSON_IsObject(input)) {
        cJSON_Delete(input);

        return send_error(
            connection,
            MHD_HTTP_BAD_REQUEST,
            "Invalid JSON request"
        );
    }

    cJSON *player =
        cJSON_GetObjectItemCaseSensitive(input, "player");

    cJSON *topic =
        cJSON_GetObjectItemCaseSensitive(input, "topic");

    cJSON *count =
        cJSON_GetObjectItemCaseSensitive(input, "count");

    if (!cJSON_IsString(player) ||
        !cJSON_IsString(topic) ||
        !cJSON_IsNumber(count) ||
        (count->valueint != 10 && count->valueint != 15) ||
        (double)count->valueint != count->valuedouble) {

        cJSON_Delete(input);

        return send_error(
            connection,
            MHD_HTTP_BAD_REQUEST,
            "Player, topic and question count are required"
        );
    }

    char player_name[50];
    char topic_name[80];

    if (strlen(player->valuestring) >= sizeof(player_name) ||
        strlen(topic->valuestring) >= sizeof(topic_name)) {

        cJSON_Delete(input);

        return send_error(
            connection,
            MHD_HTTP_BAD_REQUEST,
            "Player name or topic is too long"
        );
    }

    snprintf(
        player_name,
        sizeof(player_name),
        "%s",
        player->valuestring
    );

    snprintf(
        topic_name,
        sizeof(topic_name),
        "%s",
        topic->valuestring
    );

    int question_count = count->valueint;

    cJSON_Delete(input);

    cJSON *output = NULL;

    pthread_mutex_lock(&sessions_mutex);

    ServerSession *slot = NULL;

    for (int i = 0; i < MAX_SESSIONS; i++) {
        if (!sessions[i].used ||
            sessions[i].quiz.completed) {
            slot = &sessions[i];
            break;
        }
    }

    if (!slot) {
        pthread_mutex_unlock(&sessions_mutex);

        return send_error(
            connection,
            MHD_HTTP_SERVICE_UNAVAILABLE,
            "Maximum active quiz sessions reached"
        );
    }

    memset(slot, 0, sizeof(*slot));

    int started = quiz_engine_start(
        &slot->quiz,
        player_name,
        topic_name,
        question_count
    );

    if (!started) {
        pthread_mutex_unlock(&sessions_mutex);

        return send_error(
            connection,
            MHD_HTTP_BAD_REQUEST,
            "AI question generation failed. Check Render logs for the [AI] Gemini error response; the website quiz uses live Gemini generation."
        );
    }

    slot->used = 1;

    session_counter++;

    snprintf(
        slot->quiz.session_id,
        sizeof(slot->quiz.session_id),
        "quiz-%ld-%lu",
        (long)time(NULL),
        session_counter
    );

    output = cJSON_CreateObject();

    if (!output) {
        slot->used = 0;
        pthread_mutex_unlock(&sessions_mutex);
        return MHD_NO;
    }

    cJSON_AddBoolToObject(output, "success", 1);

    cJSON_AddStringToObject(
        output,
        "session_id",
        slot->quiz.session_id
    );

    cJSON_AddStringToObject(
        output,
        "player",
        slot->quiz.player
    );

    cJSON_AddStringToObject(
        output,
        "topic",
        slot->quiz.topic
    );

    cJSON_AddNumberToObject(
        output,
        "total_questions",
        slot->quiz.total_questions
    );

    cJSON_AddItemToObject(
        output,
        "current_question",
        create_question_json(
            &slot->quiz.questions[0],
            0,
            slot->quiz.total_questions
        )
    );

    pthread_mutex_unlock(&sessions_mutex);

    return send_json(
        connection,
        MHD_HTTP_OK,
        output
    );
}

/* ================= FINAL RESULT JSON ================= */

static void add_final_result(
    cJSON *output,
    const WebQuizSession *session,
    int history_saved
) {
    double percentage = 0;

    if (session->total_questions > 0) {
        percentage =
            (double)session->correct_answers * 100.0 /
            session->total_questions;
    }

    struct timespec now;

    double duration = 0;

    if (clock_gettime(CLOCK_MONOTONIC, &now) == 0) {
        double end_time =
            (double)now.tv_sec +
            (double)now.tv_nsec / 1000000000.0;

        duration = end_time - session->start_time;

        if (duration < 0) duration = 0;
    }

    time_t current_time = time(NULL);
    struct tm local_time;
    char date[40] = "Unavailable";

    if (localtime_r(&current_time, &local_time)) {
        strftime(
            date,
            sizeof(date),
            "%d-%m-%Y %I:%M %p",
            &local_time
        );
    }

    cJSON_AddBoolToObject(output, "completed", 1);

    cJSON_AddStringToObject(
        output,
        "student",
        session->player
    );

    cJSON_AddStringToObject(
        output,
        "topic",
        session->topic
    );

    cJSON_AddNumberToObject(
        output,
        "correct",
        session->correct_answers
    );

    cJSON_AddNumberToObject(
        output,
        "incorrect",
        session->incorrect_answers
    );

    cJSON_AddNumberToObject(
        output,
        "total",
        session->total_questions
    );

    cJSON_AddNumberToObject(
        output,
        "percentage",
        percentage
    );

    cJSON_AddNumberToObject(
        output,
        "duration_seconds",
        duration
    );

    cJSON_AddStringToObject(
        output,
        "date",
        date
    );

    cJSON_AddBoolToObject(
        output,
        "history_saved",
        history_saved
    );
}

/* ================= SUBMIT ANSWER API ================= */

/*
   POST /api/quiz/answer

   JSON:
   {
       "session_id": "quiz-...",
       "answer": "A"
   }
*/

static enum MHD_Result handle_submit_answer(
    struct MHD_Connection *connection,
    const char *body
) {
    cJSON *input = cJSON_Parse(body);

    if (!input || !cJSON_IsObject(input)) {
        cJSON_Delete(input);

        return send_error(
            connection,
            MHD_HTTP_BAD_REQUEST,
            "Invalid JSON request"
        );
    }

    cJSON *session_value =
        cJSON_GetObjectItemCaseSensitive(input, "session_id");

    cJSON *answer_value =
        cJSON_GetObjectItemCaseSensitive(input, "answer");

    if (!cJSON_IsString(session_value) ||
        !cJSON_IsString(answer_value) ||
        strlen(answer_value->valuestring) != 1) {

        cJSON_Delete(input);

        return send_error(
            connection,
            MHD_HTTP_BAD_REQUEST,
            "Session ID and one answer character are required"
        );
    }

    char session_id[64];

    snprintf(
        session_id,
        sizeof(session_id),
        "%s",
        session_value->valuestring
    );

    char answer = answer_value->valuestring[0];

    cJSON_Delete(input);

    cJSON *output = NULL;

    pthread_mutex_lock(&sessions_mutex);

    ServerSession *slot = find_session(session_id);

    if (!slot || slot->quiz.completed) {
        pthread_mutex_unlock(&sessions_mutex);

        return send_error(
            connection,
            MHD_HTTP_NOT_FOUND,
            "Quiz session not found or already completed"
        );
    }

    WebQuizSession *session = &slot->quiz;

    Question current_question =
        session->questions[session->current_question];

    int result = quiz_engine_submit_answer(
        session,
        answer
    );

    if (result == -1) {
        pthread_mutex_unlock(&sessions_mutex);

        return send_error(
            connection,
            MHD_HTTP_BAD_REQUEST,
            "Invalid answer"
        );
    }

    output = cJSON_CreateObject();

    if (!output) {
        pthread_mutex_unlock(&sessions_mutex);
        return MHD_NO;
    }

    cJSON_AddBoolToObject(output, "success", 1);

    cJSON_AddBoolToObject(
        output,
        "correct",
        result == 1
    );

    char correct_answer[2] = {
        current_question.correct_option,
        '\0'
    };

    cJSON_AddStringToObject(
        output,
        "correct_answer",
        correct_answer
    );

    cJSON_AddStringToObject(
        output,
        "explanation",
        current_question.explanation
    );

    if (session->completed) {
        add_final_result(
            output,
            session,
            result != -2
        );
    } else {
        cJSON_AddBoolToObject(output, "completed", 0);

        cJSON_AddItemToObject(
            output,
            "next_question",
            create_question_json(
                &session->questions[session->current_question],
                session->current_question,
                session->total_questions
            )
        );
    }

    pthread_mutex_unlock(&sessions_mutex);

    return send_json(
        connection,
        MHD_HTTP_OK,
        output
    );
}

/* ================= HEALTH CHECK ================= */

static enum MHD_Result handle_health_check(
    struct MHD_Connection *connection
) {
    cJSON *json = cJSON_CreateObject();

    if (!json) return MHD_NO;

    cJSON_AddStringToObject(json, "status", "online");

    cJSON_AddStringToObject(
        json,
        "message",
        "C backend connected"
    );

    return send_json(
        connection,
        MHD_HTTP_OK,
        json
    );
}

/* ================= REQUEST COMPLETED ================= */

static void request_completed(
    void *cls,
    struct MHD_Connection *connection,
    void **con_cls,
    enum MHD_RequestTerminationCode toe
) {
    (void)cls;
    (void)connection;
    (void)toe;

    if (con_cls && *con_cls) {
        free(*con_cls);
        *con_cls = NULL;
    }
}

/* ================= MAIN REQUEST HANDLER ================= */

static enum MHD_Result answer_to_connection(
    void *cls,
    struct MHD_Connection *connection,
    const char *url,
    const char *method,
    const char *version,
    const char *upload_data,
    size_t *upload_data_size,
    void **con_cls
) {
    (void)cls;
    (void)version;

    /* Create request context */
    if (*con_cls == NULL) {
        RequestData *request = calloc(1, sizeof(RequestData));

        if (!request) return MHD_NO;

        *con_cls = request;

        return MHD_YES;
    }

    RequestData *request = *con_cls;

    /* Handle incoming POST body */
    if (*upload_data_size > 0) {
        if (strcmp(method, "POST") != 0) {
            *upload_data_size = 0;
            return MHD_YES;
        }

        if (*upload_data_size >
            MAX_REQUEST_BODY - request->length) {

            request->processed = 1;

            *upload_data_size = 0;

            return send_error(
                connection,
                MHD_HTTP_CONTENT_TOO_LARGE,
                "Request body too large"
            );
        }

        memcpy(
            request->body + request->length,
            upload_data,
            *upload_data_size
        );

        request->length += *upload_data_size;

        request->body[request->length] = '\0';

        *upload_data_size = 0;

        return MHD_YES;
    }

    if (request->processed) {
        return MHD_YES;
    }

    request->processed = 1;

    /* Handle CORS preflight */
    if (strcmp(method, "OPTIONS") == 0) {
        struct MHD_Response *response =
            MHD_create_response_from_buffer(
                0,
                "",
                MHD_RESPMEM_PERSISTENT
            );

        if (!response) return MHD_NO;

        add_cors_headers(response);

        enum MHD_Result result =
            MHD_queue_response(
                connection,
                MHD_HTTP_NO_CONTENT,
                response
            );

        MHD_destroy_response(response);

        return result;
    }

    /* GET routes */
    if (strcmp(method, "GET") == 0) {
        if (strcmp(url, "/") == 0) {
            return handle_health_check(connection);
        }

        if (strcmp(url, "/api/history") == 0) {
            return handle_history_api(connection);
        }

        return send_error(
            connection,
            MHD_HTTP_NOT_FOUND,
            "GET endpoint not found"
        );
    }

    /* POST routes */
    if (strcmp(method, "POST") == 0) {
        if (strcmp(url, "/api/quiz/start") == 0) {
            return handle_start_quiz(
                connection,
                request->body
            );
        }

        if (strcmp(url, "/api/quiz/answer") == 0) {
            return handle_submit_answer(
                connection,
                request->body
            );
        }

        return send_error(
            connection,
            MHD_HTTP_NOT_FOUND,
            "POST endpoint not found"
        );
    }

    return send_error(
        connection,
        MHD_HTTP_METHOD_NOT_ALLOWED,
        "Method not allowed"
    );
}

/* ================= MAIN ================= */

int main(void) {
    if (curl_global_init(CURL_GLOBAL_DEFAULT) != CURLE_OK) {
        printf("Curl initialization failed!\n");
        return 1;
    }

    struct MHD_Daemon *daemon = MHD_start_daemon(
        MHD_USE_INTERNAL_POLLING_THREAD,
        PORT,
        NULL,
        NULL,
        &answer_to_connection,
        NULL,
        MHD_OPTION_NOTIFY_COMPLETED,
        &request_completed,
        NULL,
        MHD_OPTION_END
    );

    if (!daemon) {
        printf("Server start failed!\n");
        curl_global_cleanup();
        return 1;
    }

    printf("\n====================================\n");
    printf("       AI POWERED QUIZ GAME\n");
    printf("====================================\n");
    printf("C Backend Server Started!\n");
    printf("Open: http://localhost:8080\n");
    printf("\nAvailable APIs:\n");
    printf("GET  /api/history\n");
    printf("POST /api/quiz/start\n");
    printf("POST /api/quiz/answer\n");
    printf("\nPress ENTER to stop server.\n");

    getchar();

    MHD_stop_daemon(daemon);

    curl_global_cleanup();

    printf("Server stopped.\n");

    return 0;
}