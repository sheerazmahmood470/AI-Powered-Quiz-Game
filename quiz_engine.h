#ifndef QUIZ_ENGINE_H
#define QUIZ_ENGINE_H

#include "ai.h"

#define WEB_MAX_QUESTIONS 15

typedef struct {
    char session_id[64];
    char player[50];
    char topic[80];

    Question questions[WEB_MAX_QUESTIONS];

    int total_questions;
    int current_question;
    int correct_answers;
    int incorrect_answers;
    int completed;

    double start_time;
} WebQuizSession;

int quiz_engine_start(
    WebQuizSession *session,
    const char *player,
    const char *topic,
    int question_count
);
int quiz_engine_start_offline(
    WebQuizSession *session,
    const char *player,
    const char *topic,
    int question_count
);

int quiz_engine_submit_answer(
    WebQuizSession *session,
    char answer
);

#endif