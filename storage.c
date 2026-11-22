#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
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
