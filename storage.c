#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <libpq-fe.h>
#include "storage.h"

#define MAX_SEED_BYTES 10000000

static const char *database_url(void) {
    const char *value = getenv("DATABASE_URL");
    return (value && value[0]) ? value : NULL;
}

int storage_is_configured(void) {
    return database_url() != NULL;
}

static PGconn *open_database(void) {
    const char *url = database_url();
    if (!url) return NULL;
    PGconn *db = PQconnectdb(url);
    if (!db || PQstatus(db) != CONNECTION_OK) {
        if (db) PQfinish(db);
        return NULL;
    }
    return db;
}

static char *read_seed_file(const char *path) {
    FILE *file = fopen(path, "rb");
    if (!file) return strdup("[]");
    if (fseek(file, 0, SEEK_END) != 0) { fclose(file); return NULL; }
    long length = ftell(file);
    if (length < 0 || length > MAX_SEED_BYTES ||
        fseek(file, 0, SEEK_SET) != 0) { fclose(file); return NULL; }
    char *buffer = malloc((size_t)length + 1);
    if (!buffer) { fclose(file); return NULL; }
    size_t got = fread(buffer, 1, (size_t)length, file);
    fclose(file);
    if (got != (size_t)length) { free(buffer); return NULL; }
    buffer[got] = '\0';
    return buffer;
}

static int seed_key(PGconn *db, const char *key, const char *path) {
    char *seed = read_seed_file(path);
    if (!seed) return 0;
    const char *values[2] = { key, seed };
    PGresult *result = PQexecParams(db,
        "INSERT INTO quiz_game_store(data_key,payload) VALUES($1,$2::jsonb) ON CONFLICT(data_key) DO NOTHING",
        2, NULL, values, NULL, NULL, 0);
    free(seed);
    int ok = result && PQresultStatus(result) == PGRES_COMMAND_OK;
    if (result) PQclear(result);
    return ok;
}

int storage_init(void) {
    if (!storage_is_configured()) {
        const char *required = getenv("QUIZ_REQUIRE_DATABASE");
        if (required && (!strcmp(required, "1") || !strcmp(required, "true") || !strcmp(required, "yes"))) {
            fprintf(stderr, "[STORAGE] DATABASE_URL is required but is not configured. Refusing temporary storage.\\n");
            return 0;
        }
        fprintf(stderr, "[STORAGE] Neon not configured; using local development storage.\\n");
        return 1;
    }
    PGconn *db = open_database();
    if (!db) {
        fprintf(stderr, "[STORAGE] PostgreSQL connection failed. Check DATABASE_URL and Neon status.\n");
        return 0;
    }

    PGresult *r = PQexec(db,
        "CREATE TABLE IF NOT EXISTS quiz_game_store("
        "data_key TEXT PRIMARY KEY,payload JSONB NOT NULL,"
        "updated_at TIMESTAMPTZ NOT NULL DEFAULT NOW());"
        "CREATE TABLE IF NOT EXISTS quiz_game_meta("
        "meta_key TEXT PRIMARY KEY,meta_value TEXT NOT NULL);"
        "CREATE TABLE IF NOT EXISTS quiz_history("
        "id BIGSERIAL PRIMARY KEY,record JSONB NOT NULL,created_at BIGINT NOT NULL);"
        "CREATE INDEX IF NOT EXISTS quiz_history_fifo_idx ON quiz_history(created_at,id);"
        "CREATE TABLE IF NOT EXISTS quiz_reviews("
        "id BIGSERIAL PRIMARY KEY,review JSONB NOT NULL,created_at BIGINT NOT NULL);"
        "CREATE INDEX IF NOT EXISTS quiz_reviews_fifo_idx ON quiz_reviews(created_at,id);"
        "CREATE TABLE IF NOT EXISTS quiz_question_bank("
        "id BIGSERIAL PRIMARY KEY,topic TEXT NOT NULL,question_hash TEXT NOT NULL,"
        "question JSONB NOT NULL,created_at TIMESTAMPTZ NOT NULL DEFAULT NOW(),used_at TIMESTAMPTZ);"
        "CREATE UNIQUE INDEX IF NOT EXISTS quiz_question_bank_unique_idx ON quiz_question_bank(topic,question_hash);"
        "CREATE INDEX IF NOT EXISTS quiz_question_bank_pick_idx ON quiz_question_bank(topic,used_at,id)");
    int ok = r && PQresultStatus(r) == PGRES_COMMAND_OK;
    if (r) PQclear(r);

    if (ok) ok = seed_key(db, "history", "history.json");
    if (ok) ok = seed_key(db, "reviews", "reviews.json");

    /* One-time migration preserves the existing history blob before row storage starts. */
    if (ok) {
        PGresult *marker = PQexec(db,
            "SELECT 1 FROM quiz_game_meta WHERE meta_key='history_rows_migrated'");
        int migrated = marker && PQresultStatus(marker) == PGRES_TUPLES_OK &&
                       PQntuples(marker) > 0;
        if (marker) PQclear(marker);

        if (!migrated) {
            PGresult *legacy = PQexec(db,
                "SELECT payload::text FROM quiz_game_store WHERE data_key='history'");
            if (!legacy || PQresultStatus(legacy) != PGRES_TUPLES_OK) {
                ok = 0;
            } else {
                if (PQntuples(legacy) == 1) {
                    const char *values[1] = { PQgetvalue(legacy, 0, 0) };
                    PGresult *inserted = PQexecParams(db,
                        "INSERT INTO quiz_history(record,created_at) "
                        "SELECT item.record,COALESCE(NULLIF(item.record->>'timestamp','')::bigint,extract(epoch from now())::bigint) "
                        "FROM jsonb_array_elements($1::jsonb) WITH ORDINALITY AS item(record,ordinality) "
                        "ORDER BY item.ordinality",
                        1, NULL, values, NULL, NULL, 0);
                    ok = inserted && PQresultStatus(inserted) == PGRES_COMMAND_OK;
                    if (inserted) PQclear(inserted);
                }
                PQclear(legacy);
            }
            if (ok) {
                PGresult *done = PQexec(db,
                    "INSERT INTO quiz_game_meta(meta_key,meta_value) "
                    "VALUES('history_rows_migrated','yes') ON CONFLICT(meta_key) DO NOTHING");
                ok = done && PQresultStatus(done) == PGRES_COMMAND_OK;
                if (done) PQclear(done);
            }
        }
    }

    /* One-time migration from the legacy reviews JSONB blob to independent rows. */
    if (ok) {
        PGresult *marker = PQexec(db,
            "SELECT 1 FROM quiz_game_meta WHERE meta_key='reviews_rows_migrated'");
        int migrated = marker && PQresultStatus(marker) == PGRES_TUPLES_OK &&
                       PQntuples(marker) > 0;
        if (marker) PQclear(marker);
        if (!migrated) {
            PGresult *legacy = PQexec(db,
                "SELECT payload::text FROM quiz_game_store WHERE data_key='reviews'");
            if (!legacy || PQresultStatus(legacy) != PGRES_TUPLES_OK) {
                ok = 0;
            } else {
                if (PQntuples(legacy) == 1) {
                    const char *values[1] = { PQgetvalue(legacy, 0, 0) };
                    PGresult *inserted = PQexecParams(db,
                        "INSERT INTO quiz_reviews(review,created_at) "
                        "SELECT item.review,COALESCE(NULLIF(item.review->>'timestamp','')::bigint,extract(epoch from now())::bigint) "
                        "FROM jsonb_array_elements($1::jsonb) WITH ORDINALITY AS item(review,ordinality) "
                        "ORDER BY item.ordinality",
                        1, NULL, values, NULL, NULL, 0);
                    ok = inserted && PQresultStatus(inserted) == PGRES_COMMAND_OK;
                    if (inserted) PQclear(inserted);
                }
                PQclear(legacy);
            }
            if (ok) {
                PGresult *done = PQexec(db,
                    "INSERT INTO quiz_game_meta(meta_key,meta_value) "
                    "VALUES('reviews_rows_migrated','yes') ON CONFLICT(meta_key) DO NOTHING");
                ok = done && PQresultStatus(done) == PGRES_COMMAND_OK;
                if (done) PQclear(done);
            }
        }
    }

    if (!ok) fprintf(stderr, "[STORAGE] PostgreSQL schema or history migration failed.\n");
    if (ok) fprintf(stderr, "[STORAGE] Neon PostgreSQL connected; quiz history and reviews are persistent.\\n");
    PQfinish(db);
    return ok;
}

char *storage_read_json(const char *key) {
    if (!storage_is_configured() || !key) return NULL;
    PGconn *db = open_database();
    if (!db) return NULL;
    const char *values[1] = { key };
    PGresult *r = PQexecParams(db,
        "SELECT payload::text FROM quiz_game_store WHERE data_key=$1",
        1, NULL, values, NULL, NULL, 0);
    char *copy = NULL;
    if (r && PQresultStatus(r) == PGRES_TUPLES_OK &&
        PQntuples(r) == 1 && !PQgetisnull(r, 0, 0)) {
        copy = strdup(PQgetvalue(r, 0, 0));
    }
    if (r) PQclear(r);
    PQfinish(db);
    return copy;
}

int storage_write_json(const char *key, const char *json) {
    if (!storage_is_configured() || !key || !json) return 0;
    PGconn *db = open_database();
    if (!db) return 0;
    const char *values[2] = { key, json };
    PGresult *r = PQexecParams(db,
        "INSERT INTO quiz_game_store(data_key,payload,updated_at) "
        "VALUES($1,$2::jsonb,NOW()) ON CONFLICT(data_key) "
        "DO UPDATE SET payload=EXCLUDED.payload,updated_at=NOW()",
        2, NULL, values, NULL, NULL, 0);
    int ok = r && PQresultStatus(r) == PGRES_COMMAND_OK;
    if (r) PQclear(r);
    PQfinish(db);
    return ok;
}

/* Dashboard shows the latest 5,000 records; older records remain stored in Neon. */
char *storage_read_history_json(void) {
    if (!storage_is_configured()) return NULL;
    PGconn *db = open_database();
    if (!db) return NULL;
    PGresult *r = PQexec(db,
        "SELECT COALESCE(jsonb_agg(record ORDER BY id),'[]'::jsonb)::text "
        "FROM (SELECT id,record FROM quiz_history ORDER BY id DESC LIMIT 5000) recent");
    char *copy = NULL;
    if (r && PQresultStatus(r) == PGRES_TUPLES_OK && PQntuples(r) == 1) {
        copy = strdup(PQgetvalue(r, 0, 0));
    }
    if (r) PQclear(r);
    PQfinish(db);
    return copy;
}

/* Atomic append and global FIFO cleanup. The default logical live-row budget
   is 650 MiB, leaving safety headroom below Neon's 1 GB project storage quota. */
int storage_append_history_json(const char *json) {
    if (!storage_is_configured() || !json) return 0;
    PGconn *db = open_database();
    if (!db) return 0;

    long long limit = 650LL * 1024LL * 1024LL;
    const char *configured = getenv("QUIZ_HISTORY_MAX_BYTES");
    if (configured && configured[0]) {
        char *end = NULL;
        long long parsed = strtoll(configured, &end, 10);
        if (end && *end == '\0' && parsed >= 1024LL * 1024LL) limit = parsed;
    }
    char limit_text[32];
    snprintf(limit_text, sizeof(limit_text), "%lld", limit);
    const char *values[2] = { json, limit_text };

    PGresult *r = PQexec(db, "BEGIN");
    int ok = r && PQresultStatus(r) == PGRES_COMMAND_OK;
    if (r) PQclear(r);

    if (ok) {
        r = PQexec(db, "SELECT pg_advisory_xact_lock(731942061)");
        ok = r && PQresultStatus(r) == PGRES_TUPLES_OK;
        if (r) PQclear(r);
    }
    if (ok) {
        r = PQexecParams(db,
            "INSERT INTO quiz_history(record,created_at) "
            "VALUES($1::jsonb,COALESCE(NULLIF(($1::jsonb)->>'timestamp','')::bigint,extract(epoch from now())::bigint))",
            1, NULL, values, NULL, NULL, 0);
        ok = r && PQresultStatus(r) == PGRES_COMMAND_OK;
        if (r) PQclear(r);
    }
    if (ok) {
        r = PQexecParams(db,
            "WITH ranked AS ("
            " SELECT id,SUM(pg_column_size(record)::bigint+64) "
            " OVER (ORDER BY created_at DESC,id DESC) AS retained_bytes FROM quiz_history"
            "), expired AS (SELECT id FROM ranked WHERE retained_bytes>$1::bigint) "
            "DELETE FROM quiz_history h USING expired e WHERE h.id=e.id",
            1, NULL, &values[1], NULL, NULL, 0);
        ok = r && PQresultStatus(r) == PGRES_COMMAND_OK;
        if (r) PQclear(r);
    }
    if (ok) {
        r = PQexec(db, "COMMIT");
        ok = r && PQresultStatus(r) == PGRES_COMMAND_OK;
        if (r) PQclear(r);
    } else {
        r = PQexec(db, "ROLLBACK");
        if (r) PQclear(r);
    }

    if (!ok) fprintf(stderr, "[STORAGE] Neon quiz append/FIFO retention failed.\n");
    PQfinish(db);
    return ok;
}

/* Reviews use their own PostgreSQL table and an independent global FIFO budget. */
char *storage_read_reviews_json(void) {
    if (!storage_is_configured()) return NULL;
    PGconn *db = open_database();
    if (!db) return NULL;
    PGresult *r = PQexec(db,
        "SELECT COALESCE(jsonb_agg(review ORDER BY id),'[]'::jsonb)::text FROM quiz_reviews");
    char *copy = NULL;
    if (r && PQresultStatus(r) == PGRES_TUPLES_OK && PQntuples(r) == 1)
        copy = strdup(PQgetvalue(r, 0, 0));
    if (r) PQclear(r);
    PQfinish(db);
    return copy;
}

int storage_append_review_json(const char *json) {
    if (!storage_is_configured() || !json) return 0;
    PGconn *db = open_database();
    if (!db) return 0;

    long long limit = 150LL * 1024LL * 1024LL;
    const char *configured = getenv("QUIZ_REVIEWS_MAX_BYTES");
    if (configured && configured[0]) {
        char *end = NULL;
        long long parsed = strtoll(configured, &end, 10);
        if (end && *end == '\0' && parsed >= 1024LL * 1024LL) limit = parsed;
    }
    char limit_text[32];
    snprintf(limit_text, sizeof(limit_text), "%lld", limit);
    const char *values[2] = { json, limit_text };

    PGresult *r = PQexec(db, "BEGIN");
    int ok = r && PQresultStatus(r) == PGRES_COMMAND_OK;
    if (r) PQclear(r);
    if (ok) {
        r = PQexec(db, "SELECT pg_advisory_xact_lock(731942062)");
        ok = r && PQresultStatus(r) == PGRES_TUPLES_OK;
        if (r) PQclear(r);
    }
    if (ok) {
        r = PQexecParams(db,
            "INSERT INTO quiz_reviews(review,created_at) "
            "VALUES($1::jsonb,COALESCE(NULLIF(($1::jsonb)->>'timestamp','')::bigint,extract(epoch from now())::bigint))",
            1, NULL, values, NULL, NULL, 0);
        ok = r && PQresultStatus(r) == PGRES_COMMAND_OK;
        if (r) PQclear(r);
    }
    if (ok) {
        r = PQexecParams(db,
            "WITH ranked AS ("
            " SELECT id,SUM(pg_column_size(review)::bigint+64) "
            " OVER (ORDER BY created_at DESC,id DESC) AS retained_bytes FROM quiz_reviews"
            "), expired AS (SELECT id FROM ranked WHERE retained_bytes>$1::bigint) "
            "DELETE FROM quiz_reviews h USING expired e WHERE h.id=e.id",
            1, NULL, &values[1], NULL, NULL, 0);
        ok = r && PQresultStatus(r) == PGRES_COMMAND_OK;
        if (r) PQclear(r);
    }
    if (ok) {
        r = PQexec(db, "COMMIT");
        ok = r && PQresultStatus(r) == PGRES_COMMAND_OK;
        if (r) PQclear(r);
    } else {
        r = PQexec(db, "ROLLBACK");
        if (r) PQclear(r);
    }
    if (!ok) fprintf(stderr, "[STORAGE] Neon review append/FIFO retention failed.\n");
    PQfinish(db);
    return ok;
}


/* Dedicated question bank: only MCQ payloads are stored here. Quiz history
   remains in quiz_history and is never copied into this table. */
int storage_add_questions_json(const char *topic, const char *questions_json) {
    if (!storage_is_configured() || !topic || !questions_json) return 0;
    PGconn *db = open_database();
    if (!db) return 0;
    const char *values[2] = { topic, questions_json };
    PGresult *r = PQexec(db, "BEGIN");
    int ok = r && PQresultStatus(r) == PGRES_COMMAND_OK;
    if (r) PQclear(r);
    if (ok) {
        r = PQexecParams(db,
            "INSERT INTO quiz_question_bank(topic,question_hash,question) "
            "SELECT $1,md5(lower(regexp_replace(item->>'question','[^a-zA-Z0-9]','','g'))),item "
            "FROM jsonb_array_elements($2::jsonb) AS item "
            "WHERE jsonb_typeof(item)='object' AND length(trim(COALESCE(item->>'question','')))>0 "
            "ON CONFLICT(topic,question_hash) DO NOTHING",
            2, NULL, values, NULL, NULL, 0);
        ok = r && PQresultStatus(r) == PGRES_COMMAND_OK;
        if (r) PQclear(r);
    }
    if (ok) {
        r = PQexec(db, "COMMIT");
        ok = r && PQresultStatus(r) == PGRES_COMMAND_OK;
        if (r) PQclear(r);
    } else {
        r = PQexec(db, "ROLLBACK");
        if (r) PQclear(r);
    }
    PQfinish(db);
    return ok;
}

/* Atomically claim unused questions so concurrent Render instances cannot
   serve the same bank entry in different quizzes. */
char *storage_take_questions_json(const char *topic, int limit) {
    if (!storage_is_configured() || !topic || limit <= 0 || limit > 15) return NULL;
    PGconn *db = open_database();
    if (!db) return NULL;
    const char *values[2] = { topic, NULL };
    char limit_text[16];
    snprintf(limit_text, sizeof(limit_text), "%d", limit);
    values[1] = limit_text;
    PGresult *r = PQexec(db, "BEGIN");
    int ok = r && PQresultStatus(r) == PGRES_COMMAND_OK;
    if (r) PQclear(r);
    char *copy = NULL;
    if (ok) {
        r = PQexecParams(db,
            "WITH picked AS ("
            " SELECT id FROM quiz_question_bank WHERE topic=$1 AND used_at IS NULL "
            " ORDER BY random() LIMIT $2::int FOR UPDATE SKIP LOCKED"
            "), claimed AS ("
            " UPDATE quiz_question_bank q SET used_at=NOW() FROM picked p "
            " WHERE q.id=p.id RETURNING q.question"
            ") SELECT COALESCE(jsonb_agg(question),'[]'::jsonb)::text FROM claimed",
            2, NULL, values, NULL, NULL, 0);
        ok = r && PQresultStatus(r) == PGRES_TUPLES_OK && PQntuples(r) == 1;
        if (ok) copy = strdup(PQgetvalue(r, 0, 0));
        if (r) PQclear(r);
    }
    if (ok) {
        r = PQexec(db, "COMMIT");
        ok = r && PQresultStatus(r) == PGRES_COMMAND_OK;
        if (r) PQclear(r);
    } else {
        r = PQexec(db, "ROLLBACK");
        if (r) PQclear(r);
    }
    PQfinish(db);
    if (!ok) { free(copy); return NULL; }
    return copy;
}

int storage_question_bank_count(const char *topic, int unused_only) {
    if (!storage_is_configured() || !topic) return -1;
    PGconn *db = open_database();
    if (!db) return -1;
    const char *values[1] = { topic };
    const char *sql = unused_only
        ? "SELECT count(*) FROM quiz_question_bank WHERE topic=$1 AND used_at IS NULL"
        : "SELECT count(*) FROM quiz_question_bank WHERE topic=$1";
    PGresult *r = PQexecParams(db, sql, 1, NULL, values, NULL, NULL, 0);
    int count = -1;
    if (r && PQresultStatus(r) == PGRES_TUPLES_OK && PQntuples(r) == 1)
        count = atoi(PQgetvalue(r, 0, 0));
    if (r) PQclear(r);
    PQfinish(db);
    return count;
}

