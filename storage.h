#ifndef QUIZ_STORAGE_H
#define QUIZ_STORAGE_H
int storage_is_configured(void);
int storage_init(void);
char *storage_read_json(const char *key);
int storage_write_json(const char *key, const char *json);
char *storage_read_history_json(void);
char *storage_read_reviews_json(void);
int storage_append_review_json(const char *json);
int storage_append_history_json(const char *json);
int storage_add_questions_json(const char *topic, const char *questions_json);
char *storage_take_questions_json(const char *topic, int limit);
int storage_question_bank_count(const char *topic, int unused_only);
#endif
