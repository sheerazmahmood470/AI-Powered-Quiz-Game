#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <libpq-fe.h>
#include <cjson/cJSON.h>
#include "storage.h"

#define MAX_SEED_BYTES 10000000

static const char *database_url(void) {
    const char *value = getenv("DATABASE_URL");
    return (value && value[0]) ? value : NULL;
}

static const char *supabase_database_url(void) {
    const char *value = getenv("SUPABASE_DATABASE_URL");
    return (value && value[0]) ? value : NULL;
}

static PGconn *open_supabase_database(void) {
    const char *url = supabase_database_url();
    if (!url) return NULL;
    PGconn *db = PQconnectdb(url);
    if (!db || PQstatus(db) != CONNECTION_OK) {
        if (db) PQfinish(db);
        return NULL;
    }
    return db;
}

/* Supabase is a secondary question-bank replica. Neon remains authoritative
   for claims so the two independent databases cannot issue the same row
   concurrently as separate sources of truth. */
static int supabase_exec_questions(PGconn *db, const char *topic, const char *json) {
    if (!db || !topic || !json) return 0;
    const char *values[2] = { topic, json };
    PGresult *r = PQexecParams(db,
        "INSERT INTO public.quiz_question_bank(topic,question_hash,question) "
        "SELECT $1,md5(lower(regexp_replace(item->>'question','[^a-zA-Z0-9]','','g'))),item "
        "FROM jsonb_array_elements($2::jsonb) AS item "
        "WHERE jsonb_typeof(item)='object' AND length(trim(COALESCE(item->>'question','')))>0 "
        "ON CONFLICT(topic,question_hash) DO NOTHING",
        2, NULL, values, NULL, NULL, 0);
    int ok = r && PQresultStatus(r) == PGRES_COMMAND_OK;
    if (r) PQclear(r);
    return ok;
}

static void mirror_questions_to_supabase(const char *topic, const char *json) {
    PGconn *db = open_supabase_database();
    if (!db) {
        if (supabase_database_url())
            fprintf(stderr, "[STORAGE] Supabase question mirror unavailable; Neon remains active.\n");
        return;
    }
    if (!supabase_exec_questions(db, topic, json))
        fprintf(stderr, "[STORAGE] Supabase question mirror insert failed for %s.\n", topic);
    PQfinish(db);
}

static void mirror_consumed_questions_to_supabase(const char *topic, const char *json) {
    PGconn *db = open_supabase_database();
    if (!db) return;
    const char *values[2] = { topic, json };
    PGresult *r = PQexecParams(db,
        "WITH consumed AS ("
        " SELECT md5(lower(regexp_replace(item->>'question','[^a-zA-Z0-9]','','g'))) AS question_hash "
        " FROM jsonb_array_elements($2::jsonb) AS item"
        ") UPDATE public.quiz_question_bank q SET used_at=COALESCE(q.used_at,NOW()) "
        "FROM consumed c WHERE q.topic=$1 AND q.question_hash=c.question_hash",
        2, NULL, values, NULL, NULL, 0);
    if (!r || PQresultStatus(r) != PGRES_COMMAND_OK)
        fprintf(stderr, "[STORAGE] Supabase consumed-question sync failed for %s; Neon remains authoritative.\n", topic);
    if (r) PQclear(r);
    PQfinish(db);
}


/* Reconcile the replica from Neon on startup, including retired hashes. */
static void sync_neon_question_bank_to_supabase(PGconn *neon) {
    if (!neon || !supabase_database_url()) return;
    PGconn *supabase = open_supabase_database();
    if (!supabase) return;
    PGresult *rows = PQexec(neon,
        "SELECT topic,question_hash,question::text,used_at::text,direct_ready "
        "FROM quiz_question_bank ORDER BY id");
    int ok = rows && PQresultStatus(rows) == PGRES_TUPLES_OK;
    if (ok) for (int i = 0; i < PQntuples(rows); i++) {
        const char *values[5] = {
            PQgetvalue(rows,i,0), PQgetvalue(rows,i,1), PQgetvalue(rows,i,2),
            PQgetisnull(rows,i,3) ? NULL : PQgetvalue(rows,i,3), PQgetvalue(rows,i,4)
        };
        PGresult *r = PQexecParams(supabase,
            "INSERT INTO public.quiz_question_bank(topic,question_hash,question,used_at,direct_ready) "
            "VALUES($1,$2,$3::jsonb,$4::timestamptz,$5::boolean) "
            "ON CONFLICT(topic,question_hash) DO UPDATE SET "
            "used_at=COALESCE(public.quiz_question_bank.used_at,EXCLUDED.used_at),"
            "direct_ready=public.quiz_question_bank.direct_ready OR EXCLUDED.direct_ready",
            5,NULL,values,NULL,NULL,0);
        if (!r || PQresultStatus(r) != PGRES_COMMAND_OK) ok = 0;
        if (r) PQclear(r);
        if (!ok) break;
    }
    if (rows) PQclear(rows);
    if (ok) {
        rows = PQexec(neon,"SELECT topic,question_hash,retired_at::text FROM quiz_question_archive");
        ok = rows && PQresultStatus(rows) == PGRES_TUPLES_OK;
        if (ok) for (int i = 0; i < PQntuples(rows); i++) {
            const char *values[3] = {PQgetvalue(rows,i,0),PQgetvalue(rows,i,1),PQgetvalue(rows,i,2)};
            PGresult *r = PQexecParams(supabase,
                "INSERT INTO public.quiz_question_archive(topic,question_hash,retired_at) "
                "VALUES($1,$2,$3::timestamptz) ON CONFLICT(topic,question_hash) DO NOTHING",
                3,NULL,values,NULL,NULL,0);
            if (!r || PQresultStatus(r) != PGRES_COMMAND_OK) ok = 0;
            if (r) PQclear(r);
            if (!ok) break;
        }
        if (rows) PQclear(rows);
    }
    if (!ok) fprintf(stderr,"[STORAGE] Startup Neon-to-Supabase question reconciliation was incomplete.\n");
    else fprintf(stderr,"[STORAGE] Supabase question replica reconciled from Neon.\n");
    PQfinish(supabase);
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


static int seed_question_file(PGconn *db, const char *topic, const char *path) {
    char *data = read_seed_file(path);
    if (!data) return 0;
    cJSON *root = cJSON_Parse(data);
    free(data);
    cJSON *questions = root ? cJSON_GetObjectItemCaseSensitive(root, "questions") : NULL;
    char *array = cJSON_IsArray(questions) ? cJSON_PrintUnformatted(questions) : NULL;
    cJSON_Delete(root);
    if (!array) return 1; /* Missing/invalid optional seed is not a database failure. */
    const char *values[2] = { topic, array };
    PGresult *r = PQexecParams(db,
        "INSERT INTO quiz_question_bank(topic,question_hash,question) "
        "SELECT $1,md5(lower(regexp_replace(item->>'question','[^a-zA-Z0-9]','','g'))),item "
        "FROM jsonb_array_elements($2::jsonb) AS item "
        "WHERE jsonb_typeof(item)='object' AND length(trim(COALESCE(item->>'question','')))>0 "
        "AND NOT EXISTS (SELECT 1 FROM quiz_question_archive a WHERE a.topic=$1 "
        "AND a.question_hash=md5(lower(regexp_replace(item->>'question','[^a-zA-Z0-9]','','g')))) "
        "ON CONFLICT(topic,question_hash) DO NOTHING",
        2, NULL, values, NULL, NULL, 0);
    free(array);
    int ok = r && PQresultStatus(r) == PGRES_COMMAND_OK;
    if (r) PQclear(r);
    return ok;
}

int storage_init(void) {
    if (!storage_is_configured()) {
        const char *required = getenv("QUIZ_REQUIRE_DATABASE");
        if (required && (!strcmp(required, "1") || !strcmp(required, "true") || !strcmp(required, "yes"))) {
            fprintf(stderr, "[STORAGE] DATABASE_URL is required but is not configured. Refusing temporary storage.\n");
            return 0;
        }
        fprintf(stderr, "[STORAGE] Neon not configured; using local development storage.\n");
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
        "question JSONB NOT NULL,created_at TIMESTAMPTZ NOT NULL DEFAULT NOW(),used_at TIMESTAMPTZ,"
        "direct_ready BOOLEAN NOT NULL DEFAULT FALSE);"
        "ALTER TABLE quiz_question_bank ADD COLUMN IF NOT EXISTS direct_ready BOOLEAN NOT NULL DEFAULT FALSE;"
        "CREATE UNIQUE INDEX IF NOT EXISTS quiz_question_bank_unique_idx ON quiz_question_bank(topic,question_hash);"
        "CREATE INDEX IF NOT EXISTS quiz_question_bank_pick_idx ON quiz_question_bank(topic,used_at,id);"
        "CREATE TABLE IF NOT EXISTS quiz_question_archive("
        "topic TEXT NOT NULL,question_hash TEXT NOT NULL,retired_at TIMESTAMPTZ NOT NULL DEFAULT NOW(),"
        "PRIMARY KEY(topic,question_hash));"
        "CREATE TABLE IF NOT EXISTS quiz_question_refill_debt("
        "topic TEXT PRIMARY KEY,pending_count INTEGER NOT NULL DEFAULT 0 CHECK(pending_count>=0),"
        "updated_at TIMESTAMPTZ NOT NULL DEFAULT NOW())"
);
    int ok = r && PQresultStatus(r) == PGRES_COMMAND_OK;
    if (r) PQclear(r);

    /* Provision the dedicated question tables in Supabase when configured.
       Supabase errors are non-fatal: Neon remains the authoritative store. */
    if (supabase_database_url()) {
        PGconn *supabase = open_supabase_database();
        if (supabase) {
            PGresult *sr = PQexec(supabase,
                "CREATE TABLE IF NOT EXISTS public.quiz_question_bank("
                "id BIGINT GENERATED BY DEFAULT AS IDENTITY PRIMARY KEY,"
                "topic TEXT NOT NULL,question_hash TEXT NOT NULL,question JSONB NOT NULL,"
                "created_at TIMESTAMPTZ NOT NULL DEFAULT NOW(),used_at TIMESTAMPTZ,"
                "direct_ready BOOLEAN NOT NULL DEFAULT FALSE,"
                "CONSTRAINT quiz_question_bank_topic_hash_key UNIQUE(topic,question_hash));"
                "CREATE TABLE IF NOT EXISTS public.quiz_question_archive("
                "topic TEXT NOT NULL,question_hash TEXT NOT NULL,retired_at TIMESTAMPTZ NOT NULL DEFAULT NOW(),"
                "PRIMARY KEY(topic,question_hash));"
                "CREATE TABLE IF NOT EXISTS public.quiz_question_refill_debt("
                "topic TEXT PRIMARY KEY,pending_count INTEGER NOT NULL DEFAULT 0 CHECK(pending_count>=0),"
                "updated_at TIMESTAMPTZ NOT NULL DEFAULT NOW());"
                "CREATE TABLE IF NOT EXISTS public.quiz_question_refill_state("
                "topic TEXT PRIMARY KEY,target_count INTEGER NOT NULL DEFAULT 5000,"
                "last_checked_at TIMESTAMPTZ,last_success_at TIMESTAMPTZ,last_error TEXT,"
                "updated_at TIMESTAMPTZ NOT NULL DEFAULT NOW());"
                "CREATE INDEX IF NOT EXISTS quiz_question_bank_pick_idx "
                "ON public.quiz_question_bank(topic,used_at,id);");
            if (!sr || PQresultStatus(sr) != PGRES_COMMAND_OK)
                fprintf(stderr, "[STORAGE] Supabase question schema setup failed; continuing with Neon.\n");
            if (sr) PQclear(sr);
            PQfinish(supabase);
        } else {
            fprintf(stderr, "[STORAGE] Supabase connection unavailable; continuing with Neon.\n");
        }
    }

    if (ok) sync_neon_question_bank_to_supabase(db);

    if (ok) ok = seed_key(db, "history", "history.json");
    if (ok) ok = seed_key(db, "reviews", "reviews.json");
    /* Seed only question content into its own table. Never import quiz history. */
    if (ok) ok = seed_question_file(db, "Philosophy", "questions/philosophy.json");
    if (ok) ok = seed_question_file(db, "AI and ML", "questions/ai_ml.json");
    if (ok) ok = seed_question_file(db, "History", "questions/history.json");
    if (ok) ok = seed_question_file(db, "C programming", "questions/c_programming.json");
    if (ok) ok = seed_question_file(db, "Finance", "questions/finance.json");

    /* Mirror question seed files only; history and reviews stay exclusively in Neon. */
    if (supabase_database_url()) {
        PGconn *supabase = open_supabase_database();
        if (supabase) {
            const char *topics[] = {"Philosophy","AI and ML","History","C programming","Finance"};
            const char *paths[] = {"questions/philosophy.json","questions/ai_ml.json","questions/history.json","questions/c_programming.json","questions/finance.json"};
            for (int i = 0; i < 5; i++) {
                char *data = read_seed_file(paths[i]);
                cJSON *root = data ? cJSON_Parse(data) : NULL;
                cJSON *questions = root ? cJSON_GetObjectItemCaseSensitive(root,"questions") : NULL;
                char *array = cJSON_IsArray(questions) ? cJSON_PrintUnformatted(questions) : NULL;
                if (array && !supabase_exec_questions(supabase, topics[i], array))
                    fprintf(stderr, "[STORAGE] Supabase seed mirror failed for %s.\n", topics[i]);
                free(array); cJSON_Delete(root); free(data);
            }
            PQfinish(supabase);
        }
    }

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
    if (ok) fprintf(stderr, "[STORAGE] Neon PostgreSQL connected; quiz history and reviews are persistent.\n");
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
    if (ok) mirror_questions_to_supabase(topic, questions_json);
    return ok;
}

/* Insert background-generated questions into the normal Neon pool and
   return the exact number of newly inserted rows (duplicates are not counted). */
int storage_add_questions_count_json(const char *topic, const char *questions_json) {
    if (!storage_is_configured() || !topic || !questions_json) return 0;
    PGconn *db = open_database();
    if (!db) return 0;
    const char *values[2] = { topic, questions_json };
    PGresult *r = PQexec(db, "BEGIN");
    int ok = r && PQresultStatus(r) == PGRES_COMMAND_OK;
    if (r) PQclear(r);
    int inserted = 0;
    if (ok) {
        r = PQexecParams(db,
            "INSERT INTO quiz_question_bank(topic,question_hash,question) "
            "SELECT $1,md5(lower(regexp_replace(item->>'question','[^a-zA-Z0-9]','','g'))),item "
            "FROM jsonb_array_elements($2::jsonb) AS item "
            "WHERE jsonb_typeof(item)='object' AND length(trim(COALESCE(item->>'question','')))>0 "
            "AND NOT EXISTS (SELECT 1 FROM quiz_question_archive a WHERE a.topic=$1 "
            "AND a.question_hash=md5(lower(regexp_replace(item->>'question','[^a-zA-Z0-9]','','g')))) "
            "ON CONFLICT(topic,question_hash) DO NOTHING",
            2, NULL, values, NULL, NULL, 0);
        ok = r && PQresultStatus(r) == PGRES_COMMAND_OK;
        if (ok) inserted = atoi(PQcmdTuples(r));
        if (r) PQclear(r);
    }
    if (ok && inserted > 0) {
        char inserted_text[16];
        snprintf(inserted_text, sizeof(inserted_text), "%d", inserted);
        const char *debt_values[2] = { topic, inserted_text };
        r = PQexecParams(db,
            "UPDATE quiz_question_refill_debt "
            "SET pending_count=GREATEST(0,pending_count-$2::integer),updated_at=NOW() "
            "WHERE topic=$1",
            2, NULL, debt_values, NULL, NULL, 0);
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
    if (ok && inserted > 0) mirror_questions_to_supabase(topic, questions_json);
    return ok ? inserted : 0;
}

int storage_question_refill_debt(const char *topic) {
    if (!storage_is_configured() || !topic) return -1;
    PGconn *db = open_database();
    if (!db) return -1;
    const char *values[1] = { topic };
    PGresult *r = PQexecParams(db,
        "SELECT pending_count FROM quiz_question_refill_debt WHERE topic=$1",
        1, NULL, values, NULL, NULL, 0);
    int count = 0;
    if (r && PQresultStatus(r) == PGRES_TUPLES_OK && PQntuples(r) == 1)
        count = atoi(PQgetvalue(r, 0, 0));
    else if (!r || PQresultStatus(r) != PGRES_TUPLES_OK)
        count = -1;
    if (r) PQclear(r);
    PQfinish(db);
    return count;
}

/* Store API questions reserved for direct delivery to a future quiz. */
int storage_add_direct_questions_json(const char *topic, const char *questions_json) {
    if (!storage_is_configured() || !topic || !questions_json) return 0;
    PGconn *db = open_database();
    if (!db) return 0;
    const char *values[2] = { topic, questions_json };
    PGresult *r = PQexecParams(db,
        "INSERT INTO quiz_question_bank(topic,question_hash,question,direct_ready) "
        "SELECT $1,md5(lower(regexp_replace(item->>'question','[^a-zA-Z0-9]','','g'))),item,TRUE "
        "FROM jsonb_array_elements($2::jsonb) AS item "
        "WHERE jsonb_typeof(item)='object' AND length(trim(COALESCE(item->>'question','')))>0 "
        "ON CONFLICT(topic,question_hash) DO NOTHING",
        2, NULL, values, NULL, NULL, 0);
    int ok = r && PQresultStatus(r) == PGRES_COMMAND_OK;
    if (r) PQclear(r);
    PQfinish(db);
    return ok;
}

/* Atomically claim the persistent direct-delivery queue. */
char *storage_take_direct_questions_json(const char *topic, int limit) {
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
            " SELECT id FROM quiz_question_bank WHERE topic=$1 AND used_at IS NULL AND direct_ready=TRUE "
            " ORDER BY id LIMIT $2::int FOR UPDATE SKIP LOCKED"
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

/* Atomically CUT questions out of the active Neon bank.
   DELETE ... RETURNING makes delivery and removal one transaction: once a
   quiz receives a question, that row no longer exists in this topic's bank. */
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
            "), removed AS ("
            " DELETE FROM quiz_question_bank q USING picked p "
            " WHERE q.id=p.id RETURNING q.topic,q.question_hash,q.question"
            "), archived AS ("
            " INSERT INTO quiz_question_archive(topic,question_hash) "
            " SELECT topic,question_hash FROM removed ON CONFLICT(topic,question_hash) DO NOTHING "
            " RETURNING question_hash"
            "), debt AS ("
            " INSERT INTO quiz_question_refill_debt(topic,pending_count,updated_at) "
            " SELECT $1,count(*)::integer,NOW() FROM removed HAVING count(*)>0 "
            " ON CONFLICT(topic) DO UPDATE SET pending_count=quiz_question_refill_debt.pending_count+EXCLUDED.pending_count,updated_at=NOW()"
            ") SELECT COALESCE(jsonb_agg(question),'[]'::jsonb)::text FROM removed",
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
    if (copy) mirror_consumed_questions_to_supabase(topic, copy);
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


int storage_reset_question_usage(const char *topic) {
    if (!storage_is_configured() || !topic) return 0;
    PGconn *db = open_database();
    if (!db) return 0;
    const char *values[1] = { topic };
    PGresult *r = PQexecParams(db,
        "UPDATE quiz_question_bank SET used_at=NULL WHERE topic=$1",
        1, NULL, values, NULL, NULL, 0);
    int ok = r && PQresultStatus(r) == PGRES_COMMAND_OK;
    if (r) PQclear(r);
    PQfinish(db);
    return ok;
}

int storage_mark_questions_used_json(const char *topic, const char *questions_json) {
    if (!storage_is_configured() || !topic || !questions_json) return 0;
    PGconn *db = open_database();
    if (!db) return 0;
    const char *values[2] = { topic, questions_json };
    PGresult *r = PQexecParams(db,
        "UPDATE quiz_question_bank q SET used_at=NOW() "
        "FROM jsonb_array_elements($2::jsonb) AS item "
        "WHERE q.topic=$1 AND q.question_hash=md5(lower(regexp_replace(item->>'question','[^a-zA-Z0-9]','','g')))",
        2, NULL, values, NULL, NULL, 0);
    int ok = r && PQresultStatus(r) == PGRES_COMMAND_OK;
    if (r) PQclear(r);
    PQfinish(db);
    return ok;
}

/* Insert only never-before-seen questions and claim them in the same
   transaction. This prevents a generated question from being repeated in
   another quiz, including when multiple server instances run concurrently. */
char *storage_add_claim_questions_json(const char *topic, const char *questions_json) {
    if (!storage_is_configured() || !topic || !questions_json) return NULL;
    PGconn *db = open_database();
    if (!db) return NULL;
    const char *values[2] = { topic, questions_json };
    PGresult *r = PQexec(db, "BEGIN");
    int ok = r && PQresultStatus(r) == PGRES_COMMAND_OK;
    if (r) PQclear(r);
    char *copy = NULL;
    if (ok) {
        r = PQexecParams(db,
            "WITH inserted AS ("
            " INSERT INTO quiz_question_bank(topic,question_hash,question,used_at) "
            " SELECT $1,md5(lower(regexp_replace(item->>'question','[^a-zA-Z0-9]','','g'))),item,NOW() "
            " FROM jsonb_array_elements($2::jsonb) AS item "
            " WHERE jsonb_typeof(item)='object' AND length(trim(COALESCE(item->>'question','')))>0 "
            " ON CONFLICT(topic,question_hash) DO NOTHING RETURNING question"
            ") SELECT COALESCE(jsonb_agg(question),'[]'::jsonb)::text FROM inserted",
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
