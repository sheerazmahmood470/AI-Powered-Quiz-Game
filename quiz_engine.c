
#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>
#include <time.h>
#include <sys/stat.h>
#include <pthread.h>
#include <cjson/cJSON.h>

#include "quiz_engine.h"

#define MAX_BANK 2000
#define MAX_HISTORY 5000
#define NAME_LEN 50

/* Protects history file operations */
static pthread_mutex_t history_mutex = PTHREAD_MUTEX_INITIALIZER;

/* Protects AI generation and shared cache/question-bank writes */
static pthread_mutex_t quiz_start_mutex = PTHREAD_MUTEX_INITIALIZER;

/* ================= TOPICS ================= */

static const char *field_names[5] = {
    "Philosophy",
    "AI and ML",
    "History",
    "C programming",
    "Finance"
};

static const char *field_files[5] = {
    "questions/philosophy.json",
    "questions/ai_ml.json",
    "questions/history.json",
    "questions/c_programming.json",
    "questions/finance.json"
};

/* ================= HISTORY STRUCTURE ================= */

typedef struct {
    char name[NAME_LEN];
    int score;
    int total;
    double seconds;
    char topic[80];
    char date[40];
    long timestamp;
} HistoryRecord;

/* ================= TOPIC LOOKUP ================= */

static int get_topic_index(const char *topic) {
    if (!topic) return -1;

    for (int i = 0; i < 5; i++) {
        if (strcasecmp(topic, field_names[i]) == 0) {
            return i;
        }
    }

    if (strcasecmp(topic, "AI & ML") == 0) return 1;
    if (strcasecmp(topic, "C Programming") == 0) return 3;

    return -1;
}

/* ================= QUESTION JSON ================= */

static int question_from_json(cJSON *item, Question *q) {
    if (!cJSON_IsObject(item) || !q) return 0;

    cJSON *question =
        cJSON_GetObjectItemCaseSensitive(item, "question");

    cJSON *options =
        cJSON_GetObjectItemCaseSensitive(item, "options");

    cJSON *correct =
        cJSON_GetObjectItemCaseSensitive(item, "correct_option");

    cJSON *explanation =
        cJSON_GetObjectItemCaseSensitive(item, "explanation");

    if (!cJSON_IsString(question) ||
        !cJSON_IsArray(options) ||
        cJSON_GetArraySize(options) != 4 ||
        !cJSON_IsString(correct) ||
        strlen(correct->valuestring) != 1 ||
        !cJSON_IsString(explanation)) {
        return 0;
    }

    char answer =
        (char)toupper((unsigned char)correct->valuestring[0]);

    if (answer < 'A' || answer > 'D' ||
        strlen(question->valuestring) >= QUESTION_LEN ||
        strlen(explanation->valuestring) >= EXPLANATION_LEN) {
        return 0;
    }

    strcpy(q->question, question->valuestring);
    strcpy(q->explanation, explanation->valuestring);
    q->correct_option = answer;

    for (int i = 0; i < 4; i++) {
        cJSON *option = cJSON_GetArrayItem(options, i);

        if (!cJSON_IsString(option) ||
            strlen(option->valuestring) >= OPTION_LEN) {
            return 0;
        }

        strcpy(q->options[i], option->valuestring);
    }

    return 1;
}

static cJSON *question_to_json(const Question *q) {
    cJSON *item = cJSON_CreateObject();

    if (!item) return NULL;

    cJSON_AddStringToObject(item, "question", q->question);

    cJSON *options = cJSON_AddArrayToObject(item, "options");

    if (!options) {
        cJSON_Delete(item);
        return NULL;
    }

    for (int i = 0; i < 4; i++) {
        cJSON *option = cJSON_CreateString(q->options[i]);

        if (!option) {
            cJSON_Delete(item);
            return NULL;
        }

        cJSON_AddItemToArray(options, option);
    }

    char answer[2] = {q->correct_option, '\0'};

    cJSON_AddStringToObject(item, "correct_option", answer);
    cJSON_AddStringToObject(item, "explanation", q->explanation);

    return item;
}

/* ================= LOAD QUESTION BANK ================= */

static int load_bank(
    const char *path,
    Question bank[MAX_BANK]
) {
    FILE *file = fopen(path, "r");

    if (!file) return 0;

    if (fseek(file, 0, SEEK_END) != 0) {
        fclose(file);
        return 0;
    }

    long length = ftell(file);

    if (length <= 0 || length > 5000000 ||
        fseek(file, 0, SEEK_SET) != 0) {
        fclose(file);
        return 0;
    }

    char *data = malloc((size_t)length + 1);

    if (!data) {
        fclose(file);
        return 0;
    }

    size_t got = fread(data, 1, (size_t)length, file);
    data[got] = '\0';

    fclose(file);

    if (got != (size_t)length) {
        free(data);
        return 0;
    }

    cJSON *root = cJSON_Parse(data);
    free(data);

    cJSON *questions = root
        ? cJSON_GetObjectItemCaseSensitive(root, "questions")
        : NULL;

    int count = 0;

    if (cJSON_IsArray(questions)) {
        int total = cJSON_GetArraySize(questions);

        for (int i = 0; i < total && count < MAX_BANK; i++) {
            if (question_from_json(
                cJSON_GetArrayItem(questions, i),
                &bank[count]
            )) {
                count++;
            }
        }
    }

    cJSON_Delete(root);

    return count;
}

/* ================= SAVE QUESTION BANK ================= */

static int save_bank(
    const char *path,
    Question bank[MAX_BANK],
    int count
) {
    if (!path || !bank || count < 0 || count > MAX_BANK) {
        return 0;
    }

    cJSON *root = cJSON_CreateObject();

    if (!root) return 0;

    cJSON *questions = cJSON_AddArrayToObject(root, "questions");

    if (!questions) {
        cJSON_Delete(root);
        return 0;
    }

    for (int i = 0; i < count; i++) {
        cJSON *item = question_to_json(&bank[i]);

        if (!item) {
            cJSON_Delete(root);
            return 0;
        }

        cJSON_AddItemToArray(questions, item);
    }

    char *data = cJSON_Print(root);
    cJSON_Delete(root);

    if (!data) return 0;

    char temp_path[256];

    int written = snprintf(
        temp_path,
        sizeof(temp_path),
        "%s.tmp",
        path
    );

    if (written < 0 || (size_t)written >= sizeof(temp_path)) {
        free(data);
        return 0;
    }

    FILE *file = fopen(temp_path, "w");

    if (!file) {
        free(data);
        return 0;
    }

    int ok = fputs(data, file) >= 0;

    if (fclose(file) != 0) {
        ok = 0;
    }

    free(data);

    if (!ok || rename(temp_path, path) != 0) {
        remove(temp_path);
        return 0;
    }

    return 1;
}

/* ================= DUPLICATE CHECK ================= */

static int same_question(const Question *a, const Question *b) {
    if (!a || !b) return 0;

    const unsigned char *x =
        (const unsigned char *)a->question;

    const unsigned char *y =
        (const unsigned char *)b->question;

    while (*x || *y) {
        while (*x && !isalnum(*x)) x++;
        while (*y && !isalnum(*y)) y++;

        if (tolower(*x) != tolower(*y)) {
            return 0;
        }

        if (*x) x++;
        if (*y) y++;
    }

    return 1;
}

static int question_exists(
    const Question *question,
    const Question *bank,
    int count
) {
    for (int i = 0; i < count; i++) {
        if (same_question(question, &bank[i])) {
            return 1;
        }
    }

    return 0;
}

/* ================= SHUFFLE QUESTIONS ================= */

static void shuffle_questions(Question *questions, int count) {
    for (int i = count - 1; i > 0; i--) {
        int j = rand() % (i + 1);

        Question temp = questions[i];
        questions[i] = questions[j];
        questions[j] = temp;
    }
}

/* ================= LOAD HISTORY ================= */

static int load_history(
    HistoryRecord records[MAX_HISTORY]
) {
    FILE *file = fopen("history.json", "r");

    if (!file) return 0;

    if (fseek(file, 0, SEEK_END) != 0) {
        fclose(file);
        return 0;
    }

    long length = ftell(file);

    if (length <= 0 || length > 10000000 ||
        fseek(file, 0, SEEK_SET) != 0) {
        fclose(file);
        return 0;
    }

    char *data = malloc((size_t)length + 1);

    if (!data) {
        fclose(file);
        return 0;
    }

    size_t got = fread(data, 1, (size_t)length, file);
    data[got] = '\0';

    fclose(file);

    if (got != (size_t)length) {
        free(data);
        return 0;
    }

    cJSON *root = cJSON_Parse(data);
    free(data);

    int count = 0;

    if (cJSON_IsArray(root)) {
        cJSON *item;

        cJSON_ArrayForEach(item, root) {
            if (count >= MAX_HISTORY) break;

            cJSON *name = cJSON_GetObjectItemCaseSensitive(item, "name");
            cJSON *topic = cJSON_GetObjectItemCaseSensitive(item, "topic");
            cJSON *date = cJSON_GetObjectItemCaseSensitive(item, "date");
            cJSON *score = cJSON_GetObjectItemCaseSensitive(item, "score");
            cJSON *total = cJSON_GetObjectItemCaseSensitive(item, "total");
            cJSON *seconds = cJSON_GetObjectItemCaseSensitive(item, "seconds");
            cJSON *timestamp = cJSON_GetObjectItemCaseSensitive(item, "timestamp");

            if (cJSON_IsString(name) &&
                cJSON_IsString(topic) &&
                cJSON_IsString(date) &&
                cJSON_IsNumber(score) &&
                cJSON_IsNumber(total) &&
                cJSON_IsNumber(seconds) &&
                cJSON_IsNumber(timestamp)) {

                snprintf(records[count].name, NAME_LEN,
                         "%s", name->valuestring);

                snprintf(records[count].topic, 80,
                         "%s", topic->valuestring);

                snprintf(records[count].date, 40,
                         "%s", date->valuestring);

                records[count].score = score->valueint;
                records[count].total = total->valueint;
                records[count].seconds = seconds->valuedouble;
                records[count].timestamp =
                    (long)timestamp->valuedouble;

                count++;
            }
        }
    }

    cJSON_Delete(root);

    return count;
}

/* ================= SAVE HISTORY ================= */

static int save_history(
    HistoryRecord records[MAX_HISTORY],
    int count
) {
    cJSON *root = cJSON_CreateArray();

    if (!root) return 0;

    for (int i = 0; i < count; i++) {
        cJSON *item = cJSON_CreateObject();

        if (!item) {
            cJSON_Delete(root);
            return 0;
        }

        cJSON_AddStringToObject(item, "name", records[i].name);
        cJSON_AddStringToObject(item, "topic", records[i].topic);
        cJSON_AddNumberToObject(item, "score", records[i].score);
        cJSON_AddNumberToObject(item, "total", records[i].total);
        cJSON_AddNumberToObject(item, "seconds", records[i].seconds);
        cJSON_AddStringToObject(item, "date", records[i].date);
        cJSON_AddNumberToObject(item, "timestamp", records[i].timestamp);

        cJSON_AddItemToArray(root, item);
    }

    char *data = cJSON_Print(root);
    cJSON_Delete(root);

    if (!data) return 0;

    FILE *file = fopen("history.json.tmp", "w");

    if (!file) {
        free(data);
        return 0;
    }

    int ok = fputs(data, file) >= 0;

    if (fclose(file) != 0) ok = 0;

    free(data);

    if (!ok || rename("history.json.tmp", "history.json") != 0) {
        remove("history.json.tmp");
        return 0;
    }

    return 1;
}

/* ================= QUIZ START ================= */

int quiz_engine_start(
    WebQuizSession *session,
    const char *player,
    const char *topic,
    int question_count
) {
    if (!session || !player || !topic) return 0;

    if (question_count != 10 && question_count != 15) {
        return 0;
    }

    int topic_index = get_topic_index(topic);

    if (topic_index < 0) return 0;

    size_t player_length = strlen(player);

    if (player_length == 0 ||
        player_length >= sizeof(session->player)) {
        return 0;
    }

    int has_non_space = 0;

    for (size_t i = 0; player[i]; i++) {
        if (!isspace((unsigned char)player[i])) {
            has_non_space = 1;
            break;
        }
    }

    if (!has_non_space) return 0;

    pthread_mutex_lock(&quiz_start_mutex);

    int success = 0;
    Question *bank = NULL;

    memset(session, 0, sizeof(*session));

    snprintf(session->player, sizeof(session->player),
             "%s", player);

    snprintf(session->topic, sizeof(session->topic),
             "%s", field_names[topic_index]);

    mkdir("questions", 0777);

    bank = calloc(MAX_BANK, sizeof(Question));

    if (!bank) goto cleanup;

    int bank_count = load_bank(
        field_files[topic_index],
        bank
    );

    if (bank_count > 0) {
        shuffle_questions(bank, bank_count);
    }

    /*
       Required online split:
       10 questions = 5 local + 5 AI
       15 questions = 7 local + 8 AI
    */

    int bank_target = (question_count == 10) ? 5 : 7;
    int ai_needed = question_count - bank_target;

    /*
       Online mode must have the required number of local questions.
       Otherwise terminal.c can try offline mode.
    */

    if (bank_count < bank_target) {
        printf("\n[AI] Not enough local questions for the online split.\n");
        printf("[AI] Required local questions: %d\n", bank_target);
        printf("[AI] Available local questions: %d\n", bank_count);
        goto cleanup;
    }

    int local_count = bank_target;

    for (int i = 0; i < local_count; i++) {
        session->questions[i] = bank[i];
    }

    AiTask task;
    memset(&task, 0, sizeof(task));

    snprintf(task.topic, sizeof(task.topic),
             "%s", field_names[topic_index]);

    task.count = ai_needed;

    task.excluded_count = local_count;

    for (int i = 0; i < task.excluded_count; i++) {
        task.excluded[i] = bank[i];
    }

    remove("cache.json");

    printf("\n[AI] Generating %d AI questions...\n", ai_needed);

    generate_questions(&task);

    printf("[AI] Generation status: %s\n",
           task.success ? "SUCCESS" : "FAILED");

    printf("[AI] Questions received: %d/%d\n",
           task.generated_count, ai_needed);

    /*
       The online quiz requires the complete AI question count.
       Partial generation must not silently become a local-only quiz.
    */

    if (!task.success || task.generated_count != ai_needed) {
        printf("[AI] Required AI question count was not generated.\n");
        remove("cache.json");
        goto cleanup;
    }

    int accepted_ai = 0;

    for (int i = 0; i < task.generated_count; i++) {
        Question *candidate = &task.generated[i];

        /* Check against the complete existing JSON bank. */
        if (question_exists(candidate, bank, bank_count)) {
            printf("[AI] Duplicate with existing JSON bank skipped.\n");
            continue;
        }

        /* Check against questions already selected for this quiz. */
        if (question_exists(
            candidate,
            session->questions,
            local_count + accepted_ai
        )) {
            printf("[AI] Duplicate within generated questions skipped.\n");
            continue;
        }

        session->questions[local_count + accepted_ai] = *candidate;
        accepted_ai++;
    }

    printf("[AI] Unique questions accepted: %d/%d\n",
           accepted_ai, ai_needed);

    /*
       Do not start online mode if duplicates leave fewer AI questions
       than required. Terminal can then use offline fallback.
    */

    if (accepted_ai != ai_needed) {
        printf("[AI] Not enough unique AI questions were accepted.\n");
        remove("cache.json");
        goto cleanup;
    }

    /*
       Append accepted AI questions to the matching topic JSON file.
       Save result is checked before online quiz can start.
    */

    if (bank_count + accepted_ai > MAX_BANK) {
        printf("[SAVE] Question bank has reached its maximum capacity.\n");
        remove("cache.json");
        goto cleanup;
    }

    int saved_count = bank_count;

    for (int i = 0; i < accepted_ai; i++) {
        bank[saved_count++] =
            session->questions[local_count + i];
    }

    printf("[SAVE] Saving %d new AI questions to %s...\n",
           accepted_ai, field_files[topic_index]);

    int save_result = save_bank(
        field_files[topic_index],
        bank,
        saved_count
    );

    printf("[SAVE] Result: %s\n",
           save_result ? "SUCCESS" : "FAILED");

    printf("[SAVE] Total questions in JSON bank: %d\n",
           save_result ? saved_count : bank_count);

    remove("cache.json");

    if (!save_result) {
        printf("[SAVE] AI questions could not be saved. Online quiz cancelled.\n");
        goto cleanup;
    }

    /*
       At this point:
       - Required local questions are selected.
       - Required AI questions are selected.
       - All selected questions passed duplicate checks.
       - AI questions have been saved successfully.
    */

    session->total_questions = question_count;
    session->current_question = 0;
    session->correct_answers = 0;
    session->incorrect_answers = 0;
    session->completed = 0;

    struct timespec now;

    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) {
        goto cleanup;
    }

    session->start_time =
        (double)now.tv_sec +
        (double)now.tv_nsec / 1000000000.0;

    success = 1;

cleanup:
    free(bank);

    if (!success) {
        memset(session, 0, sizeof(*session));
    }

    pthread_mutex_unlock(&quiz_start_mutex);

    return success;
}

/* ================= SUBMIT ANSWER ================= */

/*
   Return values:
   1  = Correct answer
   0  = Incorrect answer
  -1  = Invalid answer or session
  -2  = Could not save completed quiz history
*/

int quiz_engine_submit_answer(
    WebQuizSession *session,
    char answer
) {
    if (!session || session->completed ||
        session->total_questions <= 0 ||
        session->total_questions > WEB_MAX_QUESTIONS ||
        session->current_question < 0 ||
        session->current_question >= session->total_questions) {
        return -1;
    }

    answer = (char)toupper((unsigned char)answer);

    if (answer < 'A' || answer > 'D') {
        return -1;
    }

    Question *current =
        &session->questions[session->current_question];

    int correct = (answer == current->correct_option);

    if (correct) {
        session->correct_answers++;
    } else {
        session->incorrect_answers++;
    }

    session->current_question++;

    if (session->current_question >= session->total_questions) {
        struct timespec now;

        if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) {
            session->completed = 1;
            return -2;
        }

        double end_time =
            (double)now.tv_sec +
            (double)now.tv_nsec / 1000000000.0;

        double duration = end_time - session->start_time;

        if (duration < 0) duration = 0;

        HistoryRecord *records =
            calloc(MAX_HISTORY, sizeof(HistoryRecord));

        if (!records) {
            session->completed = 1;
            return -2;
        }

        int saved = 0;

        pthread_mutex_lock(&history_mutex);

        int history_count = load_history(records);

        if (history_count < MAX_HISTORY) {
            HistoryRecord *record = &records[history_count];

            snprintf(
                record->name,
                sizeof(record->name),
                "%s",
                session->player
            );

            snprintf(
                record->topic,
                sizeof(record->topic),
                "%s",
                session->topic
            );

            record->score = session->correct_answers;
            record->total = session->total_questions;
            record->seconds = duration;

            time_t timestamp = time(NULL);
            struct tm local_time;

            record->timestamp = (long)timestamp;

            if (localtime_r(&timestamp, &local_time) != NULL) {
                strftime(
                    record->date,
                    sizeof(record->date),
                    "%d-%m-%Y %I:%M %p",
                    &local_time
                );

                saved = save_history(records, history_count + 1);
            }
        }

        pthread_mutex_unlock(&history_mutex);

        free(records);

        session->completed = 1;

        if (!saved) {
            return -2;
        }
    }

    return correct ? 1 : 0;
}

/* ================= OFFLINE TERMINAL QUIZ ================= */

int quiz_engine_start_offline(
    WebQuizSession *session,
    const char *player,
    const char *topic,
    int question_count
) {
    if (!session || !player || !topic) return 0;

    if (question_count != 10 && question_count != 15) {
        return 0;
    }

    int topic_index = get_topic_index(topic);

    if (topic_index < 0) return 0;

    size_t player_length = strlen(player);

    if (player_length == 0 ||
        player_length >= sizeof(session->player)) {
        return 0;
    }

    int has_non_space = 0;

    for (size_t i = 0; player[i]; i++) {
        if (!isspace((unsigned char)player[i])) {
            has_non_space = 1;
            break;
        }
    }

    if (!has_non_space) return 0;

    pthread_mutex_lock(&quiz_start_mutex);

    Question *bank = calloc(MAX_BANK, sizeof(Question));

    if (!bank) {
        pthread_mutex_unlock(&quiz_start_mutex);
        return 0;
    }

    int bank_count = load_bank(
        field_files[topic_index],
        bank
    );

    /*
     * The bank must contain enough valid entries to potentially
     * build the requested quiz. Duplicate entries are filtered below.
     */

    if (bank_count < question_count) {
        free(bank);
        pthread_mutex_unlock(&quiz_start_mutex);
        return 0;
    }

    shuffle_questions(bank, bank_count);

    memset(session, 0, sizeof(*session));

    snprintf(session->player, sizeof(session->player),
             "%s", player);

    snprintf(session->topic, sizeof(session->topic),
             "%s", field_names[topic_index]);

    int selected_count = 0;

    for (int i = 0;
         i < bank_count && selected_count < question_count;
         i++) {

        /* Skip a question if its normalized text is already selected. */
        if (question_exists(
            &bank[i],
            session->questions,
            selected_count
        )) {
            continue;
        }

        session->questions[selected_count] = bank[i];
        selected_count++;
    }

    /*
     * Do not start an incomplete quiz if the JSON bank does not
     * contain enough unique questions.
     */

    if (selected_count != question_count) {
        free(bank);
        pthread_mutex_unlock(&quiz_start_mutex);
        memset(session, 0, sizeof(*session));
        return 0;
    }

    session->total_questions = question_count;
    session->current_question = 0;
    session->correct_answers = 0;
    session->incorrect_answers = 0;
    session->completed = 0;

    struct timespec now;

    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) {
        free(bank);
        pthread_mutex_unlock(&quiz_start_mutex);
        memset(session, 0, sizeof(*session));
        return 0;
    }

    session->start_time =
        (double)now.tv_sec +
        (double)now.tv_nsec / 1000000000.0;

    free(bank);
    pthread_mutex_unlock(&quiz_start_mutex);

    return 1;
}
