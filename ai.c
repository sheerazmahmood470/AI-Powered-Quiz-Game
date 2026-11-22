
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>
#include <stdint.h>
#include <curl/curl.h>
#include <cjson/cJSON.h>

#include "ai.h"

#define MAX_PROMPT_SIZE 12000
#define MAX_RESPONSE_SIZE (4 * 1024 * 1024)

typedef struct {
    char *data;
    size_t len;
} Buffer;

/* ================= STRING VALIDATION ================= */

static int has_text(const char *s) {
    if (!s) return 0;

    while (*s) {
        if (!isspace((unsigned char)*s)) return 1;
        s++;
    }

    return 0;
}

static int same_text(const char *a, const char *b) {
    if (!a || !b) return 0;
    while (*a || *b) {
        while (*a && !isalnum((unsigned char)*a)) a++;
        while (*b && !isalnum((unsigned char)*b)) b++;
        if (tolower((unsigned char)*a) != tolower((unsigned char)*b)) return 0;
        if (*a) a++;
        if (*b) b++;
    }
    return 1;
}
static void replace_text_token(char *text, size_t capacity, const char *from, const char *to) {
    if (!text || !from || !to || !*from) return;
    size_t fl=strlen(from),tl=strlen(to); char *p=text;
    while((p=strstr(p,from))!=NULL){size_t tail=strlen(p+fl);if((size_t)(p-text)+tl+tail+1>capacity)return;memmove(p+tl,p+fl,tail+1);memcpy(p,to,tl);p+=tl;}
}
static void unwrap_latex_text(char *text,const char *command) {
    char *p=text;size_t n=strlen(command);
    while((p=strstr(p,command))!=NULL){char *open=p+n;if(*open!='{'){p=open;continue;}char *close=strchr(open+1,'}');if(!close){p=open+1;continue;}size_t inner=(size_t)(close-open-1);memmove(p,open+1,inner);memmove(p+inner,close+1,strlen(close+1)+1);p+=inner;}
}
static int looks_like_c_code(const char *s) {
    return strstr(s,"printf")||strstr(s,"scanf")||strstr(s,"#include")||
           strstr(s,"sizeof")||strstr(s,"return ")||strstr(s,"->")||
           strstr(s,"NULL")||strstr(s,"char ")||strstr(s,"\\\\n")||
           strstr(s,"\\\\t")||strstr(s,"\\\\0");
}
static int is_known_latex_command(const char *slash) {
    static const char *commands[] = {
        "frac", "sqrt", "sum", "int", "alpha", "beta", "gamma",
        "delta", "theta", "lambda", "mu", "pi", "rho", "sigma",
        "omega", "upsilon", "infty", "begin", "end", "text",
        "mathrm", "operatorname", "times", "cdot", "neq", "leq",
        "geq", "rightarrow", "left", "right"
    };
    for (size_t i = 0; i < sizeof(commands) / sizeof(commands[0]); i++) {
        size_t n = strlen(commands[i]);
        if (strncmp(slash + 1, commands[i], n) == 0) return 1;
    }
    return 0;
}

static int has_unrendered_math_command(const char *text) {
    int is_code = looks_like_c_code(text);
    for (const char *p = text; p[0] && p[1]; p++) {
        if (p[0] == '\\' && isalpha((unsigned char)p[1])) {
            if (is_known_latex_command(p)) return 1;
            if (is_code && strchr("abfnrtv", p[1])) continue;
            return 1;
        }
    }
    return 0;
}

static void normalize_ai_text(char *text,size_t capacity) {
    if(!text||!capacity)return;
    replace_text_token(text,capacity,"\\times","×");replace_text_token(text,capacity,"\\cdot","·");replace_text_token(text,capacity,"\\neq","≠");replace_text_token(text,capacity,"\\leq","≤");replace_text_token(text,capacity,"\\geq","≥");replace_text_token(text,capacity,"\\rightarrow","→");replace_text_token(text,capacity,"\\to","→");replace_text_token(text,capacity,"\\pi","π");
    unwrap_latex_text(text,"\\text");unwrap_latex_text(text,"\\mathrm");unwrap_latex_text(text,"\\operatorname");
    replace_text_token(text,capacity,"\\left","");replace_text_token(text,capacity,"\\right","");replace_text_token(text,capacity,"\\displaystyle","");replace_text_token(text,capacity,"\\(","");replace_text_token(text,capacity,"\\)","");replace_text_token(text,capacity,"\\[","");replace_text_token(text,capacity,"\\]","");replace_text_token(text,capacity,"\\,"," ");replace_text_token(text,capacity,"\\;"," ");replace_text_token(text,capacity,"**","");replace_text_token(text,capacity,"__","");
    if(!looks_like_c_code(text)){replace_text_token(text,capacity,"\\n"," ");replace_text_token(text,capacity,"\\\\"," ");}
    char *r=text,*w=text;int space=0;while(*r){unsigned char ch=(unsigned char)*r++;if(isspace(ch)){space=(w!=text);continue;}if(space&&(size_t)(w-text)+1<capacity)*w++=' ';space=0;if((size_t)(w-text)+1<capacity)*w++=(char)ch;}*w='\0';
}
static int similar_question_text(const char *a,const char *b) {
    char left[64][32], right[64][32];
    int lc=0, rc=0;
    const char *input[2]={a,b};
    char (*set[2])[32]={left,right};
    int *count[2]={&lc,&rc};
    for(int side=0;side<2;side++){const unsigned char *p=(const unsigned char *)input[side];while(*p&&*count[side]<64){while(*p&&!isalnum(*p))p++;char token[32];size_t n=0;while(*p&&isalnum(*p)){if(n<31)token[n++]=(char)tolower(*p);p++;}token[n]='\0';if(n<3)continue;
        if(!strcmp(token,"what")||!strcmp(token,"which")||!strcmp(token,"when")||!strcmp(token,"where")||!strcmp(token,"does")||!strcmp(token,"this")||!strcmp(token,"that")||!strcmp(token,"with")||!strcmp(token,"from")||!strcmp(token,"into")||!strcmp(token,"for")||!strcmp(token,"the")||!strcmp(token,"and")||!strcmp(token,"are")||!strcmp(token,"how")||!strcmp(token,"following")||!strcmp(token,"context")||!strcmp(token,"question"))continue;
        int found=0;for(int i=0;i<*count[side];i++)if(!strcmp(set[side][i],token)){found=1;break;}if(!found)snprintf(set[side][(*count[side])++],32,"%s",token);
    }}
    if (lc < 3 || rc < 3) return 0;
    int shared=0;
    for (int i=0;i<lc;i++) {
        for (int j=0;j<rc;j++) {
            if (!strcmp(left[i],right[j])) { shared++; break; }
        }
    }
    int total=lc+rc-shared;
    return shared>=3 && total>0 && (double)shared/(double)total>=0.72;
}
static int duplicate_question(const Question *q,const Question *list,int count) {
    for(int i=0;i<count;i++)if(same_text(q->question,list[i].question)||similar_question_text(q->question,list[i].question))return 1;
    return 0;
}

/* ================= HTTP RESPONSE BUFFER ================= */

static size_t write_cb(
    char *ptr,
    size_t size,
    size_t nmemb,
    void *userdata
) {
    Buffer *buf = (Buffer *)userdata;

    if (!buf || !ptr || size == 0 ||
        nmemb > SIZE_MAX / size) {
        return 0;
    }

    size_t chunk = size * nmemb;

    if (chunk > MAX_RESPONSE_SIZE - buf->len ||
        buf->len > SIZE_MAX - chunk - 1) {
        return 0;
    }

    char *tmp = realloc(buf->data, buf->len + chunk + 1);

    if (!tmp) return 0;

    buf->data = tmp;

    memcpy(buf->data + buf->len, ptr, chunk);

    buf->len += chunk;
    buf->data[buf->len] = '\0';

    return chunk;
}

/* ================= JSON QUESTION VALIDATION ================= */

static int copy_json_question(
    cJSON *item,
    Question *q
) {
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
        !cJSON_IsString(explanation)) {
        return 0;
    }

    if (!has_text(question->valuestring) ||
        !has_text(explanation->valuestring)) {
        return 0;
    }

    if (strlen(correct->valuestring) != 1 ||
        strlen(question->valuestring) >= QUESTION_LEN ||
        strlen(explanation->valuestring) >= EXPLANATION_LEN) {
        return 0;
    }

    char answer =
        (char)toupper((unsigned char)correct->valuestring[0]);

    if (answer < 'A' || answer > 'D') {
        return 0;
    }

    memset(q, 0, sizeof(*q));

    snprintf(q->question, sizeof(q->question),
             "%s", question->valuestring);
    normalize_ai_text(q->question, sizeof(q->question));
    if (!has_text(q->question) || has_unrendered_math_command(q->question)) return 0;

    snprintf(q->explanation, sizeof(q->explanation),
             "%s", explanation->valuestring);
    normalize_ai_text(q->explanation, sizeof(q->explanation));
    if (!has_text(q->explanation) || has_unrendered_math_command(q->explanation)) return 0;

    q->correct_option = answer;

    for (int i = 0; i < 4; i++) {
        cJSON *option = cJSON_GetArrayItem(options, i);

        if (!cJSON_IsString(option) ||
            !has_text(option->valuestring) ||
            strlen(option->valuestring) >= OPTION_LEN) {
            return 0;
        }

        snprintf(q->options[i], sizeof(q->options[i]),
                 "%s", option->valuestring);
        normalize_ai_text(q->options[i], sizeof(q->options[i]));
        if (!has_text(q->options[i]) || has_unrendered_math_command(q->options[i])) return 0;

        for (int j = 0; j < i; j++) {
            if (same_text(q->options[i], q->options[j])) {
                return 0;
            }
        }
    }

    return 1;
}

/* ================= PROMPT CREATION ================= */

static int build_prompt(
    const AiTask *task,
    char *prompt,
    size_t capacity
) {
    int used = snprintf(
        prompt,
        capacity,
        "Create exactly %d original, unique, medium-difficulty "
        "multiple-choice questions about %s. "
        "Every question must test a different concept; never rephrase the same fact. "
        "Avoid repeating or closely imitating any other question in this set. "
        "Use clear, factually correct questions and explanations. "
        "Every question must have exactly four distinct, plausible answer options. "
        "Independently solve each question before choosing correct_option; verify exactly one option is correct and all distractors are incorrect. "
        "Use plain text and standard programming notation only: no LaTeX commands, Markdown, or escaped formatting tokens. "
        "For C pointer arithmetic, clearly distinguish n elements from n bytes and use standard C notation such as n * sizeof(*p). "
        "The correct_option must identify the genuinely correct answer. "
        "Keep each explanation to one concise sentence (maximum 20 words).\n"
        "Reference questions:\n",
        task->count,
        task->topic
    );

    if (used < 0 || (size_t)used >= capacity) return 0;

    for (int i = 0; i < task->excluded_count; i++) {
        int n = snprintf(
            prompt + used,
            capacity - (size_t)used,
            "- %s\n",
            task->excluded[i].question
        );

        if (n < 0 || (size_t)n >= capacity - (size_t)used) {
            return 0;
        }

        used += n;
    }

    int n = snprintf(
        prompt + used,
        capacity - (size_t)used,
        "\nReturn ONLY a valid JSON object with one key named questions. "
        "The questions array must contain exactly %d objects. "
        "Each object must contain these four fields: "
        "question (string), options (array of exactly four strings), "
        "correct_option (one uppercase letter A, B, C, or D), "
        "and explanation (string). "
        "Do not include A/B/C/D labels inside option strings. "
        "Do not use markdown or extra text.",
        task->count
    );

    return n >= 0 && (size_t)n < capacity - (size_t)used;
}

/* ================= REQUEST CREATION ================= */

static char *create_request_body(const char *prompt) {
    cJSON *request = cJSON_CreateObject();

    if (!request) return NULL;

    cJSON *contents = cJSON_AddArrayToObject(request, "contents");
    cJSON *content = cJSON_CreateObject();
    cJSON *parts = cJSON_CreateArray();
    cJSON *part = cJSON_CreateObject();

    if (!contents || !content || !parts || !part) {
        cJSON_Delete(request);
        cJSON_Delete(content);
        cJSON_Delete(parts);
        cJSON_Delete(part);
        return NULL;
    }

    if (!cJSON_AddStringToObject(content, "role", "user") ||
        !cJSON_AddStringToObject(part, "text", prompt)) {
        cJSON_Delete(request);
        cJSON_Delete(content);
        cJSON_Delete(parts);
        cJSON_Delete(part);
        return NULL;
    }

    if (!cJSON_AddItemToArray(parts, part)) {
        cJSON_Delete(request);
        cJSON_Delete(content);
        cJSON_Delete(parts);
        cJSON_Delete(part);
        return NULL;
    }

    if (!cJSON_AddItemToObject(content, "parts", parts)) {
        cJSON_Delete(request);
        cJSON_Delete(content);
        cJSON_Delete(parts);
        return NULL;
    }

    if (!cJSON_AddItemToArray(contents, content)) {
        cJSON_Delete(request);
        cJSON_Delete(content);
        return NULL;
    }

    cJSON *generation_config = cJSON_AddObjectToObject(request, "generationConfig");
    if (!generation_config ||
        !cJSON_AddStringToObject(generation_config, "responseMimeType", "application/json") ||
        !cJSON_AddNumberToObject(generation_config, "temperature", 0.65) ||
        !cJSON_AddNumberToObject(generation_config, "maxOutputTokens", 8192)) {
        cJSON_Delete(request);
        return NULL;
    }

    char *body = cJSON_PrintUnformatted(request);

    cJSON_Delete(request);

    return body;
}

/* ================= GEMINI RESPONSE PARSING ================= */

static int parse_generated_questions(
    const char *response_text,
    AiTask *task
) {
    if (!response_text || !task) return 0;

    cJSON *api_root = cJSON_Parse(response_text);

    if (!api_root) return 0;

    cJSON *candidates =
        cJSON_GetObjectItemCaseSensitive(api_root, "candidates");

    cJSON *candidate = cJSON_GetArrayItem(candidates, 0);

    cJSON *content = candidate
        ? cJSON_GetObjectItemCaseSensitive(candidate, "content")
        : NULL;

    cJSON *parts = content
        ? cJSON_GetObjectItemCaseSensitive(content, "parts")
        : NULL;

    cJSON *part = cJSON_GetArrayItem(parts, 0);

    cJSON *text = part
        ? cJSON_GetObjectItemCaseSensitive(part, "text")
        : NULL;

    if (!cJSON_IsString(text)) {
        cJSON_Delete(api_root);
        return 0;
    }

    const char *json_start = strchr(text->valuestring, '{');
    const char *json_end = strrchr(text->valuestring, '}');

    if (!json_start || !json_end || json_end < json_start) {
        cJSON_Delete(api_root);
        return 0;
    }

    size_t json_length = (size_t)(json_end - json_start + 1);

    char *json_text = malloc(json_length + 1);

    if (!json_text) {
        cJSON_Delete(api_root);
        return 0;
    }

    memcpy(json_text, json_start, json_length);
    json_text[json_length] = '\0';

    cJSON *quiz = cJSON_Parse(json_text);

    free(json_text);
    cJSON_Delete(api_root);

    if (!quiz) return 0;

    cJSON *questions =
        cJSON_GetObjectItemCaseSensitive(quiz, "questions");

    if (!cJSON_IsArray(questions) ||
        cJSON_GetArraySize(questions) != task->count) {
        cJSON_Delete(quiz);
        return 0;
    }

    int accepted = 0;
    int excluded_count = task->excluded_count;

    for (int i = 0; i < task->count; i++) {
        Question candidate_question;

        if (!copy_json_question(
            cJSON_GetArrayItem(questions, i),
            &candidate_question
        )) {
            continue;
        }

        if (duplicate_question(
            &candidate_question,
            task->excluded,
            excluded_count
        )) {
            continue;
        }

        if (duplicate_question(
            &candidate_question,
            task->generated,
            accepted
        )) {
            continue;
        }

        if (accepted >= MAX_QUESTIONS) break;

        task->generated[accepted++] = candidate_question;
    }

    if (accepted > 0) {
        task->generated_count = accepted;
        task->success = 1;

    }

    cJSON_Delete(quiz);

    return accepted;
}

/* ================= GENERATE QUESTIONS ================= */

void *generate_questions(void *arg) {
    AiTask *task = (AiTask *)arg;
    if (!task) return NULL;

    task->success = 0;
    task->generated_count = 0;
    memset(task->generated, 0, sizeof(task->generated));

    if (task->count <= 0 || task->count > MAX_QUESTIONS ||
        task->excluded_count < 0 || task->excluded_count > 8 ||
        !has_text(task->topic)) {
        fprintf(stderr, "[AI] Invalid generation request.\n");
        return NULL;
    }

    const char *key_names[] = {
        "GEMINI_API_KEY",
        "GEMINI_API_KEY_2",
        "GEMINI_API_KEY_3",
        "GEMINI_API_KEY_4",
        "GEMINI_API_KEY_5"
    };
    const int key_slots = (int)(sizeof(key_names) / sizeof(key_names[0]));
    int configured_keys = 0;
    for (int i = 0; i < key_slots; i++) {
        const char *key = getenv(key_names[i]);
        if (key && key[0]) configured_keys++;
    }
    if (!configured_keys) {
        fprintf(stderr, "[AI] No Gemini API key is configured.\n");
        return NULL;
    }

    char prompt[MAX_PROMPT_SIZE];
    if (!build_prompt(task, prompt, sizeof(prompt))) {
        fprintf(stderr, "[AI] Prompt creation failed.\n");
        return NULL;
    }

    char *body = create_request_body(prompt);
    if (!body) {
        fprintf(stderr, "[AI] Request creation failed.\n");
        return NULL;
    }

    const char *url =
        "https://generativelanguage.googleapis.com/v1beta/models/gemini-3.5-flash-lite:generateContent";

    int attempt = 0;
    for (int i = 0; i < key_slots; i++) {
        const char *api_key = getenv(key_names[i]);
        if (!api_key || !api_key[0]) continue;

        attempt++;
        fprintf(stderr, "[AI] Trying configured Gemini key %d/%d.\n",
                attempt, configured_keys);

        CURL *curl = curl_easy_init();
        if (!curl) {
            fprintf(stderr, "[AI] cURL initialization failed for key slot %d.\n", i + 1);
            continue;
        }

        Buffer response = {NULL, 0};
        struct curl_slist *headers = NULL;
        char key_header[2048];

        int n = snprintf(key_header, sizeof(key_header),
                         "x-goog-api-key: %s", api_key);
        if (n < 0 || (size_t)n >= sizeof(key_header)) {
            fprintf(stderr, "[AI] API key in slot %d is too long.\n", i + 1);
            curl_easy_cleanup(curl);
            continue;
        }

        struct curl_slist *next = curl_slist_append(
            headers, "Content-Type: application/json");
        if (!next) {
            curl_easy_cleanup(curl);
            continue;
        }
        headers = next;

        next = curl_slist_append(headers, key_header);
        if (!next) {
            curl_slist_free_all(headers);
            curl_easy_cleanup(curl);
            continue;
        }
        headers = next;

        curl_easy_setopt(curl, CURLOPT_URL, url);
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
        curl_easy_setopt(curl, CURLOPT_POST, 1L);
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body);
        curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, (long)strlen(body));
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_cb);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
        curl_easy_setopt(curl, CURLOPT_TIMEOUT, 35L);
        curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L);
        curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);

        CURLcode result = curl_easy_perform(curl);
        long status = 0;
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);

        /* Log Google's error response (never the API key) to diagnose rejected requests. */
        if (status != 200 && response.data) {
            fprintf(stderr, "[AI] Gemini error response for key slot %d: %.1200s\n",
                    i + 1, response.data);
        }

        int accepted = 0;
        if (result == CURLE_OK && status == 200 && response.data) {
            task->success = 0;
            task->generated_count = 0;
            memset(task->generated, 0, sizeof(task->generated));
            accepted = parse_generated_questions(response.data, task);
        }

        if (result == CURLE_OK) {
            fprintf(stderr, "[AI] Gemini HTTP status for key slot %d: %ld.\n",
                    i + 1, status);
        } else {
            fprintf(stderr, "[AI] Gemini request failed for key slot %d: %s.\n",
                    i + 1, curl_easy_strerror(result));
        }

        if (accepted == task->count &&
            task->generated_count == task->count) {
            task->success = 1;
            fprintf(stderr, "[AI] Gemini key slot %d succeeded; received %d questions.\n",
                    i + 1, task->generated_count);
            free(response.data);
            curl_slist_free_all(headers);
            curl_easy_cleanup(curl);
            free(body);
            return NULL;
        }

        fprintf(stderr, "[AI] Key slot %d did not return the full valid question set; trying the next key if available.\n",
                i + 1);

        task->success = 0;
        task->generated_count = 0;
        memset(task->generated, 0, sizeof(task->generated));

        free(response.data);
        curl_slist_free_all(headers);
        curl_easy_cleanup(curl);
    }

    fprintf(stderr, "[AI] All configured Gemini API keys failed.\n");
    free(body);
    return NULL;
}
