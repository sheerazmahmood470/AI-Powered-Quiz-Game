
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
#include "storage.h"

#define MAX_BANK 2000
#define MAX_HISTORY 5000
#define MAX_AI_QUESTION_HISTORY 300
#define NAME_LEN 50

/* Protects history file operations */
static pthread_mutex_t history_mutex = PTHREAD_MUTEX_INITIALIZER;

/* Protects AI generation and shared cache/question-bank writes */
static pthread_mutex_t quiz_start_mutex = PTHREAD_MUTEX_INITIALIZER;

/* Render persistent disk path; falls back to the bundled history on first boot. */
static const char *history_file_path(void) {
    const char *configured = getenv("QUIZ_HISTORY_FILE");
    return (configured && configured[0]) ? configured : "history.json";
}

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


static int question_exists(const Question *question, const Question *bank, int count);
static void shuffle_questions(Question *questions, int count);

static char *questions_to_json_array(const Question *questions, int count) {
    cJSON *array = cJSON_CreateArray();
    if (!array) return NULL;
    for (int i = 0; i < count; i++) {
        cJSON *item = question_to_json(&questions[i]);
        if (!item) { cJSON_Delete(array); return NULL; }
        cJSON_AddItemToArray(array, item);
    }
    char *json = cJSON_PrintUnformatted(array);
    cJSON_Delete(array);
    return json;
}

static int parse_questions_json(const char *json, Question *destination, int capacity) {
    if (!json || !destination || capacity <= 0) return 0;
    cJSON *root = cJSON_Parse(json);
    int count = 0;
    if (cJSON_IsArray(root)) {
        int total = cJSON_GetArraySize(root);
        for (int i = 0; i < total && count < capacity; i++) {
            Question candidate;
            memset(&candidate, 0, sizeof(candidate));
            if (!question_from_json(cJSON_GetArrayItem(root, i), &candidate)) continue;
            if (question_exists(&candidate, destination, count)) continue;
            destination[count++] = candidate;
        }
    }
    cJSON_Delete(root);
    return count;
}


/* One non-blocking refill worker per topic. It adds a small validated batch
   while the current quiz is already running; reaching 2,000 is gradual and
   never makes quiz start wait for the AI providers. */
static pthread_mutex_t bank_refill_mutex = PTHREAD_MUTEX_INITIALIZER;
static int bank_refill_active[5];

typedef struct {
    int topic_index;
    int refill_count;
} QuestionRefillArgs;

static void *question_bank_refill_worker(void *argument) {
    QuestionRefillArgs args = *(QuestionRefillArgs *)argument;
    free(argument);
    int topic_index = args.topic_index;
    int target = args.refill_count;
    Question attempted[MAX_QUESTIONS];
    memset(attempted, 0, sizeof(attempted));
    int attempted_count = 0;
    int inserted_total = 0;

    fprintf(stderr, "[QUESTION BANK] Background Neon-only refill started for %s; target=%d.\n",
            field_names[topic_index], target);

    /* Each round invokes the complete five-provider fallback chain.
       Retry only the remaining shortage; never block the quiz request. */
    for (int attempt = 1; attempt <= 5 && inserted_total < target; attempt++) {
        AiTask task;
        memset(&task, 0, sizeof(task));
        snprintf(task.topic, sizeof(task.topic), "%s", field_names[topic_index]);
        task.count = target - inserted_total;
        task.excluded_count = attempted_count < 8 ? attempted_count : 8;
        for (int i = 0; i < task.excluded_count; i++)
            task.excluded[i] = attempted[i];

        generate_questions(&task);
        if (!task.success || task.generated_count <= 0) {
            fprintf(stderr, "[QUESTION BANK] %s refill attempt %d returned no valid questions.\n",
                    field_names[topic_index], attempt);
            continue;
        }

        int batch_count = task.generated_count;
        if (batch_count > target - inserted_total)
            batch_count = target - inserted_total;

        /* Remember attempted questions so a later retry asks for different ones. */
        for (int i = 0; i < batch_count && attempted_count < MAX_QUESTIONS; i++)
            attempted[attempted_count++] = task.generated[i];

        char *json = questions_to_json_array(task.generated, batch_count);
        int inserted = json
            ? storage_add_questions_count_json(field_names[topic_index], json) : 0;
        free(json);
        if (inserted > 0) inserted_total += inserted;

        fprintf(stderr, "[QUESTION BANK] %s attempt %d inserted %d new Neon question(s); progress %d/%d.\n",
                field_names[topic_index], attempt, inserted, inserted_total, target);
        if (inserted == 0) continue;
    }

    if (inserted_total < target) {
        fprintf(stderr, "[QUESTION BANK] %s refill incomplete: %d/%d inserted. Remaining shortage will be retried after a later quiz.\n",
                field_names[topic_index], inserted_total, target);
    } else {
        fprintf(stderr, "[QUESTION BANK] %s refill complete: %d/%d new questions saved in Neon.\n",
                field_names[topic_index], inserted_total, target);
    }

    pthread_mutex_lock(&bank_refill_mutex);
    bank_refill_active[topic_index] = 0;
    pthread_mutex_unlock(&bank_refill_mutex);
    return NULL;
}

static void schedule_question_bank_refill(int topic_index, int refill_count) {
    if (!storage_is_configured() || topic_index < 0 || topic_index >= 5 ||
        refill_count <= 0 || refill_count > MAX_QUESTIONS) return;

    pthread_mutex_lock(&bank_refill_mutex);
    if (bank_refill_active[topic_index]) {
        pthread_mutex_unlock(&bank_refill_mutex);
        return;
    }
    bank_refill_active[topic_index] = 1;
    pthread_mutex_unlock(&bank_refill_mutex);

    QuestionRefillArgs *argument = malloc(sizeof(*argument));
    if (!argument) {
        pthread_mutex_lock(&bank_refill_mutex);
        bank_refill_active[topic_index] = 0;
        pthread_mutex_unlock(&bank_refill_mutex);
        return;
    }
    argument->topic_index = topic_index;
    argument->refill_count = refill_count;
    pthread_t worker;
    if (pthread_create(&worker, NULL, question_bank_refill_worker, argument) == 0) {
        pthread_detach(worker);
    } else {
        free(argument);
        pthread_mutex_lock(&bank_refill_mutex);
        bank_refill_active[topic_index] = 0;
        pthread_mutex_unlock(&bank_refill_mutex);
    }
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


/* ================= ROTATING AI QUESTION CACHE =================
   Keep the previous complete batch in Neon as a no-repeat reference.
   Every new quiz generates one fresh full batch (never one request/question).
   The browser still receives the whole batch at quiz start for instant navigation.
*/
static Question process_question_cache[5][MAX_AI_QUESTION_HISTORY];
static int process_question_cache_count[5];
static int process_question_cache_ready[5];

static int load_ai_question_cache(
    int topic_index,
    Question *destination
) {
    if (topic_index < 0 || topic_index >= 5 || !destination) return 0;
    char key[80];
    snprintf(key, sizeof(key), "ai_questions_v2_%d", topic_index);
    char *data = storage_is_configured() ? storage_read_json(key) : NULL;
    if (!data) {
        /* Seed the usable cache from the bundled topic JSON bank on first use.
           This gives instant quizzes even when every external AI provider is down. */
        int seeded = load_bank(field_files[topic_index], destination);
        if (seeded > 0) {
            fprintf(stderr, "[CACHE] Loaded %d bundled %s question(s) as the initial local bank.\n",
                    seeded, field_names[topic_index]);
        }
        return seeded;
    }
    cJSON *root = cJSON_Parse(data);
    free(data);
    cJSON *items = root
        ? cJSON_GetObjectItemCaseSensitive(root, "questions") : NULL;
    int accepted = 0;
    if (cJSON_IsArray(items)) {
        int total = cJSON_GetArraySize(items);
        for (int i = 0; i < total && accepted < MAX_AI_QUESTION_HISTORY; i++) {
            Question candidate;
            memset(&candidate, 0, sizeof(candidate));
            if (!question_from_json(cJSON_GetArrayItem(items, i), &candidate))
                continue;
            if (question_exists(&candidate, destination, accepted))
                continue;
            destination[accepted++] = candidate;
        }
    }
    cJSON_Delete(root);
    return accepted;
}

static int save_ai_question_cache(
    int topic_index,
    int question_count,
    const Question *questions
) {
    if (!storage_is_configured() || !questions) return 0;
    char key[80];
    snprintf(key, sizeof(key), "ai_questions_v2_%d", topic_index);
    cJSON *root = cJSON_CreateObject();
    cJSON *items = root ? cJSON_AddArrayToObject(root, "questions") : NULL;
    if (!items) { cJSON_Delete(root); return 0; }
    for (int i = 0; i < question_count; i++) {
        cJSON *item = question_to_json(&questions[i]);
        if (!item) { cJSON_Delete(root); return 0; }
        cJSON_AddItemToArray(items, item);
    }
    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!json) return 0;
    int saved = storage_write_json(key, json);
    free(json);
    return saved;
}

static int generate_fresh_batch(
    int topic_index,
    int question_count,
    const Question *previous,
    int previous_count,
    Question *destination
) {
    int accepted = 0;

    /* Keep valid questions from each response. Never throw away a useful
       partial batch just because some generated questions were duplicates. */
    for (int attempt = 1; attempt <= 5 && accepted < question_count; attempt++) {
        AiTask task;
        memset(&task, 0, sizeof(task));
        snprintf(task.topic, sizeof(task.topic), "%s", field_names[topic_index]);

        int remaining = question_count - accepted;
        task.count = remaining;

        /* The prompt supports up to eight exclusions. Prioritize questions
           already accepted for this quiz, then rotate through the old history. */
        for (int i = 0; i < accepted && task.excluded_count < 8; i++) {
            task.excluded[task.excluded_count++] = destination[i];
        }

        int available_previous = previous ? previous_count : 0;
        int previous_slots = 8 - task.excluded_count;
        if (available_previous > 0 && previous_slots > 0) {
            int start = ((attempt - 1) * previous_slots) % available_previous;
            for (int offset = 0;
                 offset < available_previous && task.excluded_count < 8;
                 offset++) {
                int source = (start + offset) % available_previous;
                if (question_exists(&previous[source],
                                    task.excluded, task.excluded_count)) {
                    continue;
                }
                task.excluded[task.excluded_count++] = previous[source];
            }
        }

        printf("[AI] Generating batch attempt %d/5: requesting %d remaining %s questions.\n",
               attempt, remaining, field_names[topic_index]);

        generate_questions(&task);

        if (!task.success || task.generated_count <= 0) {
            fprintf(stderr, "[AI] No provider returned usable questions; stopping instead of repeating the full provider chain.\n");
            return 0;
        }

        int before = accepted;
        for (int i = 0; i < task.generated_count && accepted < question_count; i++) {
            const Question *candidate = &task.generated[i];

            if (previous &&
                question_exists(candidate, previous, previous_count)) {
                continue;
            }

            if (question_exists(candidate, destination, accepted)) {
                continue;
            }

            destination[accepted++] = *candidate;
        }

        fprintf(stderr, "[AI] Unique questions collected: %d/%d (added %d on attempt %d).\n",
                accepted, question_count, accepted - before, attempt);
    }

    if (accepted != question_count) {
        fprintf(stderr, "[AI] Quiz cancelled: collected only %d of %d unique questions after 5 attempts.\n",
                accepted, question_count);
        return 0;
    }

    return 1;
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
    char *data = NULL;
    if (storage_is_configured()) {
        data = storage_read_history_json();
        if (!data) return -1;
    } else {
        FILE *file = fopen(history_file_path(), "r");
        if (!file) return 0;
        if (fseek(file, 0, SEEK_END) != 0) { fclose(file); return 0; }
        long length = ftell(file);
        if (length <= 0 || length > 10000000 || fseek(file, 0, SEEK_SET) != 0) {
            fclose(file); return 0;
        }
        data = malloc((size_t)length + 1);
        if (!data) { fclose(file); return 0; }
        size_t got = fread(data, 1, (size_t)length, file);
        data[got] = '\0';
        fclose(file);
        if (got != (size_t)length) { free(data); return 0; }
    }
    cJSON *root = cJSON_Parse(data);
    free(data);
    int count = 0;

    if (!cJSON_IsArray(root)) {
        cJSON_Delete(root);
        return -1; /* Never overwrite a damaged history file as if it were empty. */
    }

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

    if (storage_is_configured()) {
        int ok = storage_write_json("history", data);
        free(data);
        return ok;
    }

    char temp_path[1200];
    int path_length = snprintf(temp_path, sizeof(temp_path), "%s.tmp",
                               history_file_path());
    if (path_length < 0 || (size_t)path_length >= sizeof(temp_path)) {
        free(data);
        return 0;
    }

    FILE *file = fopen(temp_path, "w");

    if (!file) {
        free(data);
        return 0;
    }

    int ok = fputs(data, file) >= 0;

    if (fclose(file) != 0) ok = 0;

    free(data);

    if (!ok || rename(temp_path, history_file_path()) != 0) {
        remove(temp_path);
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
    if (question_count != 10 && question_count != 15) return 0;

    int topic_index = get_topic_index(topic);
    if (topic_index < 0) return 0;

    size_t player_length = strlen(player);
    if (player_length == 0 || player_length >= sizeof(session->player)) return 0;

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
    memset(session, 0, sizeof(*session));
    snprintf(session->player, sizeof(session->player), "%s", player);
    snprintf(session->topic, sizeof(session->topic), "%s", field_names[topic_index]);


    Question fresh_batch[WEB_MAX_QUESTIONS];
    memset(fresh_batch, 0, sizeof(fresh_batch));

    if (storage_is_configured()) {
        /* Every quiz question comes from Neon only. The atomic claim marks
           each selected row used permanently, preventing repeat delivery. */
        int selected_count = 0;
        char *stored = storage_take_questions_json(field_names[topic_index], question_count);
        if (stored) {
            selected_count = parse_questions_json(stored, fresh_batch, question_count);
            free(stored);
        }

        /* Refill this exact topic in the background by the number consumed.
           The quiz never waits for any external AI provider. */
        if (selected_count > 0)
            schedule_question_bank_refill(topic_index, selected_count);

        /* Used questions stay retired; never recycle them into the active pool. */
        if (selected_count != question_count) {
            fprintf(stderr, "[QUESTION BANK] Background refill is not ready; only %d/%d unique questions are available.\\n",
                    selected_count, question_count);
            goto cleanup;
        }
        shuffle_questions(fresh_batch, question_count);
    } else {
            Question previous_batch[MAX_AI_QUESTION_HISTORY];
            Question fresh_batch[WEB_MAX_QUESTIONS];
            memset(previous_batch, 0, sizeof(previous_batch));
            memset(fresh_batch, 0, sizeof(fresh_batch));
        
            int previous_count = 0;
            if (process_question_cache_ready[topic_index]) {
                previous_count = process_question_cache_count[topic_index];
                memcpy(previous_batch, process_question_cache[topic_index],
                       (size_t)previous_count * sizeof(Question));
            } else {
                previous_count = load_ai_question_cache(topic_index, previous_batch);
                if (previous_count > 0) {
                    memcpy(process_question_cache[topic_index], previous_batch,
                           (size_t)previous_count * sizeof(Question));
                    process_question_cache_count[topic_index] = previous_count;
                    process_question_cache_ready[topic_index] = 1;
                }
            }
        
            /* Neon cache is a usable question bank, not only a no-repeat blacklist.
               Consume cached questions first so quiz start does not wait for an AI call.
               Rotate consumed questions to the end; newly generated questions refill it. */
            int cached_count = previous_count < question_count ? previous_count : question_count;
            if (cached_count > 0) {
                memcpy(fresh_batch, previous_batch,
                       (size_t)cached_count * sizeof(Question));
        
                if (previous_count > 1) {
                    Question consumed[WEB_MAX_QUESTIONS];
                    int rotate_by = cached_count % previous_count;
                    if (rotate_by == 0) rotate_by = 1;
                    if (rotate_by > previous_count) rotate_by = previous_count;
                    int moved = 0;
                    while (moved < rotate_by) {
                        consumed[moved] = previous_batch[moved];
                        moved++;
                    }
                    memmove(previous_batch, previous_batch + rotate_by,
                            (size_t)(previous_count - rotate_by) * sizeof(Question));
                    memcpy(previous_batch + previous_count - rotate_by, consumed,
                           (size_t)rotate_by * sizeof(Question));
                }
                fprintf(stderr, "[CACHE] Reusing %d stored %s question(s); AI needed for %d.\n",
                        cached_count, field_names[topic_index], question_count - cached_count);
            }
        
            int needed = question_count - cached_count;
            int retained_count = previous_count;
            if (needed > 0) {
                Question newly_generated[WEB_MAX_QUESTIONS];
                memset(newly_generated, 0, sizeof(newly_generated));
        
                if (!generate_fresh_batch(
                        topic_index, needed,
                        previous_count ? previous_batch : NULL,
                        previous_count, newly_generated)) {
                    fprintf(stderr, "[AI] Quiz cancelled: cache supplied %d/%d questions and AI could not fill the remaining %d.\n",
                            cached_count, question_count, needed);
                    goto cleanup;
                }
        
                memcpy(fresh_batch + cached_count, newly_generated,
                       (size_t)needed * sizeof(Question));
        
                if (retained_count + needed > MAX_AI_QUESTION_HISTORY) {
                    int drop_count = retained_count + needed - MAX_AI_QUESTION_HISTORY;
                    memmove(previous_batch, previous_batch + drop_count,
                            (size_t)(retained_count - drop_count) * sizeof(Question));
                    retained_count -= drop_count;
                }
                memcpy(previous_batch + retained_count, newly_generated,
                       (size_t)needed * sizeof(Question));
                retained_count += needed;
            }
        
            /* Shuffle only the quiz copy; keep the persistent bank's queue order. */
            shuffle_questions(fresh_batch, question_count);
        
            memcpy(process_question_cache[topic_index], previous_batch,
                   (size_t)retained_count * sizeof(Question));
            process_question_cache_count[topic_index] = retained_count;
            process_question_cache_ready[topic_index] = 1;
        
            if (save_ai_question_cache(topic_index, retained_count, previous_batch)) {
                printf("[CACHE] Saved rolling history (%d questions) for %s; current %d-question quiz batch is ready.\n",
                       retained_count, field_names[topic_index], question_count);
            } else {
                fprintf(stderr, "[CACHE] Persistent no-repeat reference could not be updated; current batch remains available in memory.\n");
            }
        
        
    }

    for (int i = 0; i < question_count; i++) {
        session->questions[i] = fresh_batch[i];
    }

    session->total_questions = question_count;
    session->current_question = 0;
    session->correct_answers = 0;
    session->incorrect_answers = 0;
    session->completed = 0;

    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) {
        fprintf(stderr, "[AI] Could not initialize quiz timer.\n");
        goto cleanup;
    }

    session->start_time =
        (double)now.tv_sec + (double)now.tv_nsec / 1000000000.0;
    success = 1;

cleanup:
    if (!success) memset(session, 0, sizeof(*session));
    pthread_mutex_unlock(&quiz_start_mutex);
    return success;
}

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

        int saved = 0;
        time_t timestamp = time(NULL);
        struct tm local_time;
        char date[40] = "Unavailable";
        if (localtime_r(&timestamp, &local_time) != NULL) {
            strftime(date, sizeof(date), "%d-%m-%Y %I:%M %p", &local_time);
            HistoryRecord record;
            memset(&record, 0, sizeof(record));
            snprintf(record.name, sizeof(record.name), "%s", session->player);
            snprintf(record.topic, sizeof(record.topic), "%s", session->topic);
            record.score=session->correct_answers; record.total=session->total_questions;
            record.seconds=duration; record.timestamp=(long)timestamp;
            if(storage_is_configured()) {
                cJSON *item=cJSON_CreateObject();
                if(item) {
                    cJSON_AddStringToObject(item,"name",record.name);
                    cJSON_AddStringToObject(item,"topic",record.topic);
                    cJSON_AddNumberToObject(item,"score",record.score);
                    cJSON_AddNumberToObject(item,"total",record.total);
                    cJSON_AddNumberToObject(item,"seconds",record.seconds);
                    cJSON_AddStringToObject(item,"date",date);
                    cJSON_AddNumberToObject(item,"timestamp",record.timestamp);
                    char *json=cJSON_PrintUnformatted(item);
                    if(json){saved=storage_append_history_json(json);free(json);}
                    cJSON_Delete(item);
                }
            } else {
                HistoryRecord *records=calloc(MAX_HISTORY,sizeof(HistoryRecord));
                if(records) {
                    pthread_mutex_lock(&history_mutex);
                    int history_count=load_history(records);
                    if(history_count>=0&&history_count<MAX_HISTORY) {
                        records[history_count]=record;
                        snprintf(records[history_count].date,sizeof(records[history_count].date),"%s",date);
                        saved=save_history(records,history_count+1);
                    }
                    pthread_mutex_unlock(&history_mutex);
                    free(records);
                }
            }
        }

        session->completed = 1;

        if (!saved) {
            return -2;
        }
    }

    return correct ? 1 : 0;
}


/* Validate and score an entire quiz in one operation; persist history once. */
int quiz_engine_submit_answers(WebQuizSession *session, const char *answers, int answer_count) {
    if (!session || !answers || session->completed || session->current_question != 0 ||
        session->total_questions <= 0 || session->total_questions > WEB_MAX_QUESTIONS ||
        answer_count != session->total_questions) return -1;
    for (int i = 0; i < answer_count; i++) {
        char answer = (char)toupper((unsigned char)answers[i]);
        if (answer < 'A' || answer > 'D') return -1;
    }
    int final_result = 0;
    for (int i = 0; i < answer_count; i++) {
        final_result = quiz_engine_submit_answer(session, answers[i]);
        if (final_result == -1) return -1;
        if (i < answer_count - 1 && session->completed) return -1;
    }
    return final_result == -2 ? -2 : 1;
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
