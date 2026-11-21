#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <time.h>
#include <pthread.h>
#include <sys/stat.h>
#include <cjson/cJSON.h>
#include <curl/curl.h>
#include "ai.h"

#define MAX_BANK 2000
#define MAX_HISTORY 5000
#define NAME_LEN 50

typedef struct {
    char name[NAME_LEN];
    int score;
    int total;
    double seconds;
    char topic[80];
    char date[40];
    long timestamp;
} HistoryRecord;

static const char *field_names[5] = {"Philosophy", "AI and ML", "History", "C programming", "Finance"};
static const char *field_files[5] = {"questions/philosophy.json", "questions/ai_ml.json", "questions/history.json", "questions/c_programming.json", "questions/finance.json"};

static void clear_input_line(void) {
    int ch;
    while ((ch = getchar()) != '\n' && ch != EOF) {}
}

static int read_line(char *buffer, size_t size) {
    if (!fgets(buffer, (int)size, stdin)) return 0;
    size_t len = strlen(buffer);
    if (len && buffer[len - 1] == '\n') buffer[len - 1] = '\0';
    else clear_input_line();
    return 1;
}

static int get_number(const char *message, int min, int max) {
    char input[100];
    while (1) {
        printf("%s", message);
        if (!read_line(input, sizeof(input))) return -1;
        if (strlen(input) == 1 && input[0] >= '0' + min && input[0] <= '0' + max)
            return input[0] - '0';
        printf("Invalid input. Enter a number from %d to %d.\n", min, max);
    }
}

static void display_menu(void) {
    printf("\n       WELCOME TO AI QUIZ GAME\n");
    printf("====================================\n");
    printf("1. Start New Quiz\n2. View High Scores\n3. View Quiz History\n4. Exit\n");
}

static int choose_field(void) {
    printf("\n       CHOOSE A PARTICULAR FIELD\n====================================\n");
    for (int i = 0; i < 5; i++) printf("%d. %s\n", i + 1, field_names[i]);
    int choice = get_number("Choose a topic (1-5): ", 1, 5);
    return choice - 1;
}

static int get_player_name(char name[NAME_LEN]) {
    while (1) {
        printf("Enter player name (maximum 49 characters): ");
        if (!read_line(name, NAME_LEN)) return 0;
        int has_non_space = 0;
        for (size_t i = 0; name[i]; i++) if (!isspace((unsigned char)name[i])) has_non_space = 1;
        if (has_non_space) return 1;
        printf("Name cannot be empty or contain only spaces. Try again.\n");
    }
}

static int question_from_json(cJSON *item, Question *q) {
    cJSON *question = cJSON_GetObjectItemCaseSensitive(item, "question");
    cJSON *options = cJSON_GetObjectItemCaseSensitive(item, "options");
    cJSON *correct = cJSON_GetObjectItemCaseSensitive(item, "correct_option");
    cJSON *explanation = cJSON_GetObjectItemCaseSensitive(item, "explanation");
    if (!cJSON_IsString(question) || !cJSON_IsArray(options) || cJSON_GetArraySize(options) != 4 ||
        !cJSON_IsString(correct) || strlen(correct->valuestring) != 1 || !cJSON_IsString(explanation)) return 0;
    char answer = (char)toupper((unsigned char)correct->valuestring[0]);
    if (answer < 'A' || answer > 'D' || strlen(question->valuestring) >= QUESTION_LEN ||
        strlen(explanation->valuestring) >= EXPLANATION_LEN) return 0;
    strcpy(q->question, question->valuestring);
    strcpy(q->explanation, explanation->valuestring);
    q->correct_option = answer;
    for (int i = 0; i < 4; i++) {
        cJSON *option = cJSON_GetArrayItem(options, i);
        if (!cJSON_IsString(option) || strlen(option->valuestring) >= OPTION_LEN) return 0;
        strcpy(q->options[i], option->valuestring);
    }
    return 1;
}

static cJSON *question_to_json(const Question *q) {
    cJSON *item = cJSON_CreateObject();
    cJSON_AddStringToObject(item, "question", q->question);
    cJSON *options = cJSON_AddArrayToObject(item, "options");
    for (int i = 0; i < 4; i++) cJSON_AddItemToArray(options, cJSON_CreateString(q->options[i]));
    char answer[2] = {q->correct_option, '\0'};
    cJSON_AddStringToObject(item, "correct_option", answer);
    cJSON_AddStringToObject(item, "explanation", q->explanation);
    return item;
}

static int load_bank(const char *path, Question bank[MAX_BANK]) {
    FILE *file = fopen(path, "r");
    if (!file) return 0;
    fseek(file, 0, SEEK_END);
    long length = ftell(file);
    rewind(file);
    if (length <= 0 || length > 5000000) { fclose(file); return 0; }
    char *data = malloc((size_t)length + 1);
    if (!data) { fclose(file); return 0; }
    size_t got = fread(data, 1, (size_t)length, file);
    data[got] = '\0';
    fclose(file);
    cJSON *root = cJSON_Parse(data);
    free(data);
    cJSON *questions = root ? cJSON_GetObjectItemCaseSensitive(root, "questions") : NULL;
    int count = 0;
    if (cJSON_IsArray(questions)) {
        int total = cJSON_GetArraySize(questions);
        for (int i = 0; i < total && count < MAX_BANK; i++)
            if (question_from_json(cJSON_GetArrayItem(questions, i), &bank[count])) count++;
    }
    cJSON_Delete(root);
    return count;
}

static int save_bank(const char *path, Question bank[MAX_BANK], int count) {
    cJSON *root = cJSON_CreateObject();
    cJSON *questions = cJSON_AddArrayToObject(root, "questions");
    for (int i = 0; i < count; i++) cJSON_AddItemToArray(questions, question_to_json(&bank[i]));
    char *data = cJSON_Print(root);
    cJSON_Delete(root);
    if (!data) return 0;
    char temp_path[256];
    snprintf(temp_path, sizeof(temp_path), "%s.tmp", path);
    FILE *file = fopen(temp_path, "w");
    if (!file) { free(data); return 0; }
    int ok = fputs(data, file) >= 0;
    if (fclose(file) != 0) ok = 0;
    free(data);
    if (!ok || rename(temp_path, path) != 0) { remove(temp_path); return 0; }
    return 1;
}

static void shuffle_questions(Question *questions, int count) {
    for (int i = count - 1; i > 0; i--) {
        int j = rand() % (i + 1);
        Question temp = questions[i]; questions[i] = questions[j]; questions[j] = temp;
    }
}

static int load_history(HistoryRecord records[MAX_HISTORY]) {
    FILE *file = fopen("history.json", "r");
    if (!file) return 0;
    fseek(file, 0, SEEK_END); long length = ftell(file); rewind(file);
    if (length <= 0 || length > 10000000) { fclose(file); return 0; }
    char *data = malloc((size_t)length + 1);
    if (!data) { fclose(file); return 0; }
    size_t got = fread(data, 1, (size_t)length, file); data[got] = '\0'; fclose(file);
    cJSON *root = cJSON_Parse(data); free(data);
    int count = 0;
    if (cJSON_IsArray(root)) {
        cJSON *item;
        cJSON_ArrayForEach(item, root) {
            if (count >= MAX_HISTORY) break;
            cJSON *name = cJSON_GetObjectItem(item, "name");
            cJSON *topic = cJSON_GetObjectItem(item, "topic");
            cJSON *date = cJSON_GetObjectItem(item, "date");
            cJSON *score = cJSON_GetObjectItem(item, "score");
            cJSON *total = cJSON_GetObjectItem(item, "total");
            cJSON *seconds = cJSON_GetObjectItem(item, "seconds");
            cJSON *timestamp = cJSON_GetObjectItem(item, "timestamp");
            if (cJSON_IsString(name) && cJSON_IsString(topic) && cJSON_IsString(date) &&
                cJSON_IsNumber(score) && cJSON_IsNumber(total) && cJSON_IsNumber(seconds) && cJSON_IsNumber(timestamp)) {
                snprintf(records[count].name, NAME_LEN, "%s", name->valuestring);
                snprintf(records[count].topic, sizeof(records[count].topic), "%s", topic->valuestring);
                snprintf(records[count].date, sizeof(records[count].date), "%s", date->valuestring);
                records[count].score = score->valueint; records[count].total = total->valueint;
                records[count].seconds = seconds->valuedouble; records[count].timestamp = (long)timestamp->valuedouble;
                count++;
            }
        }
    }
    cJSON_Delete(root);
    return count;
}

static int save_history(HistoryRecord records[MAX_HISTORY], int count) {
    cJSON *root = cJSON_CreateArray();
    for (int i = 0; i < count; i++) {
        cJSON *item = cJSON_CreateObject();
        cJSON_AddStringToObject(item, "name", records[i].name);
        cJSON_AddStringToObject(item, "topic", records[i].topic);
        cJSON_AddNumberToObject(item, "score", records[i].score);
        cJSON_AddNumberToObject(item, "total", records[i].total);
        cJSON_AddNumberToObject(item, "seconds", records[i].seconds);
        cJSON_AddStringToObject(item, "date", records[i].date);
        cJSON_AddNumberToObject(item, "timestamp", records[i].timestamp);
        cJSON_AddItemToArray(root, item);
    }
    char *data = cJSON_Print(root); cJSON_Delete(root);
    if (!data) return 0;
    FILE *file = fopen("history.json.tmp", "w");
    if (!file) { free(data); return 0; }
    int ok = fputs(data, file) >= 0;
    if (fclose(file) != 0) ok = 0;
    free(data);
    if (!ok || rename("history.json.tmp", "history.json") != 0) { remove("history.json.tmp"); return 0; }
    return 1;
}

static int compare_history_newest(const void *a, const void *b) {
    const HistoryRecord *x = a, *y = b;
    if (x->timestamp < y->timestamp) return 1;
    if (x->timestamp > y->timestamp) return -1;
    return 0;
}

static int compare_scores(const void *a, const void *b) {
    const HistoryRecord *x = a, *y = b;
    if (x->score < y->score) return 1;
    if (x->score > y->score) return -1;
    if (x->seconds > y->seconds) return 1;
    if (x->seconds < y->seconds) return -1;
    if (x->timestamp < y->timestamp) return 1;
    if (x->timestamp > y->timestamp) return -1;
    return 0;
}

static void print_record(const HistoryRecord *r) {
    int whole_seconds = (int)(r->seconds + 0.5);
    int mins = whole_seconds / 60;
    int secs = whole_seconds % 60;
    double average = r->total > 0 ? r->seconds / r->total : 0;
    printf("%-16s %-15s %d/%d  %d min %d sec  avg %.1f sec  %s\n",
           r->name, r->topic, r->score, r->total, mins, secs, average, r->date);
}

static void view_history(void) {
    HistoryRecord records[MAX_HISTORY]; int count = load_history(records);
    if (!count) { printf("\nNo quiz history found yet.\n"); return; }
    qsort(records, (size_t)count, sizeof(records[0]), compare_history_newest);
    printf("\nQUIZ HISTORY (newest first)\n");
    for (int i = 0; i < count; i++) { printf("%d. ", i + 1); print_record(&records[i]); }
}

static void view_high_scores(void) {
    HistoryRecord all[MAX_HISTORY];
    HistoryRecord filtered[MAX_HISTORY];

    int count = load_history(all);

    if (count == 0) {
        printf("\nNo high scores found yet.\n");
        return;
    }

    printf("\n========== ALL HIGH SCORES ==========\n");

    for (int topic = 0; topic < 5; topic++) {
        int used = 0;

        for (int i = 0; i < count; i++) {
            if (strcmp(all[i].topic, field_names[topic]) == 0) {
                filtered[used++] = all[i];
            }
        }

        printf("\nTOPIC: %s\n", field_names[topic]);
        printf("---------------------------------------------\n");

        if (used == 0) {
            printf("No scores available for this topic.\n");
            continue;
        }

        qsort(filtered, (size_t)used,
              sizeof(filtered[0]), compare_scores);

        printf("PLAYER           TOPIC           SCORE  TIME / AVERAGE       DATE\n");

        for (int i = 0; i < used; i++) {
            print_record(&filtered[i]);
        }
    }
}
static void play_quiz(const char *player, int topic_index) {
    Question bank[MAX_BANK], selected[5], quiz[MAX_QUESTIONS];
    int bank_count = load_bank(field_files[topic_index], bank);
    FILE *bank_check = fopen(field_files[topic_index], "r");
    if (!bank_check) save_bank(field_files[topic_index], bank, 0);
    else fclose(bank_check);
    if (bank_count > 0) {
        shuffle_questions(bank, bank_count);
    }
    int local_count = bank_count < 5 ? bank_count : 5;
    for (int i = 0; i < local_count; i++) { selected[i] = bank[i]; quiz[i] = bank[i]; }
    int ai_needed = 10 - local_count;

    AiTask task;
    memset(&task, 0, sizeof(task));
    snprintf(task.topic, sizeof(task.topic), "%s", field_names[topic_index]);
    task.count = ai_needed;
    task.excluded_count = local_count;
    for (int i = 0; i < local_count; i++) task.excluded[i] = selected[i];

    pthread_t ai_thread;
    int thread_started = 0;
    if (ai_needed > 0) {
        remove("cache.json");
        if (pthread_create(&ai_thread, NULL, generate_questions, &task) == 0) thread_started = 1;
        else printf("Could not start AI question generation.\n");
    }

    printf("\nStarting quiz for %s — %s\n", player, field_names[topic_index]);
    printf("Local questions will appear first.\n");
    int score = 0, answered = 0;
    char user_answers[MAX_QUESTIONS] = {0};
    double active_seconds = 0;
    struct timespec start, stop;
    clock_gettime(CLOCK_MONOTONIC, &start);

    for (int i = 0; i < local_count; i++) {
        printf("\nQuestion %d/%d (Local)\n%s\n", i + 1, 10, quiz[i].question);
        for (int j = 0; j < 4; j++) printf("%c) %s\n", 'A' + j, quiz[i].options[j]);
        char input[100]; char answer;
        while (1) {
            printf("Your answer (A/B/C/D): ");
            if (!read_line(input, sizeof(input))) input[0] = '\0';
            if (strlen(input) == 1 && strchr("ABCDabcd", input[0])) { answer = (char)toupper((unsigned char)input[0]); break; }
            printf("Invalid input. Enter A, B, C, or D.\n");
        }
        user_answers[i] = answer;
        answered++;
        if (answer == quiz[i].correct_option) { score++; printf("Correct!\n"); }
        else printf("Incorrect!\n");
        printf("Current score: %d/%d\n", score, answered);
    }

    clock_gettime(CLOCK_MONOTONIC, &stop);
    active_seconds += (stop.tv_sec - start.tv_sec) + (stop.tv_nsec - start.tv_nsec) / 1000000000.0;

    if (thread_started) {
        printf("\nPreparing AI questions (maximum response time: 20 seconds)...\n");
        pthread_join(ai_thread, NULL); /* The request itself has a 20-second total timeout. */
    }

    int ai_count = task.success ? task.generated_count : 0;
    if (ai_count > 0) {
        for (int i = 0; i < ai_count; i++) quiz[local_count + i] = task.generated[i];
        printf("AI questions are ready.\n");
    } else if (local_count > 0) {
        printf("AI questions could not be generated. Continuing with available local questions.\n");
    }

    int total_questions = local_count + ai_count;
    if (total_questions == 0) {
        printf("No local questions are available and AI generation failed. Returning to main menu.\n");
        return;
    }

    clock_gettime(CLOCK_MONOTONIC, &start);
    for (int i = local_count; i < total_questions; i++) {
        printf("\nQuestion %d/%d (AI)\n%s\n", i + 1, total_questions, quiz[i].question);
        for (int j = 0; j < 4; j++) printf("%c) %s\n", 'A' + j, quiz[i].options[j]);
        char input[100]; char answer;
        while (1) {
            printf("Your answer (A/B/C/D): ");
            if (!read_line(input, sizeof(input))) input[0] = '\0';
            if (strlen(input) == 1 && strchr("ABCDabcd", input[0])) { answer = (char)toupper((unsigned char)input[0]); break; }
            printf("Invalid input. Enter A, B, C, or D.\n");
        }
        user_answers[i] = answer;
        answered++;
        if (answer == quiz[i].correct_option) { score++; printf("Correct!\n"); }
        else printf("Incorrect!\n");
        printf("Current score: %d/%d\n", score, answered);
    }
    clock_gettime(CLOCK_MONOTONIC, &stop);
    active_seconds += (stop.tv_sec - start.tv_sec) + (stop.tv_nsec - start.tv_nsec) / 1000000000.0;

    printf("\n====================================\nQUIZ RESULTS\n====================================\n");
    printf("Player: %s\nTopic: %s\nScore: %d/%d\nPercentage: %.1f%%\nCorrect: %d\nIncorrect: %d\n",
           player, field_names[topic_index], score, total_questions, 100.0 * score / total_questions, score, total_questions - score);
    int whole_seconds = (int)(active_seconds + 0.5);
    int minutes = whole_seconds / 60;
    int remaining_seconds = whole_seconds % 60;
    printf("Total time: %d min %d sec\nAverage time per question: %.1f sec\n",
           minutes, remaining_seconds, active_seconds / answered);

    int wrong = 0;
    printf("\nEXPLANATIONS FOR WRONG ANSWERS\n");
    for (int i = 0; i < total_questions; i++) {
        if (user_answers[i] != quiz[i].correct_option) {
            wrong++;
            printf("\nQuestion %d\nCorrect answer: %c) %s\nExplanation: %s\n",
                   i + 1, quiz[i].correct_option,
                   quiz[i].options[quiz[i].correct_option - 'A'], quiz[i].explanation);
        }
    }
    if (wrong == 0) printf("Excellent! You answered every question correctly.\n");

    /* Save every generated question, including any not needed in the current quiz. */
    if (task.success && task.generated_count > 0) {
        int existing = load_bank(field_files[topic_index], bank);
        if (existing + task.generated_count <= MAX_BANK) {
            for (int i = 0; i < task.generated_count; i++) bank[existing + i] = task.generated[i];
            if (save_bank(field_files[topic_index], bank, existing + task.generated_count))
                printf("Generated questions saved to the %s question bank.\n", field_names[topic_index]);
            else printf("Could not save generated questions to the question bank.\n");
        } else printf("Question bank is full; generated questions were not saved.\n");
        remove("cache.json");
    }

    HistoryRecord records[MAX_HISTORY];
    int history_count = load_history(records);
    if (history_count < MAX_HISTORY) {
        HistoryRecord *r = &records[history_count];
        snprintf(r->name, sizeof(r->name), "%s", player);
        snprintf(r->topic, sizeof(r->topic), "%s", field_names[topic_index]);
        r->score = score; r->total = total_questions; r->seconds = active_seconds;
        time_t now = time(NULL);
        r->timestamp = (long)now;
        struct tm local_time;
        localtime_r(&now, &local_time);
        strftime(r->date, sizeof(r->date), "%d-%m-%Y %I:%M %p", &local_time);
        if (save_history(records, history_count + 1)) printf("Quiz record saved.\n");
        else printf("Could not save quiz history.\n");
    } else printf("History file has reached its record limit.\n");
}

int main(void) {
    srand((unsigned int)time(NULL));
    mkdir("questions", 0777);
    if (curl_global_init(CURL_GLOBAL_DEFAULT) != CURLE_OK) {
        fprintf(stderr, "Could not initialize libcurl.\n");
        return 1;
    }
    while (1) {
        display_menu();
        int choice = get_number("Choose an option (1-4): ", 1, 4);
        if (choice == -1) break;
        if (choice == 1) {
            char player[NAME_LEN];
            if (!get_player_name(player)) break;
            int topic = choose_field();
            if (topic >= 0) play_quiz(player, topic);
        } else if (choice == 2) view_high_scores();
        else if (choice == 3) view_history();
        else { printf("Goodbye!\n"); break; }
    }
    curl_global_cleanup();
    return 0;
}
