
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

/* ================= PROVIDER RESPONSE PARSING ================= */

static int parse_questions_text(
    const char *response_text,
    AiTask *task
) {
    if (!response_text || !task) return 0;

    const char *json_start = strchr(response_text, '{');
    const char *json_end = strrchr(response_text, '}');
    if (!json_start || !json_end || json_end < json_start) return 0;

    size_t json_length = (size_t)(json_end - json_start + 1);
    char *json_text = malloc(json_length + 1);
    if (!json_text) return 0;
    memcpy(json_text, json_start, json_length);
    json_text[json_length] = '\0';

    cJSON *quiz = cJSON_Parse(json_text);
    free(json_text);
    if (!quiz) return 0;

    cJSON *questions = cJSON_GetObjectItemCaseSensitive(quiz, "questions");
    if (!cJSON_IsArray(questions) ||
        cJSON_GetArraySize(questions) != task->count) {
        cJSON_Delete(quiz);
        return 0;
    }

    int accepted = 0;
    for (int i = 0; i < task->count; i++) {
        Question candidate_question;
        if (!copy_json_question(cJSON_GetArrayItem(questions, i), &candidate_question)) continue;
        if (duplicate_question(&candidate_question, task->excluded, task->excluded_count)) continue;
        if (duplicate_question(&candidate_question, task->generated, accepted)) continue;
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

static int parse_provider_response(const char *response_text, int provider, AiTask *task) {
    if (!response_text || !task) return 0;
    cJSON *root = cJSON_Parse(response_text);
    if (!root) return 0;
    const char *content = NULL;
    cJSON *node = NULL;

    if (provider == 0) {
        cJSON *candidates = cJSON_GetObjectItemCaseSensitive(root, "candidates");
        cJSON *candidate = cJSON_GetArrayItem(candidates, 0);
        cJSON *body = candidate ? cJSON_GetObjectItemCaseSensitive(candidate, "content") : NULL;
        cJSON *parts = body ? cJSON_GetObjectItemCaseSensitive(body, "parts") : NULL;
        cJSON *part = cJSON_GetArrayItem(parts, 0);
        node = part ? cJSON_GetObjectItemCaseSensitive(part, "text") : NULL;
    } else if (provider == 2) {
        cJSON *result = cJSON_GetObjectItemCaseSensitive(root, "result");
        node = result ? cJSON_GetObjectItemCaseSensitive(result, "response") : NULL;
    } else {
        cJSON *choices = cJSON_GetObjectItemCaseSensitive(root, "choices");
        cJSON *choice = cJSON_GetArrayItem(choices, 0);
        cJSON *message = choice ? cJSON_GetObjectItemCaseSensitive(choice, "message") : NULL;
        node = message ? cJSON_GetObjectItemCaseSensitive(message, "content") : NULL;
    }
    if (cJSON_IsString(node)) content = node->valuestring;
    int accepted = content ? parse_questions_text(content, task) : 0;
    cJSON_Delete(root);
    return accepted;
}

static char *create_compatible_request_body(const char *prompt, int provider) {
    cJSON *request = cJSON_CreateObject();
    if (!request) return NULL;

    if (provider == 2) {
        cJSON *messages = cJSON_AddArrayToObject(request, "messages");
        cJSON *message = cJSON_CreateObject();
        if (!messages || !message) {
            cJSON_Delete(request);
            cJSON_Delete(message);
            return NULL;
        }
        cJSON_AddStringToObject(message, "role", "user");
        cJSON_AddStringToObject(message, "content", prompt);
        cJSON_AddItemToArray(messages, message);
        cJSON_AddNumberToObject(request, "max_tokens", 8192);
        cJSON_AddNumberToObject(request, "temperature", 0.65);
    } else {
        cJSON_AddStringToObject(request, "model",
            provider == 1 ? "openai/gpt-oss-120b" : "openrouter/free");
        cJSON *messages = cJSON_AddArrayToObject(request, "messages");
        cJSON *message = cJSON_CreateObject();
        if (!messages || !message) {
            cJSON_Delete(request);
            cJSON_Delete(message);
            return NULL;
        }
        cJSON_AddStringToObject(message, "role", "user");
        cJSON_AddStringToObject(message, "content", prompt);
        cJSON_AddItemToArray(messages, message);
        cJSON_AddNumberToObject(request, "temperature", 0.65);
        cJSON_AddNumberToObject(request, "max_tokens", 8192);
        cJSON *format = cJSON_AddObjectToObject(request, "response_format");
        if (format) cJSON_AddStringToObject(format, "type", "json_object");
    }

    char *body = cJSON_PrintUnformatted(request);
    cJSON_Delete(request);
    return body;
}

/* provider: 0=Gemini, 1=Groq, 2=Cloudflare Workers AI, 3=OpenRouter */
static int call_ai_provider(
    int provider,
    const char *api_key,
    const char *account_id,
    const char *prompt,
    AiTask *task
) {
    if (!api_key || !api_key[0] || !prompt || !task) return 0;

    char url[1024];
    char auth_header[4096];
    char key_header[4096];
    const char *header_value = NULL;
    char *body = NULL;

    if (provider == 0) {
        snprintf(url, sizeof(url),
            "https://generativelanguage.googleapis.com/v1beta/models/gemini-3.5-flash-lite:generateContent");
        body = create_request_body(prompt);
        int n = snprintf(key_header, sizeof(key_header), "x-goog-api-key: %s", api_key);
        if (n < 0 || (size_t)n >= sizeof(key_header)) { free(body); return 0; }
        header_value = key_header;
    } else if (provider == 1) {
        snprintf(url, sizeof(url), "https://api.groq.com/openai/v1/chat/completions");
        body = create_compatible_request_body(prompt, provider);
        int n = snprintf(auth_header, sizeof(auth_header), "Authorization: Bearer %s", api_key);
        if (n < 0 || (size_t)n >= sizeof(auth_header)) { free(body); return 0; }
        header_value = auth_header;
    } else if (provider == 2) {
        if (!account_id || !account_id[0]) return 0;
        snprintf(url, sizeof(url),
            "https://api.cloudflare.com/client/v4/accounts/%s/ai/run/@cf/meta/llama-3.3-70b-instruct-fp8-fast",
            account_id);
        body = create_compatible_request_body(prompt, provider);
        int n = snprintf(auth_header, sizeof(auth_header), "Authorization: Bearer %s", api_key);
        if (n < 0 || (size_t)n >= sizeof(auth_header)) { free(body); return 0; }
        header_value = auth_header;
    } else {
        snprintf(url, sizeof(url), "https://openrouter.ai/api/v1/chat/completions");
        body = create_compatible_request_body(prompt, provider);
        int n = snprintf(auth_header, sizeof(auth_header), "Authorization: Bearer %s", api_key);
        if (n < 0 || (size_t)n >= sizeof(auth_header)) { free(body); return 0; }
        header_value = auth_header;
    }

    if (!body) return 0;
    CURL *curl = curl_easy_init();
    if (!curl) { free(body); return 0; }

    Buffer response = {NULL, 0};
    struct curl_slist *headers = NULL;
    struct curl_slist *next = curl_slist_append(headers, "Content-Type: application/json");
    if (!next) { curl_easy_cleanup(curl); free(body); return 0; }
    headers = next;
    next = curl_slist_append(headers, header_value);
    if (!next) {
        curl_slist_free_all(headers);
        curl_easy_cleanup(curl);
        free(body);
        return 0;
    }
    headers = next;
    if (provider == 3) {
        next = curl_slist_append(headers, "HTTP-Referer: https://ai-powered-quiz-game.onrender.com");
        if (next) headers = next;
        next = curl_slist_append(headers, "X-Title: AI Powered Quiz Game");
        if (next) headers = next;
    }

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
    int accepted = 0;
    if (result == CURLE_OK && status == 200 && response.data) {
        task->success = 0;
        task->generated_count = 0;
        memset(task->generated, 0, sizeof(task->generated));
        accepted = parse_provider_response(response.data, provider, task);
    } else if (response.data) {
        fprintf(stderr, "[AI] Provider %d returned HTTP %ld: %.500s\n",
            provider, status, response.data);
    } else if (result != CURLE_OK) {
        fprintf(stderr, "[AI] Provider %d request failed: %s\n",
            provider, curl_easy_strerror(result));
    }

    free(response.data);
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    free(body);
    return accepted == task->count && task->generated_count == task->count;
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

    char prompt[MAX_PROMPT_SIZE];
    if (!build_prompt(task, prompt, sizeof(prompt))) {
        fprintf(stderr, "[AI] Prompt creation failed.\n");
        return NULL;
    }

    const char *gemini_primary = getenv("GEMINI_API_KEY");
    const char *groq_key = getenv("GROQ_API_KEY");
    const char *cloudflare_key = getenv("CLOUDFLARE_API_TOKEN");
    const char *cloudflare_account = getenv("CLOUDFLARE_ACCOUNT_ID");
    const char *openrouter_key = getenv("OPENROUTER_API_KEY");
    const char *gemini_last = getenv("GEMINI_API_KEY_5");

    struct {
        int provider;
        const char *name;
        const char *key;
    } providers[] = {
        {0, "Gemini primary", gemini_primary},
        {1, "Groq", groq_key},
        {2, "Cloudflare Workers AI", cloudflare_key},
        {3, "OpenRouter", openrouter_key},
        {0, "Gemini final fallback", gemini_last}
    };

    for (size_t i = 0; i < sizeof(providers) / sizeof(providers[0]); i++) {
        if (!providers[i].key || !providers[i].key[0]) {
            fprintf(stderr, "[AI] %s is not configured; skipping.\n", providers[i].name);
            continue;
        }
        if (providers[i].provider == 2 && (!cloudflare_account || !cloudflare_account[0])) {
            fprintf(stderr, "[AI] Cloudflare Account ID is missing; skipping provider.\n");
            continue;
        }

        fprintf(stderr, "[AI] Trying %s (%zu/5).\n", providers[i].name, i + 1);
        if (call_ai_provider(providers[i].provider, providers[i].key,
                providers[i].provider == 2 ? cloudflare_account : NULL,
                prompt, task)) {
            task->success = 1;
            fprintf(stderr, "[AI] %s succeeded with %d questions.\n",
                providers[i].name, task->generated_count);
            return NULL;
        }

        task->success = 0;
        task->generated_count = 0;
        memset(task->generated, 0, sizeof(task->generated));
        fprintf(stderr, "[AI] %s failed validation or was unavailable; moving to next provider.\n",
            providers[i].name);
    }

    fprintf(stderr, "[AI] All configured AI providers failed.\n");
    return NULL;
}
