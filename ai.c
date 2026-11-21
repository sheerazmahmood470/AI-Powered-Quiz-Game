#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <curl/curl.h>
#include <cjson/cJSON.h>
#include "ai.h"

typedef struct {
    char *data;
    size_t len;
} Buffer;

static size_t write_cb(char *ptr, size_t size, size_t nmemb, void *userdata) {
    Buffer *buf = (Buffer *)userdata;
    size_t chunk = size * nmemb;
    char *tmp = realloc(buf->data, buf->len + chunk + 1);
    if (!tmp) return 0;
    buf->data = tmp;
    memcpy(buf->data + buf->len, ptr, chunk);
    buf->len += chunk;
    buf->data[buf->len] = '\0';
    return chunk;
}

static int copy_json_question(cJSON *item, Question *q) {
    cJSON *question = cJSON_GetObjectItemCaseSensitive(item, "question");
    cJSON *options = cJSON_GetObjectItemCaseSensitive(item, "options");
    cJSON *correct = cJSON_GetObjectItemCaseSensitive(item, "correct_option");
    cJSON *explanation = cJSON_GetObjectItemCaseSensitive(item, "explanation");
    if (!cJSON_IsString(question) || !cJSON_IsArray(options) ||
        cJSON_GetArraySize(options) != 4 || !cJSON_IsString(correct) ||
        !cJSON_IsString(explanation) || strlen(correct->valuestring) != 1) return 0;
    char answer = correct->valuestring[0];
    if (answer < 'A' || answer > 'D') return 0;
    if (strlen(question->valuestring) >= QUESTION_LEN ||
        strlen(explanation->valuestring) >= EXPLANATION_LEN) return 0;
    strcpy(q->question, question->valuestring);
    strcpy(q->explanation, explanation->valuestring);
    q->correct_option = answer;
    for (int i = 0; i < 4; i++) {
        cJSON *opt = cJSON_GetArrayItem(options, i);
        if (!cJSON_IsString(opt) || strlen(opt->valuestring) >= OPTION_LEN) return 0;
        strcpy(q->options[i], opt->valuestring);
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

void *generate_questions(void *arg) {
    AiTask *task = (AiTask *)arg;
    task->success = 0;
    task->generated_count = 0;

    const char *api_key = getenv("GEMINI_API_KEY");
    if (!api_key || !api_key[0]) return NULL;

    char prompt[12000];
    int used = snprintf(prompt, sizeof(prompt),
        "Create exactly %d unique medium-difficulty multiple-choice questions about %s. "
        "Do not repeat or closely imitate these existing questions: ", task->count, task->topic);
    if (used < 0 || (size_t)used >= sizeof(prompt)) return NULL;

    for (int i = 0; i < task->excluded_count; i++) {
        int n = snprintf(prompt + used, sizeof(prompt) - (size_t)used,
                         "\n- %s", task->excluded[i].question);
        if (n < 0 || (size_t)n >= sizeof(prompt) - (size_t)used) return NULL;
        used += n;
    }

    int n = snprintf(prompt + used, sizeof(prompt) - (size_t)used,
        "\nReturn ONLY a valid JSON object with one key named questions. "
        "questions must be an array containing exactly %d objects. "
        "Each object must contain: question (string), options (array of exactly four strings), "
        "correct_option (one letter A, B, C, or D), and explanation (short, maximum 3-4 lines). "
        "Do not put A/B/C/D labels inside option strings. No markdown or extra text.", task->count);
    if (n < 0 || (size_t)n >= sizeof(prompt) - (size_t)used) return NULL;

    cJSON *request = cJSON_CreateObject();
    cJSON *contents = cJSON_AddArrayToObject(request, "contents");
    cJSON *content = cJSON_CreateObject();
    cJSON_AddStringToObject(content, "role", "user");
    cJSON *parts = cJSON_AddArrayToObject(content, "parts");
    cJSON *part = cJSON_CreateObject();
    cJSON_AddStringToObject(part, "text", prompt);
    cJSON_AddItemToArray(parts, part);
    cJSON_AddItemToArray(contents, content);
    char *body = cJSON_PrintUnformatted(request);
    cJSON_Delete(request);
    if (!body) return NULL;

    CURL *curl = curl_easy_init();
    if (!curl) { free(body); return NULL; }
    Buffer response = {NULL, 0};
    struct curl_slist *headers = NULL;
    headers = curl_slist_append(headers, "Content-Type: application/json");
    char key_header[512];
    n = snprintf(key_header, sizeof(key_header), "x-goog-api-key: %s", api_key);
    if (n < 0 || (size_t)n >= sizeof(key_header)) goto cleanup;
    headers = curl_slist_append(headers, key_header);

    curl_easy_setopt(curl, CURLOPT_URL,
    "https://generativelanguage.googleapis.com/v1beta/models/gemini-3.5-flash-lite:generateContent");
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_POST, 1L);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 20L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 8L);

    CURLcode result = curl_easy_perform(curl);
    if (result != CURLE_OK) goto cleanup;
    long status = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
    if (status != 200 || !response.data) goto cleanup;

    cJSON *api_root = cJSON_Parse(response.data);
    cJSON *candidates = api_root ? cJSON_GetObjectItemCaseSensitive(api_root, "candidates") : NULL;
    cJSON *candidate = cJSON_GetArrayItem(candidates, 0);
    cJSON *api_content = candidate ? cJSON_GetObjectItemCaseSensitive(candidate, "content") : NULL;
    cJSON *api_parts = api_content ? cJSON_GetObjectItemCaseSensitive(api_content, "parts") : NULL;
    cJSON *api_part = cJSON_GetArrayItem(api_parts, 0);
    cJSON *text = api_part ? cJSON_GetObjectItemCaseSensitive(api_part, "text") : NULL;
    if (cJSON_IsString(text)) {
        cJSON *quiz = cJSON_Parse(text->valuestring);
        cJSON *questions = quiz ? cJSON_GetObjectItemCaseSensitive(quiz, "questions") : NULL;
        if (cJSON_IsArray(questions) && cJSON_GetArraySize(questions) == task->count) {
            int valid = 1;
            for (int i = 0; i < task->count; i++) {
                if (!copy_json_question(cJSON_GetArrayItem(questions, i), &task->generated[i])) {
                    valid = 0; break;
                }
            }
            if (valid) {
                task->generated_count = task->count;
                task->success = 1;
                FILE *cache = fopen("cache.json", "w");
                if (cache) {
                    char *saved = cJSON_Print(quiz);
                    if (saved) { fprintf(cache, "%s\n", saved); free(saved); }
                    fclose(cache);
                }
            }
        }
        cJSON_Delete(quiz);
    }
    cJSON_Delete(api_root);

cleanup:
    free(body);
    free(response.data);
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    return NULL;
}