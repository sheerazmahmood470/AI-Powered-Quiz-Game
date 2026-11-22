#define _POSIX_C_SOURCE 200809L
#include <stdio
 if(ok) {
  PGresult *m=PQexec(db,"SELECT 1 FROM quiz_game_meta WHERE meta_key='history_rows_migrated'");
  int done=m&&PQresultStatus(m)==PGRES_TUPLES_OK&&PQntuples(m)>0;
  if(m)PQclear(m);
  if(!done) {
   PGresult *legacy=PQexec(db,"SELECT payload::text FROM quiz_game_store WHERE data_key='history'");
   if(!legacy||PQresultStatus(legacy)!=PGRES_TUPLES_OK) ok=0;
   else {
    if(PQntuples(legacy)==1) {
     const char *v[1]={PQgetvalue(legacy,0,0)};
     PGresult *ins=PQexecParams(db,
      "INSERT INTO quiz_history(record,created_at) "
      "SELECT x.record,COALESCE(NULLIF(x.record->>'timestamp','')::bigint,extract(epoch from now())::bigint) "
      "FROM jsonb_array_elements($1::jsonb) WITH ORDINALITY AS x(record,n) ORDER BY x.n",
      1,NULL,v,NULL,NULL,0);
     if(!ins||PQresultStatus(ins)!=PGRES_COMMAND_OK) ok=0;
     if(ins)PQclear(ins);
    }
    PQclear(legacy);
   }
   if(ok) {
    PGresult *mark=PQexec(db,"INSERT INTO quiz_game_meta(meta_key,meta_value) VALUES('history_rows_migrated','yes') ON CONFLICT(meta_key) DO NOTHING");
    ok=mark&&PQresultStatus(mark)==PGRES_COMMAND_OK;if(mark)PQclear(mark);
   }
  }
 }
.h>
#include <stdlib.h>
#include <string.h>
#include <libpq-fe.h>
#include "storage.h"
#define MAX_SEED_BYTES 10000000
static const char *database_url(void){const char *v=getenv("DATABASE_URL");return(v&&v[0])?v:NULL;}
int storage_is_configured(void){return database_url()!=NULL;}
static PGconn *open_database(void){
 const char *url=database_url();if(!url)return NULL;
 PGconn *db=PQconnectdb(url);
 if(!db||PQstatus(db)!=CONNECTION_OK){if(db)PQfinish(db);return NULL;}return db;
}
static char *read_seed_file(const char *path){
 FILE *f=fopen(path,"rb");if(!f)return strdup("[]");
 if(fseek(f,0,SEEK_END)!=0){fclose(f);return NULL;}long n=ftell(f);
 if(n<0||n>MAX_SEED_BYTES||fseek(f,0,SEEK_SET)!=0){fclose(f);return NULL;}
 char *b=malloc((size_t)n+1);if(!b){fclose(f);return NULL;}
 size_t got=fread(b,1,(size_t)n,f);fclose(f);if(got!=(size_t)n){free(b);return NULL;}b[got]='\0';return b;
}
static int seed_key(PGconn *db,const char *key,const char *path){
 char *seed=read_seed_file(path);if(!seed)return 0;const char *v[2]={key,seed};
 PGresult *r=PQexecParams(db,"INSERT INTO quiz_game_store(data_key,payload) VALUES($1,$2::jsonb) ON CONFLICT(data_key) DO NOTHING",2,NULL,v,NULL,NULL,0);
 free(seed);int ok=r&&PQresultStatus(r)==PGRES_COMMAND_OK;if(r)PQclear(r);return ok;
}
int storage_init(void){
 if(!storage_is_configured())return 1;PGconn *db=open_database();
 if(!db){fprintf(stderr,"[STORAGE] PostgreSQL connection failed. Check DATABASE_URL and Neon status.\n");return 0;}
 PGresult *r=PQexec(db,"CREATE TABLE IF NOT EXISTS quiz_game_store(data_key TEXT PRIMARY KEY,payload JSONB NOT NULL,updated_at TIMESTAMPTZ NOT NULL DEFAULT NOW())");
 int ok=r&&PQresultStatus(r)==PGRES_COMMAND_OK;if(r)PQclear(r);
 if(ok) {
  r=PQexec(db,"CREATE TABLE IF NOT EXISTS quiz_game_meta(meta_key TEXT PRIMARY KEY,meta_value TEXT NOT NULL);"
               "CREATE TABLE IF NOT EXISTS quiz_history(id BIGSERIAL PRIMARY KEY,record JSONB NOT NULL,created_at BIGINT NOT NULL);"
               "CREATE INDEX IF NOT EXISTS quiz_history_fifo_idx ON quiz_history(created_at,id)");
  ok=r&&PQresultStatus(r)==PGRES_COMMAND_OK;if(r)PQclear(r);
 }
 if(ok)ok=seed_key(db,"history","history.json");if(ok)ok=seed_key(db,"reviews","reviews.json");
 if(!ok)fprintf(stderr,"[STORAGE] PostgreSQL schema or initial data setup failed.\n");PQfinish(db);return ok;
}
char *storage_read_json(const char *key){
 if(!storage_is_configured()||!key)return NULL;PGconn *db=open_database();if(!db)return NULL;const char *v[1]={key};
 PGresult *r=PQexecParams(db,"SELECT payload::text FROM quiz_game_store WHERE data_key=$1",1,NULL,v,NULL,NULL,0);char *copy=NULL;
 if(r&&PQresultStatus(r)==PGRES_TUPLES_OK&&PQntuples(r)==1&&!PQgetisnull(r,0,0))copy=strdup(PQgetvalue(r,0,0));
 if(r)PQclear(r);PQfinish(db);return copy;
}
int storage_write_json(const char *key,const char *json){
 if(!storage_is_configured()||!key||!json)return 0;PGconn *db=open_database();if(!db)return 0;const char *v[2]={key,json};
 PGresult *r=PQexecParams(db,"INSERT INTO quiz_game_store(data_key,payload,updated_at) VALUES($1,$2::jsonb,NOW()) ON CONFLICT(data_key) DO UPDATE SET payload=EXCLUDED.payload,updated_at=NOW()",2,NULL,v,NULL,NULL,0);
 int ok=r&&PQresultStatus(r)==PGRES_COMMAND_OK;if(r)PQclear(r);PQfinish(db);return ok;
}


char *storage_read_history_json(void) {
 if(!storage_is_configured())return NULL;
 PGconn *db=open_database();if(!db)return NULL;
 PGresult *r=PQexec(db,"SELECT COALESCE(jsonb_agg(record ORDER BY id),'[]'::jsonb)::text FROM (SELECT id,record FROM quiz_history ORDER BY id DESC LIMIT 5000) q");
 char *copy=NULL;
 if(r&&PQresultStatus(r)==PGRES_TUPLES_OK&&PQntuples(r)==1)copy=strdup(PQgetvalue(r,0,0));
 if(r)PQclear(r);PQfinish(db);return copy;
}

/* Appends atomically; a database-wide advisory lock makes FIFO cleanup safe
   across simultaneous quiz completions. Budget is logical live-row bytes. */
int storage_append_history_json(const char *json) {
 if(!storage_is_configured()||!json)return 0;
 PGconn *db=open_database();if(!db)return 0;
 long long limit=650LL*1024LL*1024LL;
 const char *env=getenv("QUIZ_HISTORY_MAX_BYTES");
 if(env&&*env){char *end=NULL;long long n=strtoll(env,&end,10);if(end&&!*end&&n>=1048576)limit=n;}
 char lim[32];snprintf(lim,sizeof(lim),"%lld",limit);
 const char *vals[2]={json,lim};
 PGresult *r=PQexec(db,"BEGIN");int ok=r&&PQresultStatus(r)==PGRES_COMMAND_OK;if(r)PQclear(r);
 if(ok){r=PQexec(db,"SELECT pg_advisory_xact_lock(731942061)");ok=r&&PQresultStatus(r)==PGRES_TUPLES_OK;if(r)PQclear(r);}
 if(ok){r=PQexecParams(db,"INSERT INTO quiz_history(record,created_at) VALUES($1::jsonb,COALESCE(NULLIF(($1::jsonb)->>'timestamp','')::bigint,extract(epoch from now())::bigint))",1,NULL,vals,NULL,NULL,0);ok=r&&PQresultStatus(r)==PGRES_COMMAND_OK;if(r)PQclear(r);}
 if(ok){r=PQexecParams(db,
  "WITH ranked AS (SELECT id,SUM(pg_column_size(record)::bigint+64) OVER (ORDER BY created_at DESC,id DESC) AS keep_bytes FROM quiz_history),"
  "expired AS (SELECT id FROM ranked WHERE keep_bytes>$1::bigint) DELETE FROM quiz_history h USING expired e WHERE h.id=e.id",
  1,NULL,&vals[1],NULL,NULL,0);ok=r&&PQresultStatus(r)==PGRES_COMMAND_OK;if(r)PQclear(r);}
 if(ok){r=PQexec(db,"COMMIT");ok=r&&PQresultStatus(r)==PGRES_COMMAND_OK;if(r)PQclear(r);}
 else {r=PQexec(db,"ROLLBACK");if(r)PQclear(r);}
 if(!ok)fprintf(stderr,"[STORAGE] Neon quiz append/FIFO cleanup failed.\\n");
 PQfinish(db);return ok;
}
