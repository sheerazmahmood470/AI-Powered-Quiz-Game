#ifndef QUIZ_STORAGE_H
#define QUIZ_STORAGE_H
int storage_is_configured(void);
int storage_init(void);
char *storage_read_json(const char *key);
int storage_write_json(const char *key, const char *json);
char *storage_read_history_json(void);
int storage_append_history_json(const char *json);
#endif
