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
#include "storage.h"

#define PORT 8080
static const char *history_file_path(void) {
    const char *configured = getenv("QUIZ_HISTORY_FILE");
    return (configured && configured[0]) ? configured : "history.json";
}

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
    MHD_add_response_header(response, "Cache-Control", "no-store, no-cache, must-revalidate, max-age=0");
    MHD_add_response_header(response, "Pragma", "no-cache");
    MHD_add_response_header(response, "Expires", "0");

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

static int migrate_bundled_history(void) {
    const char *configured = getenv("QUIZ_HISTORY_FILE");
    if (!configured || !configured[0] || strcmp(configured, "history.json") == 0) {
        return 0;
    }

    FILE *existing = fopen(configured, "rb");
    if (existing) {
        fclose(existing);
        return 1;
    }

    FILE *source = fopen("history.json", "rb");
    if (!source) return 0;

    char temp_path[1200];
    int path_length = snprintf(temp_path, sizeof(temp_path), "%s.migrate.tmp", configured);
    if (path_length < 0 || (size_t)path_length >= sizeof(temp_path)) {
        fclose(source);
        return 0;
    }

    FILE *target = fopen(temp_path, "wb");
    if (!target) {
        fclose(source);
        return 0;
    }

    char buffer[8192];
    size_t bytes;
    int ok = 1;
    while ((bytes = fread(buffer, 1, sizeof(buffer), source)) > 0) {
        if (fwrite(buffer, 1, bytes, target) != bytes) {
            ok = 0;
            break;
        }
    }
    if (ferror(source)) ok = 0;
    if (fclose(source) != 0) ok = 0;
    if (fflush(target) != 0) ok = 0;
    if (fclose(target) != 0) ok = 0;

    if (!ok || rename(temp_path, configured) != 0) {
        remove(temp_path);
        return 0;
    }
    return 1;
}

static char *read_history_file(void) {
    if (storage_is_configured()) return storage_read_history_json();
    FILE *file = fopen(history_file_path(), "rb");

    /* Migrate the bundled seed exactly once into the configured persistent disk. */
    if (!file && migrate_bundled_history()) {
        file = fopen(history_file_path(), "rb");
    }

    /* Without an explicitly configured disk, use the bundled local history. */
    const char *configured = getenv("QUIZ_HISTORY_FILE");
    if (!file && (!configured || !configured[0])) {
        file = fopen("history.json", "rb");
    }

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


static void quiz_session_storage_key(const char *id, char *key, size_t size) {
    snprintf(key, size, "quiz_session_%s", id ? id : "");
}
static cJSON *full_question_to_json(const Question *q) {
    cJSON *o=cJSON_CreateObject(); if(!o)return NULL;
    cJSON_AddStringToObject(o,"question",q->question);
    cJSON *a=cJSON_AddArrayToObject(o,"options"); if(!a){cJSON_Delete(o);return NULL;}
    for(int i=0;i<4;i++)cJSON_AddItemToArray(a,cJSON_CreateString(q->options[i]));
    char answer[2]={q->correct_option,'\0'};
    cJSON_AddStringToObject(o,"correct_option",answer);
    cJSON_AddStringToObject(o,"explanation",q->explanation);
    return o;
}
static int persist_active_quiz_session(const WebQuizSession *session) {
    if(!storage_is_configured())return 1;
    if(!session||!session->session_id[0])return 0;
    cJSON *root=cJSON_CreateObject(); if(!root)return 0;
    cJSON_AddBoolToObject(root,"completed",0);
    cJSON_AddStringToObject(root,"session_id",session->session_id);
    cJSON_AddStringToObject(root,"player",session->player);
    cJSON_AddStringToObject(root,"topic",session->topic);
    cJSON_AddNumberToObject(root,"total_questions",session->total_questions);
    cJSON_AddNumberToObject(root,"start_time",session->start_time);
    cJSON *items=cJSON_AddArrayToObject(root,"questions");
    if(!items){cJSON_Delete(root);return 0;}
    for(int i=0;i<session->total_questions;i++){cJSON *v=full_question_to_json(&session->questions[i]);if(!v){cJSON_Delete(root);return 0;}cJSON_AddItemToArray(items,v);}
    char key[128];quiz_session_storage_key(session->session_id,key,sizeof(key));
    char *json=cJSON_PrintUnformatted(root);cJSON_Delete(root);if(!json)return 0;
    int ok=storage_write_json(key,json);free(json);return ok;
}
static int persist_completed_quiz_session(const char *id,const char *answers,const char *response) {
    if(!storage_is_configured())return 1;
    cJSON *root=cJSON_CreateObject();if(!root)return 0;
    cJSON_AddBoolToObject(root,"completed",1);cJSON_AddStringToObject(root,"session_id",id);
    cJSON_AddStringToObject(root,"answers",answers);cJSON_AddStringToObject(root,"response",response);
    char key[128];quiz_session_storage_key(id,key,sizeof(key));
    char *json=cJSON_PrintUnformatted(root);cJSON_Delete(root);if(!json)return 0;
    int ok=storage_write_json(key,json);free(json);return ok;
}
/* 1=active restored, 2=completed response restored, 0=unavailable. */
static int restore_quiz_session(const char *id,WebQuizSession *session,char *done_answers,size_t done_size,char *done_response,size_t response_size) {
    if(!storage_is_configured())return 0;
    char key[128];quiz_session_storage_key(id,key,sizeof(key));char *data=storage_read_json(key);if(!data)return 0;
    cJSON *root=cJSON_Parse(data);free(data);if(!cJSON_IsObject(root)){cJSON_Delete(root);return 0;}
    cJSON *completed=cJSON_GetObjectItemCaseSensitive(root,"completed");
    if(cJSON_IsTrue(completed)){
        cJSON *a=cJSON_GetObjectItemCaseSensitive(root,"answers"),*r=cJSON_GetObjectItemCaseSensitive(root,"response");
        if(cJSON_IsString(a)&&cJSON_IsString(r)){snprintf(done_answers,done_size,"%s",a->valuestring);snprintf(done_response,response_size,"%s",r->valuestring);cJSON_Delete(root);return 2;}
        cJSON_Delete(root);return 0;
    }
    cJSON *sid=cJSON_GetObjectItemCaseSensitive(root,"session_id"),*player=cJSON_GetObjectItemCaseSensitive(root,"player"),*topic=cJSON_GetObjectItemCaseSensitive(root,"topic"),*total=cJSON_GetObjectItemCaseSensitive(root,"total_questions"),*start=cJSON_GetObjectItemCaseSensitive(root,"start_time"),*items=cJSON_GetObjectItemCaseSensitive(root,"questions");
    int valid=cJSON_IsString(sid)&&strcmp(sid->valuestring,id)==0&&cJSON_IsString(player)&&cJSON_IsString(topic)&&cJSON_IsNumber(total)&&(total->valueint==10||total->valueint==15)&&cJSON_IsNumber(start)&&cJSON_IsArray(items)&&cJSON_GetArraySize(items)==total->valueint;
    if(!valid){cJSON_Delete(root);return 0;}
    memset(session,0,sizeof(*session));snprintf(session->session_id,sizeof(session->session_id),"%s",id);snprintf(session->player,sizeof(session->player),"%s",player->valuestring);snprintf(session->topic,sizeof(session->topic),"%s",topic->valuestring);session->total_questions=total->valueint;session->start_time=start->valuedouble;
    for(int i=0;i<session->total_questions&&valid;i++){
        cJSON *v=cJSON_GetArrayItem(items,i),*qt=cJSON_GetObjectItemCaseSensitive(v,"question"),*opts=cJSON_GetObjectItemCaseSensitive(v,"options"),*ans=cJSON_GetObjectItemCaseSensitive(v,"correct_option"),*exp=cJSON_GetObjectItemCaseSensitive(v,"explanation");
        if(!cJSON_IsString(qt)||strlen(qt->valuestring)>=QUESTION_LEN||!cJSON_IsArray(opts)||cJSON_GetArraySize(opts)!=4||!cJSON_IsString(ans)||strlen(ans->valuestring)!=1||!cJSON_IsString(exp)||strlen(exp->valuestring)>=EXPLANATION_LEN){valid=0;break;}
        char answer=(char)toupper((unsigned char)ans->valuestring[0]);if(answer<'A'||answer>'D'){valid=0;break;}
        strcpy(session->questions[i].question,qt->valuestring);strcpy(session->questions[i].explanation,exp->valuestring);session->questions[i].correct_option=answer;
        for(int k=0;k<4;k++){cJSON *op=cJSON_GetArrayItem(opts,k);if(!cJSON_IsString(op)||strlen(op->valuestring)>=OPTION_LEN){valid=0;break;}strcpy(session->questions[i].options[k],op->valuestring);}
    }
    cJSON_Delete(root);return valid?1:0;
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

    /* Gemini generation is network-bound. Do not hold the global session
       mutex while waiting, or every existing quiz answer/API request stalls. */
    WebQuizSession generated_quiz;
    memset(&generated_quiz, 0, sizeof(generated_quiz));
    int started = quiz_engine_start(
        &generated_quiz,
        player_name,
        topic_name,
        question_count
    );
    if (!started) {
        return send_error(
            connection,
            MHD_HTTP_BAD_GATEWAY,
            "AI question generation failed. Check Render logs for the [AI] Gemini error response; the website quiz uses live Gemini generation."
        );
    }

    pthread_mutex_lock(&sessions_mutex);
    ServerSession *slot = NULL;
    for (int i = 0; i < MAX_SESSIONS; i++) {
        if (!sessions[i].used || sessions[i].quiz.completed) {
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
    slot->quiz = generated_quiz;
    slot->used = 1;
    session_counter++;
    snprintf(
        slot->quiz.session_id,
        sizeof(slot->quiz.session_id),
        "quiz-%ld-%lu",
        (long)time(NULL),
        session_counter
    );

    if (!persist_active_quiz_session(&slot->quiz)) {
        slot->used = 0;
        pthread_mutex_unlock(&sessions_mutex);
        return send_error(connection, MHD_HTTP_SERVICE_UNAVAILABLE,
                          "Quiz session could not be saved. Please start the quiz again.");
    }
    cJSON *output = cJSON_CreateObject();
    if (!output) {
        slot->used = 0;
        pthread_mutex_unlock(&sessions_mutex);
        return MHD_NO;
    }
    cJSON_AddBoolToObject(output, "success", 1);
    cJSON_AddStringToObject(output, "session_id", slot->quiz.session_id);
    cJSON_AddStringToObject(output, "player", slot->quiz.player);
    cJSON_AddStringToObject(output, "topic", slot->quiz.topic);
    cJSON_AddNumberToObject(output, "total_questions", slot->quiz.total_questions);
    cJSON_AddItemToObject(
        output,
        "current_question",
        create_question_json(&slot->quiz.questions[0], 0, slot->quiz.total_questions)
    );

    /* Preload the entire question set for instant local navigation.
       Correct answers and explanations remain server-side until submission. */
    cJSON *question_cache = cJSON_AddArrayToObject(output, "question_cache");
    if (question_cache) {
        for (int i = 0; i < slot->quiz.total_questions; i++) {
            cJSON *cached_question = create_question_json(
                &slot->quiz.questions[i], i, slot->quiz.total_questions
            );
            if (cached_question) cJSON_AddItemToArray(question_cache, cached_question);
        }
    }

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



/* One network submission at the end of the locally cached quiz. */
static enum MHD_Result handle_complete_quiz(struct MHD_Connection *connection,const char *body) {
    cJSON *input=cJSON_Parse(body);
    if(!cJSON_IsObject(input)){cJSON_Delete(input);return send_error(connection,MHD_HTTP_BAD_REQUEST,"Invalid quiz completion request");}
    cJSON *sidv=cJSON_GetObjectItemCaseSensitive(input,"session_id"),*av=cJSON_GetObjectItemCaseSensitive(input,"answers");
    if(!cJSON_IsString(sidv)||!cJSON_IsArray(av)){cJSON_Delete(input);return send_error(connection,MHD_HTTP_BAD_REQUEST,"Session ID and complete answer list are required");}
    char id[64],answers[WEB_MAX_QUESTIONS+1]={0};snprintf(id,sizeof(id),"%s",sidv->valuestring);
    int count=cJSON_GetArraySize(av);if(count!=10&&count!=15){cJSON_Delete(input);return send_error(connection,MHD_HTTP_BAD_REQUEST,"Submit all 10 or 15 answers together");}
    for(int i=0;i<count;i++){cJSON *v=cJSON_GetArrayItem(av,i);if(!cJSON_IsString(v)||strlen(v->valuestring)!=1){cJSON_Delete(input);return send_error(connection,MHD_HTTP_BAD_REQUEST,"Select one option for every question");}char a=(char)toupper((unsigned char)v->valuestring[0]);if(a<'A'||a>'D'){cJSON_Delete(input);return send_error(connection,MHD_HTTP_BAD_REQUEST,"One or more answers are invalid");}answers[i]=a;}
    cJSON_Delete(input);

    pthread_mutex_lock(&sessions_mutex);
    ServerSession *slot=find_session(id);
    if(!slot){
        WebQuizSession restored;char old_answers[WEB_MAX_QUESTIONS+1]={0},old_response[8192]={0};
        int state=restore_quiz_session(id,&restored,old_answers,sizeof(old_answers),old_response,sizeof(old_response));
        if(state==2){pthread_mutex_unlock(&sessions_mutex);if(strcmp(old_answers,answers)==0){cJSON *cached=cJSON_Parse(old_response);if(cached)return send_json(connection,MHD_HTTP_OK,cached);}return send_error(connection,MHD_HTTP_CONFLICT,"This quiz was already completed with a different answer set.");}
        if(state==1){for(int i=0;i<MAX_SESSIONS;i++)if(!sessions[i].used||sessions[i].quiz.completed){slot=&sessions[i];break;}if(slot){memset(slot,0,sizeof(*slot));slot->quiz=restored;slot->used=1;}}
    }
    if(!slot){pthread_mutex_unlock(&sessions_mutex);return send_error(connection,MHD_HTTP_NOT_FOUND,"Quiz session expired. Please start a new quiz.");}
    if(slot->quiz.completed){
        int same=strcmp(slot->completion_answers,answers)==0&&slot->completion_response[0];
        char saved[8192];snprintf(saved,sizeof(saved),"%s",slot->completion_response);pthread_mutex_unlock(&sessions_mutex);
        if(same){cJSON *cached=cJSON_Parse(saved);if(cached)return send_json(connection,MHD_HTTP_OK,cached);}
        return send_error(connection,MHD_HTTP_CONFLICT,"This quiz was already completed with a different answer set.");
    }
    if(count!=slot->quiz.total_questions||slot->quiz.current_question!=0){pthread_mutex_unlock(&sessions_mutex);return send_error(connection,MHD_HTTP_CONFLICT,"Quiz answer list does not match the active quiz.");}
    int scored=quiz_engine_submit_answers(&slot->quiz,answers,count);
    if(scored==-1||!slot->quiz.completed){pthread_mutex_unlock(&sessions_mutex);return send_error(connection,MHD_HTTP_BAD_REQUEST,"Quiz answers could not be validated.");}

    cJSON *output=cJSON_CreateObject();if(!output){pthread_mutex_unlock(&sessions_mutex);return MHD_NO;}
    cJSON_AddBoolToObject(output,"success",1);add_final_result(output,&slot->quiz,scored!=-2);
    cJSON *review=cJSON_AddArrayToObject(output,"answer_review");
    if(review)for(int i=0;i<count;i++){
        const Question *q=&slot->quiz.questions[i];char correct[2]={q->correct_option,'\0'},selected[2]={answers[i],'\0'};
        cJSON *item=cJSON_CreateObject();if(!item)continue;
        cJSON_AddNumberToObject(item,"number",i+1);cJSON_AddStringToObject(item,"question",q->question);
        cJSON_AddStringToObject(item,"selected_answer",selected);cJSON_AddStringToObject(item,"correct_answer",correct);
        cJSON_AddBoolToObject(item,"correct",answers[i]==q->correct_option);cJSON_AddStringToObject(item,"explanation",q->explanation);
        cJSON_AddItemToArray(review,item);
    }
    snprintf(slot->completion_answers,sizeof(slot->completion_answers),"%s",answers);
    char *response=cJSON_PrintUnformatted(output);
    if(response){snprintf(slot->completion_response,sizeof(slot->completion_response),"%s",response);if(!persist_completed_quiz_session(id,answers,response))fprintf(stderr,"[SESSION] Could not persist completion response for %s.\n",id);free(response);}
    pthread_mutex_unlock(&sessions_mutex);
    return send_json(connection,MHD_HTTP_OK,output);
}

/* ================= PUBLIC CUSTOMER REVIEWS ================= */
static pthread_mutex_t reviews_mutex = PTHREAD_MUTEX_INITIALIZER;
static const char *reviews_file_path(void) {
    const char *p = getenv("QUIZ_REVIEWS_FILE");
    if (p && p[0]) return p;
    p = getenv("QUIZ_HISTORY_FILE");
    if (p && p[0]) {
        static char path[512]; const char *slash = strrchr(p, '/');
        if (slash) { size_t n = (size_t)(slash-p+1); if (n+sizeof("reviews.json")<sizeof(path)) { memcpy(path,p,n); strcpy(path+n,"reviews.json"); return path; } }
    }
    return "reviews.json";
}
static cJSON *read_reviews(void) {
    if (storage_is_configured()) {
        char *data = storage_read_reviews_json();
        if (!data) return NULL;
        cJSON *root = cJSON_Parse(data); free(data);
        if (!cJSON_IsArray(root)) { cJSON_Delete(root); return NULL; }
        return root;
    }
    FILE *f=fopen(reviews_file_path(),"rb"); if(!f) return cJSON_CreateArray();
    if(fseek(f,0,SEEK_END)!=0){fclose(f);return NULL;} long n=ftell(f);
    if(n<0||n>5000000||fseek(f,0,SEEK_SET)!=0){fclose(f);return NULL;}
    char *b=malloc((size_t)n+1); if(!b){fclose(f);return NULL;}
    size_t got=fread(b,1,(size_t)n,f); fclose(f); if(got!=(size_t)n){free(b);return NULL;}
    b[got]='\0'; cJSON *root=cJSON_Parse(b); free(b);
    if(!cJSON_IsArray(root)){cJSON_Delete(root);return NULL;} return root;
}
static int save_reviews(cJSON *root) {
    char *data=cJSON_PrintUnformatted(root); if(!data)return 0;
    if (storage_is_configured()) { int ok=storage_write_json("reviews",data); free(data); return ok; }
    char temp[600];
    int n=snprintf(temp,sizeof(temp),"%s.tmp",reviews_file_path());
    if(n<0||(size_t)n>=sizeof(temp)){free(data);return 0;}
    FILE *f=fopen(temp,"wb"); if(!f){free(data);return 0;}
    int ok=fputs(data,f)>=0; if(fclose(f)!=0)ok=0; free(data);
    if(!ok||rename(temp,reviews_file_path())!=0){remove(temp);return 0;} return 1;
}
static const char *review_value(cJSON *o,const char *key){cJSON *v=cJSON_GetObjectItemCaseSensitive(o,key);return cJSON_IsString(v)?v->valuestring:NULL;}
static int review_valid(const char *v, size_t max, int required) {
    if (!v) return !required;

    size_t n = strlen(v);
    if (n > max || (required && !n)) return 0;

    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)v[i];
        if (c < 32 && c != '\n' && c != '\r' && c != '\t') return 0;
    }
    return 1;
}
static enum MHD_Result reviews_get(struct MHD_Connection *c){
    pthread_mutex_lock(&reviews_mutex);cJSON *root=read_reviews();pthread_mutex_unlock(&reviews_mutex);
    if(!root)return send_error(c,MHD_HTTP_INTERNAL_SERVER_ERROR,"Reviews storage is unreadable");
    return send_json(c,MHD_HTTP_OK,root);
}
static enum MHD_Result reviews_post(struct MHD_Connection *c,const char *body){
    cJSON *in=cJSON_Parse(body);if(!cJSON_IsObject(in)){cJSON_Delete(in);return send_error(c,MHD_HTTP_BAD_REQUEST,"Invalid review data");}
    const char *name=review_value(in,"name"),*title=review_value(in,"title"),*message=review_value(in,"message"),*suggestion=review_value(in,"suggestion");
    cJSON *rating=cJSON_GetObjectItemCaseSensitive(in,"rating");
    if(!review_valid(name,60,1)||!review_valid(title,100,1)||!review_valid(message,1500,1)||!review_valid(suggestion,1000,0)||!cJSON_IsNumber(rating)||rating->valueint<1||rating->valueint>5||rating->valuedouble!=rating->valueint){
        cJSON_Delete(in);return send_error(c,MHD_HTTP_BAD_REQUEST,"Please enter your name, a 1–5 star rating, a headline and review (maximum 1,500 characters).");
    }
    long now=(long)time(NULL);cJSON *entry=cJSON_CreateObject();if(!entry){cJSON_Delete(in);return MHD_NO;}
    char id[64];snprintf(id,sizeof(id),"review-%ld-%u",now,(unsigned int)rand());
    cJSON_AddStringToObject(entry,"id",id);cJSON_AddStringToObject(entry,"name",name);cJSON_AddNumberToObject(entry,"rating",rating->valueint);
    cJSON_AddStringToObject(entry,"title",title);cJSON_AddStringToObject(entry,"message",message);cJSON_AddStringToObject(entry,"suggestion",suggestion?suggestion:"");cJSON_AddNumberToObject(entry,"timestamp",now);cJSON_Delete(in);
    int ok=0;
    if(storage_is_configured()) {
        char *review_json=cJSON_PrintUnformatted(entry);
        ok=review_json&&storage_append_review_json(review_json);
        free(review_json);
        cJSON_Delete(entry);
    } else {
        pthread_mutex_lock(&reviews_mutex);
        cJSON *root=read_reviews();
        ok=root&&cJSON_GetArraySize(root)<5000;
        int appended=0;
        if(ok){cJSON_AddItemToArray(root,entry);appended=1;ok=save_reviews(root);}
        if(!appended)cJSON_Delete(entry);
        cJSON_Delete(root);
        pthread_mutex_unlock(&reviews_mutex);
    }
    if(!ok)return send_error(c,MHD_HTTP_INTERNAL_SERVER_ERROR,"Your review could not be saved. Please try again.");
    cJSON *out=cJSON_CreateObject();if(!out)return MHD_NO;cJSON_AddBoolToObject(out,"success",1);cJSON_AddStringToObject(out,"message","Thank you. Your review has been published.");
    return send_json(c,MHD_HTTP_CREATED,out);
}

/* ================= HEALTH CHECK ================= */

static enum MHD_Result handle_health_check(struct MHD_Connection *connection) {
    cJSON *json = cJSON_CreateObject();
    if (!json) return MHD_NO;
    int db_configured = storage_is_configured();
    char *data = db_configured ? storage_read_history_json() : read_history_file();
    cJSON *history = data ? cJSON_Parse(data) : NULL;
    int readable = cJSON_IsArray(history);
    int count = readable ? cJSON_GetArraySize(history) : 0;
    free(data); cJSON_Delete(history);
    cJSON_AddStringToObject(json,"status",db_configured&&!readable?"degraded":"online");
    cJSON_AddStringToObject(json,"message",db_configured?(readable?"C backend connected to Neon PostgreSQL":"Database unavailable; persistent storage is not ready"):"C backend connected (local development storage)");
    cJSON_AddBoolToObject(json,"database_configured",db_configured);
    cJSON_AddBoolToObject(json,"database_ready",db_configured&&readable);
    cJSON_AddBoolToObject(json,"history_readable",readable);
    cJSON_AddNumberToObject(json,"history_records",count);
    cJSON_AddStringToObject(json,"history_storage",db_configured?(readable?"neon-postgresql":"database-unavailable"):"local-development");
    return send_json(connection,db_configured&&!readable?MHD_HTTP_SERVICE_UNAVAILABLE:MHD_HTTP_OK,json);
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

        if (strcmp(url, "/api/health") == 0) {
            return handle_health_check(connection);
        }

        if (strcmp(url, "/api/history") == 0) {
            return handle_history_api(connection);
        }
        if (strcmp(url, "/api/reviews") == 0) {
            return reviews_get(connection);
        }

        return send_error(
            connection,
            MHD_HTTP_NOT_FOUND,
            "GET endpoint not found"
        );
    }

    /* POST routes */
    if (strcmp(method, "POST") == 0) { 
        if (strcmp(url, "/api/reviews") == 0) {
            return reviews_post(connection, request->body);
        }

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
        if (strcmp(url, "/api/quiz/complete") == 0) {
            return handle_complete_quiz(connection, request->body);
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
    if (!storage_init()) {
        fprintf(stderr, "[STORAGE] Startup aborted: Neon PostgreSQL is unavailable; temporary storage fallback is disabled.\n");
        return 1;
    }
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
