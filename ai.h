#ifndef AI_H
#define AI_H

#define MAX_QUESTIONS 15
#define QUESTION_LEN 600
#define OPTION_LEN 250
#define EXPLANATION_LEN 700

typedef struct {
    char question[QUESTION_LEN];
    char options[4][OPTION_LEN];
    char correct_option;
    char explanation[EXPLANATION_LEN];
} Question;

typedef struct {
    char topic[80];
    int count;
    Question excluded[8];
    int excluded_count;
    Question generated[MAX_QUESTIONS];
    int generated_count;
    int success;
} AiTask;

void *generate_questions(void *arg);

#endif
